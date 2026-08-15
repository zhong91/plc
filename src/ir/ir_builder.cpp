#include "ir/ir_builder.h"
#include "semantic/semantic_checker.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <unordered_map>

namespace toycc {

static int labelCounter = 0;
static std::set<std::string> globalNames;
static std::unordered_map<std::string, int32_t> globalConstValues;
static std::map<std::string, int> functionParamCounts;

// 循环栈：用于 break/continue 的标签
static std::vector<std::pair<std::string, std::string>> loopStack;
// 当前函数的统一返回标签（函数体内多处 return 都跳这里，然后走统一的尾声）
static std::string currentReturnLabel;

// 当前使用的语义检查器
static const SemanticChecker* currentChecker = nullptr;

// ====================== 函数级上下文（解决嵌套作用域 & 栈槽分配） ======================
struct FuncContext {
    // 作用域栈：每个作用域一个 name -> offset 映射
    // 进入 Block push，离开 Block pop；变量查找从顶查到底，天然支持同名遮蔽
    std::vector<std::unordered_map<std::string, int>> scopes;
    // Same-depth compile-time constants. A non-const declaration in scopes shadows
    // an outer constant, so lookup must consult both structures together.
    std::vector<std::unordered_map<std::string, int32_t>> constScopes;

    // 下一个可用“逻辑局部偏移”（局部变量 + 参数副本 + 临时 spill 共用）
    int nextOffset = 0;

    // 当前函数调用其它函数时，第 9+ 个实参所需的最大 outgoing 参数区。
    int maxOutgoingArgBytes = 0;
};

static FuncContext* fctx = nullptr;  // 当前函数上下文（build 时设置）

// 分配一个新的栈槽（4 字节）
static int allocSlot() {
    int off = fctx->nextOffset;
    fctx->nextOffset += 4;
    return off;
}

// 进入一个作用域
static void scopePush() {
    fctx->scopes.emplace_back();
    fctx->constScopes.emplace_back();
}

// 退出一个作用域（槽位暂不重用，保证实现简单且安全）
static void scopePop() {
    fctx->scopes.pop_back();
    fctx->constScopes.pop_back();
}

// 在当前（最内）作用域声明一个变量，分配新栈槽
static int declareVar(const std::string& name) {
    int off = allocSlot();
    fctx->scopes.back().emplace(name, off);
    return off;
}

static int declareConst(const std::string& name, int32_t value) {
    int off = declareVar(name);
    fctx->constScopes.back()[name] = value;
    return off;
}

static bool tryLookupLocalConst(const std::string& name, int32_t& value) {
    if (!fctx) return false;
    for (size_t depth = fctx->scopes.size(); depth-- > 0;) {
        auto decl = fctx->scopes[depth].find(name);
        if (decl == fctx->scopes[depth].end()) continue;
        auto c = fctx->constScopes[depth].find(name);
        if (c == fctx->constScopes[depth].end()) return false;
        value = c->second;
        return true;
    }
    return false;
}

// 尝试从内到外查找局部变量。返回 true 表示局部/参数存在。
static bool tryLookupVar(const std::string& name, int& offset) {
    if (!fctx) return false;
    for (auto it = fctx->scopes.rbegin(); it != fctx->scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) {
            offset = f->second;
            return true;
        }
    }
    return false;
}

// 从内到外查找变量偏移；不存在则报错
static int lookupVar(const std::string& name) {
    int offset = 0;
    if (tryLookupVar(name, offset)) return offset;
    throw std::runtime_error("No offset allocated for variable '" + name + "'");
}
// ====================================================================================

std::string newLabel() { return ".L" + std::to_string(labelCounter++); }

void buildStmt(const StmtPtr& stmt, IRFunction& irFunc);

// 判断是否为全局变量
bool isGlobalVar(const std::string& name) {
    if (currentChecker) {
        return currentChecker->isGlobal(name);
    }
    return globalNames.find(name) != globalNames.end();
}

// 获取局部变量的栈偏移
int getVarOffset(const std::string& name) {
    return lookupVar(name);
}

