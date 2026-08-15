#include "opt/compile_time_evaluator.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toycc {
namespace {

struct EvalAbort final {};

struct Cell {
    std::int32_t value = 0;
    bool isConst = false;
};

struct Frame {
    std::vector<std::unordered_map<std::string, Cell>> scopes;
};

enum class FlowKind {
    Normal,
    Break,
    Continue,
    Return,
    TailSelfCall,
};

struct Flow {
    FlowKind kind = FlowKind::Normal;
    std::int32_t value = 0;
    std::vector<std::int32_t> args;

    static Flow normal() { return {}; }
    static Flow make(FlowKind k) {
        Flow f;
        f.kind = k;
        return f;
    }
    static Flow returned(std::int32_t v) {
        Flow f;
        f.kind = FlowKind::Return;
        f.value = v;
        return f;
    }
    static Flow tail(std::vector<std::int32_t> a) {
        Flow f;
        f.kind = FlowKind::TailSelfCall;
        f.args = std::move(a);
        return f;
    }
};

class Evaluator {
public:
    Evaluator(const ASTNodePtr& root, std::uint64_t stepBudget)
        : root_(root), budget_(stepBudget) {}

    std::optional<std::int32_t> run() {
        try {
            auto comp = std::dynamic_pointer_cast<CompUnit>(root_);
            if (!comp) throw EvalAbort{};

            // 先登记函数，使后续调用可以 O(1) 查找。
            for (const auto& unit : comp->units) {
                if (unit && unit->type == ASTNodeType::Function) {
                    auto fn = std::static_pointer_cast<FunctionDef>(unit);
                    functions_[fn->name] = fn;
                }
            }

            // 按源程序顺序初始化全局对象。
            Frame globalInitFrame;
            for (const auto& unit : comp->units) {
                if (!unit || unit->type != ASTNodeType::VarDeclStmt) continue;
                auto decl = std::static_pointer_cast<VarDeclStmt>(unit);
                std::int32_t value = decl->init ? evalExpr(decl->init, globalInitFrame) : 0;
                globals_[decl->name] = Cell{value, decl->isConst};
            }

            auto result = callFunction("main", {});
            if (!result.has_value()) {
                // 标准 main 应返回 int；若前端未来允许 void main，则保守回退。
                throw EvalAbort{};
            }
            return result;
        } catch (const EvalAbort&) {
            return std::nullopt;
        } catch (const std::exception&) {
            // 优化器绝不能让本来可编译的程序失败；任何内部异常都回退普通后端。
            return std::nullopt;
        }
    }

private:
    ASTNodePtr root_;
    std::uint64_t budget_;
    std::uint64_t steps_ = 0;
    int callDepth_ = 0;
    std::unordered_map<std::string, Cell> globals_;
    std::unordered_map<std::string, std::shared_ptr<FunctionDef>> functions_;

    void tick() {
        if (++steps_ > budget_) throw EvalAbort{};
    }

    static bool truthy(std::int32_t value) { return value != 0; }