// 判断是否为常量变量
bool isConstantVar(const std::string& name) {
    if (currentChecker) {
        return currentChecker->isConstant(name);
    }
    return false;
}

// 获取常量值
int getConstValue(const std::string& name) {
    if (currentChecker) {
        auto val = currentChecker->getConstValue(name);
        if (val.has_value()) {
            return val.value();
        }
        throw std::runtime_error("Constant '" + name + "' has no value");
    }
    throw std::runtime_error("Cannot get const value in test mode");
}

static int32_t evalConstExprForIR(const ExprPtr& expr) {
    if (auto num = std::dynamic_pointer_cast<NumberLiteral>(expr)) {
        return static_cast<int32_t>(num->value);
    }
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        int32_t v = 0;
        if (tryLookupLocalConst(var->name, v)) return v;
        auto g = globalConstValues.find(var->name);
        if (g != globalConstValues.end()) return g->second;
        throw std::runtime_error("Non-constant identifier '" + var->name + "' in const initializer");
    }
    if (auto un = std::dynamic_pointer_cast<UnaryExpr>(expr)) {
        int32_t a = evalConstExprForIR(un->operand);
        if (un->op == "+") return a;
        if (un->op == "-") return static_cast<int32_t>(0u - static_cast<uint32_t>(a));
        if (un->op == "!") return a == 0 ? 1 : 0;
    }
    if (auto bin = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        int32_t a = evalConstExprForIR(bin->left);
        if (bin->op == "&&") return a == 0 ? 0 : (evalConstExprForIR(bin->right) != 0 ? 1 : 0);
        if (bin->op == "||") return a != 0 ? 1 : (evalConstExprForIR(bin->right) != 0 ? 1 : 0);
        int32_t b = evalConstExprForIR(bin->right);
        if (bin->op == "+") return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
        if (bin->op == "-") return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
        if (bin->op == "*") return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
        if (bin->op == "/") return static_cast<int32_t>(a / b);
        if (bin->op == "%") return static_cast<int32_t>(a % b);
        if (bin->op == "<") return a < b ? 1 : 0;
        if (bin->op == ">") return a > b ? 1 : 0;
        if (bin->op == "<=") return a <= b ? 1 : 0;
        if (bin->op == ">=") return a >= b ? 1 : 0;
        if (bin->op == "==") return a == b ? 1 : 0;
        if (bin->op == "!=") return a != b ? 1 : 0;
    }
    throw std::runtime_error("Unsupported constant expression in IR builder");
}

// =============================== 表达式生成 ===============================
// 所有表达式约定：计算结果存在 t0 中。需要跨子表达式保存的值放入栈槽。
void buildExpr(const ExprPtr& expr, IRFunction& irFunc) {
    // ------- 数字字面量 -------
    if (auto num = std::dynamic_pointer_cast<NumberLiteral>(expr)) {
        irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(num->value));
        return;
    }

    // ------- 变量引用 -------
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        // 必须局部优先：语义检查器的 isGlobal(name) 只说明全局表里有同名符号，
        // 不能覆盖当前作用域中的局部变量/参数。
        int localOffset = 0;
        if (tryLookupVar(var->name, localOffset)) {
            int32_t constValue = 0;
            if (tryLookupLocalConst(var->name, constValue))
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(constValue));
            else
                irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(localOffset));
        } else if (auto c = globalConstValues.find(var->name); c != globalConstValues.end()) {
            irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(c->second));
        } else if (isGlobalVar(var->name)) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD_GLOBAL, "t0", var->name);
        } else {
            int offset = getVarOffset(var->name); // 保留原有错误信息
            irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(offset));
        }
        return;
    }

    // ------- 函数调用 -------
    if (auto call = std::dynamic_pointer_cast<FunctionCall>(expr)) {
        // Step 1: 先求出所有实参并保存到本函数的逻辑局部槽。
        // 这样后续实参中的嵌套调用不会破坏已计算的参数值。
        size_t n = call->args.size();
        std::vector<int> argSlots;
        argSlots.reserve(n);

        for (size_t i = 0; i < n; ++i) {
            buildExpr(call->args[i], irFunc); // 结果在 t0
            int slot = allocSlot();
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(slot));
            argSlots.push_back(slot);
        }

        // 第 9 个及之后的参数按照 RISC-V ABI 放到调用者栈上。
        int extraArgBytes = n > 8 ? static_cast<int>((n - 8) * 4) : 0;
        fctx->maxOutgoingArgBytes = std::max(fctx->maxOutgoingArgBytes, extraArgBytes);

        // Step 2a: 前 8 个参数进入 a0~a7。
        size_t registerArgs = std::min<size_t>(n, 8);
        for (size_t i = 0; i < registerArgs; ++i) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(argSlots[i]));
            std::string reg = "a" + std::to_string(i);
            irFunc.instrs.emplace_back(IRInstrType::MV, reg, "t0");
        }

        // Step 2b: 第 9+ 个参数进入当前函数预留的 outgoing 参数区。
        for (size_t i = 8; i < n; ++i) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(argSlots[i]));
            int argOffset = static_cast<int>((i - 8) * 4);
            irFunc.instrs.emplace_back(IRInstrType::STORE_ARG, "", "t0", std::to_string(argOffset));
        }

        // Step 3: call，返回值 a0 → t0
        irFunc.instrs.emplace_back(IRInstrType::CALL, "", call->name);
        irFunc.instrs.emplace_back(IRInstrType::MV, "t0", "a0");
        return;
    }

    // ------- 一元表达式 -------
    if (auto un = std::dynamic_pointer_cast<UnaryExpr>(expr)) {
        buildExpr(un->operand, irFunc);
        if (un->op == "-") {
            irFunc.instrs.emplace_back(IRInstrType::LI, "t1", "0");
            irFunc.instrs.emplace_back(IRInstrType::SUB, "t0", "t1", "t0");
        } else if (un->op == "!") {
            irFunc.instrs.emplace_back(IRInstrType::SEQZ, "t0", "t0");
        }
        // "+" 不需要生成代码（值不变）
        return;
    }

    // ------- 二元表达式 -------
    if (auto bin = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        // 逻辑 && / ||：短路求值，并把结果规范化为 0/1。
        if (bin->op == "&&" || bin->op == "||") {
            std::string shortLabel = newLabel();
            std::string endLabel = newLabel();
            buildExpr(bin->left, irFunc);

            if (bin->op == "&&") {
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);
                irFunc.instrs.emplace_back(IRInstrType::SNEZ, "t0", "t0");
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "0");
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            } else {
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);
                irFunc.instrs.emplace_back(IRInstrType::SNEZ, "t0", "t0");
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "1");
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            }
            return;
        }

        // 其他二元运算：左边先 spill，算右边，再把左边 reload 到 t1。
        IRInstrType opType = IRInstrType::ADD;
        bool isArith = (bin->op == "+" || bin->op == "-" || bin->op == "*" ||
                        bin->op == "/" || bin->op == "%");

        if (bin->op == "+") opType = IRInstrType::ADD;
        else if (bin->op == "-") opType = IRInstrType::SUB;
        else if (bin->op == "*") opType = IRInstrType::MUL;
        else if (bin->op == "/") opType = IRInstrType::DIV;
        else if (bin->op == "%") opType = IRInstrType::REM;

        buildExpr(bin->left, irFunc);
        int leftSlot = allocSlot();
        irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(leftSlot));
        buildExpr(bin->right, irFunc);

        irFunc.instrs.emplace_back(IRInstrType::LOAD, "t1", std::to_string(leftSlot));

        if (isArith) {
            irFunc.instrs.emplace_back(opType, "t0", "t1", "t0");
            return;
        }

        // 比较运算
        if (bin->op == "==") {
            irFunc.instrs.emplace_back(IRInstrType::SUB, "t0", "t1", "t0");
            irFunc.instrs.emplace_back(IRInstrType::SEQZ, "t0", "t0");
        } else if (bin->op == "!=") {
            irFunc.instrs.emplace_back(IRInstrType::SUB, "t0", "t1", "t0");
            irFunc.instrs.emplace_back(IRInstrType::SNEZ, "t0", "t0");
        } else if (bin->op == "<") {
            irFunc.instrs.emplace_back(IRInstrType::SLT, "t0", "t1", "t0");
        } else if (bin->op == ">") {
            irFunc.instrs.emplace_back(IRInstrType::SLT, "t0", "t0", "t1");
        } else if (bin->op == "<=") {
            irFunc.instrs.emplace_back(IRInstrType::SLT, "t0", "t0", "t1");
            irFunc.instrs.emplace_back(IRInstrType::SEQZ, "t0", "t0");
        } else if (bin->op == ">=") {
            irFunc.instrs.emplace_back(IRInstrType::SLT, "t0", "t1", "t0");
            irFunc.instrs.emplace_back(IRInstrType::SEQZ, "t0", "t0");
        }
        return;
    }

    throw std::runtime_error("Unknown expression");
}