    Cell* findLocal(Frame& frame, const std::string& name) {
        for (auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    const Cell* findLocal(const Frame& frame, const std::string& name) const {
        for (auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    }

    std::int32_t readVar(const Frame& frame, const std::string& name) const {
        if (const Cell* local = findLocal(frame, name)) return local->value;
        auto global = globals_.find(name);
        if (global != globals_.end()) return global->second.value;
        throw EvalAbort{};
    }

    void writeVar(Frame& frame, const std::string& name, std::int32_t value) {
        if (Cell* local = findLocal(frame, name)) {
            if (local->isConst) throw EvalAbort{};
            local->value = value;
            return;
        }
        auto global = globals_.find(name);
        if (global != globals_.end()) {
            if (global->second.isConst) throw EvalAbort{};
            global->second.value = value;
            return;
        }
        throw EvalAbort{};
    }

    std::int32_t evalExpr(const ExprPtr& expr, Frame& frame) {
        tick();
        if (!expr) throw EvalAbort{};

        switch (expr->type) {
            case ASTNodeType::NumberLiteral: {
                auto node = std::static_pointer_cast<NumberLiteral>(expr);
                return static_cast<std::int32_t>(node->value);
            }

            case ASTNodeType::Variable: {
                auto node = std::static_pointer_cast<Variable>(expr);
                return readVar(frame, node->name);
            }

            case ASTNodeType::UnaryExpr: {
                auto node = std::static_pointer_cast<UnaryExpr>(expr);
                std::int32_t v = evalExpr(node->operand, frame);
                if (node->op == "+") return v;
                if (node->op == "-") return static_cast<std::int32_t>(-v);
                if (node->op == "!") return truthy(v) ? 0 : 1;
                throw EvalAbort{};
            }

            case ASTNodeType::BinaryExpr: {
                auto node = std::static_pointer_cast<BinaryExpr>(expr);

                // 必须保持短路语义，右值可能包含函数调用和全局变量写操作。
                if (node->op == "&&") {
                    std::int32_t lhs = evalExpr(node->left, frame);
                    if (!truthy(lhs)) return 0;
                    return truthy(evalExpr(node->right, frame)) ? 1 : 0;
                }
                if (node->op == "||") {
                    std::int32_t lhs = evalExpr(node->left, frame);
                    if (truthy(lhs)) return 1;
                    return truthy(evalExpr(node->right, frame)) ? 1 : 0;
                }

                std::int32_t lhs = evalExpr(node->left, frame);
                std::int32_t rhs = evalExpr(node->right, frame);

                if (node->op == "+") return static_cast<std::int32_t>(lhs + rhs);
                if (node->op == "-") return static_cast<std::int32_t>(lhs - rhs);
                if (node->op == "*") return static_cast<std::int32_t>(lhs * rhs);
                if (node->op == "/") {
                    if (rhs == 0) throw EvalAbort{};
                    if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) throw EvalAbort{};
                    return static_cast<std::int32_t>(lhs / rhs);
                }
                if (node->op == "%") {
                    if (rhs == 0) throw EvalAbort{};
                    if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) return 0;
                    return static_cast<std::int32_t>(lhs % rhs);
                }
                if (node->op == "==") return lhs == rhs ? 1 : 0;
                if (node->op == "!=") return lhs != rhs ? 1 : 0;
                if (node->op == "<") return lhs < rhs ? 1 : 0;
                if (node->op == ">") return lhs > rhs ? 1 : 0;
                if (node->op == "<=") return lhs <= rhs ? 1 : 0;
                if (node->op == ">=") return lhs >= rhs ? 1 : 0;
                throw EvalAbort{};
            }

            case ASTNodeType::FunctionCall: {
                auto node = std::static_pointer_cast<FunctionCall>(expr);
                std::vector<std::int32_t> args;
                args.reserve(node->args.size());
                for (const auto& arg : node->args) {
                    args.push_back(evalExpr(arg, frame));
                }
                auto result = callFunction(node->name, args);
                // void 函数作为表达式语句时返回值不会被语义使用，这里用 0 占位。
                return result.value_or(0);
            }

            default:
                throw EvalAbort{};
        }
    }

    Flow execBlock(const std::shared_ptr<Block>& block, Frame& frame,
                   const std::string& currentFunction) {
        tick();
        if (!block) return Flow::normal();

        frame.scopes.emplace_back();
        for (const auto& stmt : block->statements) {
            Flow flow = execStmt(stmt, frame, currentFunction);
            if (flow.kind != FlowKind::Normal) {
                frame.scopes.pop_back();
                return flow;
            }
        }
        frame.scopes.pop_back();
        return Flow::normal();
    }

    Flow execStmt(const StmtPtr& stmt, Frame& frame,
                  const std::string& currentFunction) {
        tick();
        if (!stmt) return Flow::normal();

        switch (stmt->type) {
            case ASTNodeType::Block:
                return execBlock(std::static_pointer_cast<Block>(stmt), frame, currentFunction);

            case ASTNodeType::EmptyStmt:
                return Flow::normal();

            case ASTNodeType::VarDeclStmt: {
                auto node = std::static_pointer_cast<VarDeclStmt>(stmt);
                if (frame.scopes.empty()) frame.scopes.emplace_back();
                // 与当前 IRBuilder 一致：先声明槽位，再计算初始化表达式。
                auto& cell = frame.scopes.back()[node->name];
                cell = Cell{0, node->isConst};
                cell.value = node->init ? evalExpr(node->init, frame) : 0;
                return Flow::normal();
            }

            case ASTNodeType::AssignStmt: {
                auto node = std::static_pointer_cast<AssignStmt>(stmt);
                std::int32_t value = evalExpr(node->value, frame);
                writeVar(frame, node->name, value);
                return Flow::normal();
            }

            case ASTNodeType::ExprStmt: {
                auto node = std::static_pointer_cast<ExprStmt>(stmt);
                if (node->expr) (void)evalExpr(node->expr, frame);
                return Flow::normal();
            }

            case ASTNodeType::IfStmt: {
                auto node = std::static_pointer_cast<IfStmt>(stmt);
                if (truthy(evalExpr(node->condition, frame))) {
                    return execStmt(node->thenBranch, frame, currentFunction);
                }
                if (node->elseBranch) {
                    return execStmt(node->elseBranch, frame, currentFunction);
                }
                return Flow::normal();
            }

            case ASTNodeType::WhileStmt: {
                auto node = std::static_pointer_cast<WhileStmt>(stmt);
                while (truthy(evalExpr(node->condition, frame))) {
                    Flow flow = execStmt(node->body, frame, currentFunction);
                    if (flow.kind == FlowKind::Break) return Flow::normal();
                    if (flow.kind == FlowKind::Continue) continue;
                    if (flow.kind != FlowKind::Normal) return flow;
                }
                return Flow::normal();
            }

            case ASTNodeType::BreakStmt:
                return Flow::make(FlowKind::Break);

            case ASTNodeType::ContinueStmt:
                return Flow::make(FlowKind::Continue);

            case ASTNodeType::ReturnStmt: {
                auto node = std::static_pointer_cast<ReturnStmt>(stmt);
                if (!node->value) return Flow::returned(0);

                // 直接尾递归：return f(...); 在编译期解释时改写成循环，避免宿主栈溢出。
                if (node->value->type == ASTNodeType::FunctionCall) {
                    auto call = std::static_pointer_cast<FunctionCall>(node->value);
                    if (call->name == currentFunction) {
                        std::vector<std::int32_t> args;
                        args.reserve(call->args.size());
                        for (const auto& arg : call->args) {
                            args.push_back(evalExpr(arg, frame));
                        }
                        return Flow::tail(std::move(args));
                    }
                }

                return Flow::returned(evalExpr(node->value, frame));
            }

            default:
                throw EvalAbort{};
        }
    }

    std::optional<std::int32_t> callFunction(
        const std::string& name,
        const std::vector<std::int32_t>& initialArgs) {
        tick();

        // 非尾递归仍会使用宿主 C++ 调用栈。限制深度以保证优化失败时可以安全回退；
        // 直接尾递归由下面的循环消除，不受这个限制。
        if (callDepth_ >= 1024) throw EvalAbort{};
        struct DepthGuard {
            int& depth;
            explicit DepthGuard(int& d) : depth(d) { ++depth; }
            ~DepthGuard() { --depth; }
        } depthGuard(callDepth_);

        auto found = functions_.find(name);
        if (found == functions_.end()) throw EvalAbort{};
        const auto& fn = found->second;
        if (fn->params.size() != initialArgs.size()) throw EvalAbort{};

        std::vector<std::int32_t> args = initialArgs;

        // 循环负责消除直接尾递归。
        for (;;) {
            tick();
            Frame frame;
            frame.scopes.emplace_back(); // 参数作用域
            for (std::size_t i = 0; i < fn->params.size(); ++i) {
                frame.scopes.back()[fn->params[i].name] = Cell{args[i], false};
            }

            Flow flow = execBlock(fn->body, frame, name);
            if (flow.kind == FlowKind::TailSelfCall) {
                if (flow.args.size() != fn->params.size()) throw EvalAbort{};
                args = std::move(flow.args);
                continue;
            }
            if (flow.kind == FlowKind::Return) {
                if (fn->returnType == ValueType::Void) return std::nullopt;
                return flow.value;
            }
            if (flow.kind == FlowKind::Normal && fn->returnType == ValueType::Void) {
                return std::nullopt;
            }
            // 语义检查器应保证 int 函数所有路径 return，走到这里说明优化器未覆盖某种情况。
            throw EvalAbort{};
        }
    }
};

} // namespace

std::optional<std::int32_t> tryEvaluateMainAtCompileTime(
    const ASTNodePtr& root,
    std::uint64_t stepBudget) {
    Evaluator evaluator(root, stepBudget);
    return evaluator.run();
}

void emitConstantMain(std::ostream& out, std::int32_t value) {
    out << ".section .text\n";
    out << ".globl main\n";
    out << "main:\n";
    out << "    li a0, " << value << "\n";
    out << "    ret\n";
}

} // namespace toycc