// =============================== 语句生成 ===============================
void buildStmt(const StmtPtr& stmt, IRFunction& irFunc) {
    if (!stmt) return;

    // 语句块（会新建作用域，支持同名变量屏蔽外层）
    if (auto block = std::dynamic_pointer_cast<Block>(stmt)) {
        scopePush();
        for (const auto& s : block->statements) {
            buildStmt(s, irFunc);
        }
        scopePop();
        return;
    }

    // 变量声明：在当前作用域登记，分配新槽，写入初始化值
    if (auto varDecl = std::dynamic_pointer_cast<VarDeclStmt>(stmt)) {
        int offset = 0;
        if (varDecl->isConst) {
            int32_t value = evalConstExprForIR(varDecl->init);
            offset = declareConst(varDecl->name, value);
            irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(value));
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
        } else {
            offset = declareVar(varDecl->name);
            if (varDecl->init) {
                buildExpr(varDecl->init, irFunc);
                irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
            }
        }
        return;
    }

    // 赋值语句：同样必须局部优先，避免局部变量遮蔽全局变量时写错目标。
    if (auto assign = std::dynamic_pointer_cast<AssignStmt>(stmt)) {
        buildExpr(assign->value, irFunc);

        int localOffset = 0;
        if (tryLookupVar(assign->name, localOffset)) {
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(localOffset));
        } else if (isGlobalVar(assign->name)) {
            irFunc.instrs.emplace_back(IRInstrType::STORE_GLOBAL, "", assign->name, "t0");
        } else {
            int offset = getVarOffset(assign->name);
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
        }
        return;
    }

    // return 语句：把值送入 a0 后跳统一返回标签
    if (auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmt)) {
        if (ret->value) {
            buildExpr(ret->value, irFunc);
            irFunc.instrs.emplace_back(IRInstrType::MV, "a0", "t0");
        }
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", currentReturnLabel);
        return;
    }

    // if 语句
    if (auto ifStmt = std::dynamic_pointer_cast<IfStmt>(stmt)) {
        std::string elseLabel = newLabel();
        std::string endLabel = newLabel();
        buildExpr(ifStmt->condition, irFunc);
        irFunc.instrs.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", elseLabel);
        buildStmt(ifStmt->thenBranch, irFunc);
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", elseLabel);
        if (ifStmt->elseBranch) {
            buildStmt(ifStmt->elseBranch, irFunc);
        }
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
        return;
    }

    // while 语句
    if (auto whileStmt = std::dynamic_pointer_cast<WhileStmt>(stmt)) {
        std::string loopLabel = newLabel();
        std::string endLabel = newLabel();
        loopStack.push_back({loopLabel, endLabel});
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", loopLabel);
        buildExpr(whileStmt->condition, irFunc);
        irFunc.instrs.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", endLabel);
        buildStmt(whileStmt->body, irFunc);
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", loopLabel);
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
        loopStack.pop_back();
        return;
    }

    // break 语句
    if (std::dynamic_pointer_cast<BreakStmt>(stmt)) {
        if (loopStack.empty()) throw std::runtime_error("break outside loop");
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", loopStack.back().second);
        return;
    }

    // continue 语句
    if (std::dynamic_pointer_cast<ContinueStmt>(stmt)) {
        if (loopStack.empty()) throw std::runtime_error("continue outside loop");
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", loopStack.back().first);
        return;
    }

    // 表达式语句
    if (auto exprStmt = std::dynamic_pointer_cast<ExprStmt>(stmt)) {
        buildExpr(exprStmt->expr, irFunc);
        return;
    }
}

// =============================== 总入口 ===============================
IRProgram IRBuilder::build(const ASTNodePtr& root, const SemanticChecker* checker) {
    currentChecker = checker;
    IRProgram program;
    auto compUnit = std::dynamic_pointer_cast<CompUnit>(root);
    if (!compUnit) throw std::runtime_error("Root is not CompUnit");

    globalNames.clear();
    globalConstValues.clear();
    functionParamCounts.clear();

    // 标签只在整个编译单元开始时清零，而不是每个函数清零。
    // 否则不同函数都会生成 .L0/.L1，汇编器会遇到重复符号。
    labelCounter = 0;

    // 第一遍：收集全局变量、常量、函数信息
    for (const auto& unit : compUnit->units) {
        if (auto varDecl = std::dynamic_pointer_cast<VarDeclStmt>(unit)) {
            int initValue = 0;
            if (checker && varDecl->isConst) {
                auto val = checker->getConstValue(varDecl->name);
                if (val.has_value()) {
                    initValue = val.value();
                } else {
                    throw std::runtime_error("Constant '" + varDecl->name + "' has no value in symbol table");
                }
            } else if (auto num = std::dynamic_pointer_cast<NumberLiteral>(varDecl->init)) {
                initValue = num->value;
            } else {
                throw std::runtime_error("Global initializer must be constant");
            }
            program.globalVars.push_back({varDecl->name, initValue});
            globalNames.insert(varDecl->name);
            if (varDecl->isConst) globalConstValues[varDecl->name] = static_cast<int32_t>(initValue);
        }
        if (auto funcDef = std::dynamic_pointer_cast<FunctionDef>(unit)) {
            functionParamCounts[funcDef->name] = static_cast<int>(funcDef->params.size());
        }
    }

    // 第二遍：逐函数生成 IR
    for (const auto& unit : compUnit->units) {
        auto funcDef = std::dynamic_pointer_cast<FunctionDef>(unit);
        if (!funcDef) continue;

        IRFunction irFunc;
        irFunc.name = funcDef->name;
        irFunc.paramCount = static_cast<int>(funcDef->params.size());
        irFunc.isVoid = (funcDef->returnType == ValueType::Void);

        FuncContext ctx;
        fctx = &ctx;
        loopStack.clear();
        currentReturnLabel = ".ret_" + funcDef->name;

        // 进入函数最外层作用域（参数 + 函数体共用）
        scopePush();

        // 参数：前 8 个来自 a0~a7；第 9+ 个来自调用者栈。
        for (size_t i = 0; i < funcDef->params.size(); ++i) {
            int off = declareVar(funcDef->params[i].name);

            if (i < 8) {
                std::string reg = "a" + std::to_string(i);
                irFunc.instrs.emplace_back(IRInstrType::MV, "t0", reg);
            } else {
                int incomingOffset = static_cast<int>((i - 8) * 4);
                irFunc.instrs.emplace_back(IRInstrType::LOAD_ARG, "t0", std::to_string(incomingOffset));
            }

            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(off));
        }

        if (funcDef->body) {
            buildStmt(funcDef->body, irFunc);
        }

        scopePop();

        // 统一返回标签
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", currentReturnLabel);

        // 保存函数布局信息，后端统一决定最终物理偏移。
        irFunc.localSize = ctx.nextOffset;
        irFunc.outgoingArgSize = ctx.maxOutgoingArgBytes;

        fctx = nullptr;
        program.functions.push_back(std::move(irFunc));
    }

    currentChecker = nullptr;
    return program;
}

} // namespace toycc
