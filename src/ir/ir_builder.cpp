#include "ir/ir_builder.h"
#include "semantic/semantic_checker.h"
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

    // 栈指针：下一个可用字节偏移（局部变量 + 参数 + 临时spill区全部共用）
    int nextOffset = 0;
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
}

// 退出一个作用域（弹出，同名局部变量的槽由于是顺序递增分配，不再重用；这是安全的教学实现）
static void scopePop() {
    fctx->scopes.pop_back();
}

// 在当前（最内）作用域声明一个变量，分配新栈槽
static int declareVar(const std::string& name) {
    int off = allocSlot();
    fctx->scopes.back().emplace(name, off);
    return off;
}

// 从内到外查找变量偏移（正确实现内层屏蔽外层）
static int lookupVar(const std::string& name) {
    for (auto it = fctx->scopes.rbegin(); it != fctx->scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
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

// =============================== 表达式生成 ===============================
// 所有表达式约定：计算结果存在 t0 中。计算过程中需要临时保持的值一律存到
// 新分配的栈槽中，绝不再使用固定寄存器 t1 作"持有"用途，避免：
//   (1) 嵌套 BinaryExpr 覆盖 t1
//   (2) 右侧有 function call 时 t1 被 callee 当作临时寄存器破坏

void buildExpr(const ExprPtr& expr, IRFunction& irFunc) {
    // ------- 数字字面量 -------
    if (auto num = std::dynamic_pointer_cast<NumberLiteral>(expr)) {
        irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(num->value));
        return;
    }

    // ------- 变量引用 -------
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        if (isGlobalVar(var->name)) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD_GLOBAL, "t0", var->name);
        } else {
            if (currentChecker && isConstantVar(var->name)) {
                int val = getConstValue(var->name);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(val));
            } else {
                int offset = getVarOffset(var->name);
                irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(offset));
            }
        }
        return;
    }

    // ------- 函数调用 -------
    if (auto call = std::dynamic_pointer_cast<FunctionCall>(expr)) {
        // Step 1: 先把所有实参计算出来，全部存在栈槽里（每个实参 4 字节）
        //         这一步防止：计算第 i+1 个实参时内部函数调用破坏之前已放入 aN 的值
        size_t n = call->args.size();
        std::vector<int> argSlots;
        argSlots.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            buildExpr(call->args[i], irFunc);     // 结果在 t0
            int slot = allocSlot();
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(slot));
            argSlots.push_back(slot);
        }
        // Step 2: 从栈槽把参数依次搬运到 a0~a7
        //         (对于≥9个参数，先不支持，抛清楚的错误，不在此静默截断)
        if (n > 8) {
            throw std::runtime_error(
                "Function call '" + call->name + "' has " + std::to_string(n) +
                " arguments; currently only up to 8 arguments are supported."
            );
        }
        for (size_t i = 0; i < n; ++i) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(argSlots[i]));
            std::string reg = "a" + std::to_string(i);
            irFunc.instrs.emplace_back(IRInstrType::MV, reg, "t0");
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
            irFunc.instrs.emplace_back(IRInstrType::SUB, "t0", "t1", "t0");  // 0 - t0
        } else if (un->op == "!") {
            irFunc.instrs.emplace_back(IRInstrType::SEQZ, "t0", "t0");
        }
        // "+" 不需要生成代码（值不变）
        return;
    }

    // ------- 二元表达式 -------
    if (auto bin = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        // 逻辑 && / || (短路求值) —— 结果必须是 0 或 1 (C 语言规范)
        if (bin->op == "&&" || bin->op == "||") {
            std::string shortLabel = newLabel();
            std::string endLabel = newLabel();
            buildExpr(bin->left, irFunc);
            if (bin->op == "&&") {
                // 左 == 0 → 结果就是 0，跳过右值计算
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);          // 结果 t0 = right 值，可能任意
                irFunc.instrs.emplace_back(IRInstrType::SNEZ, "t0", "t0");   // 规范化为 0/1
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "0");      // 短路情况：固定 0
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            } else { // ||
                // 左 != 0 → 结果就是 1，跳过右值计算
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);          // 结果 t0 = right 值，可能任意
                irFunc.instrs.emplace_back(IRInstrType::SNEZ, "t0", "t0");   // 规范化为 0/1
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "1");      // 短路情况：固定 1
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            }
            return;
        }

        // 其他二元运算：左边结果先 spill 到栈槽 → 算右边 → 左边 reload 到 t1，右边在 t0 → 运算写回 t0
        IRInstrType opType = IRInstrType::ADD;
        bool isArith = (bin->op == "+" || bin->op == "-" || bin->op == "*" ||
                        bin->op == "/" || bin->op == "%");
        bool isCmp = !isArith;

        if (bin->op == "+") opType = IRInstrType::ADD;
        else if (bin->op == "-") opType = IRInstrType::SUB;
        else if (bin->op == "*") opType = IRInstrType::MUL;
        else if (bin->op == "/") opType = IRInstrType::DIV;
        else if (bin->op == "%") opType = IRInstrType::REM;

        buildExpr(bin->left, irFunc);          // 左值 → t0
        int leftSlot = allocSlot();
        irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(leftSlot));

        buildExpr(bin->right, irFunc);         // 右值 → t0  (期间函数调用会破坏所有临时寄存器)

        // 左值 reload 到 t1
        irFunc.instrs.emplace_back(IRInstrType::LOAD, "t1", std::to_string(leftSlot));

        if (isArith) {
            irFunc.instrs.emplace_back(opType, "t0", "t1", "t0");   // t0 = t1 OP t0 (左 OP 右)
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
        int offset = declareVar(varDecl->name);
        if (varDecl->init) {
            buildExpr(varDecl->init, irFunc);
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
        }
        return;
    }

    // 赋值语句
    if (auto assign = std::dynamic_pointer_cast<AssignStmt>(stmt)) {
        buildExpr(assign->value, irFunc);
        if (isGlobalVar(assign->name)) {
            irFunc.instrs.emplace_back(IRInstrType::STORE_GLOBAL, "", assign->name, "t0");
        } else {
            int offset = getVarOffset(assign->name);
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
        }
        return;
    }

    // return 语句：把值送入 a0 后跳统一返回标签（尾声在那里，保证栈帧恢复）
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
    functionParamCounts.clear();

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
        }
        if (auto funcDef = std::dynamic_pointer_cast<FunctionDef>(unit)) {
            functionParamCounts[funcDef->name] = funcDef->params.size();
        }
    }

    // 第二遍：逐函数生成 IR
    for (const auto& unit : compUnit->units) {
        auto funcDef = std::dynamic_pointer_cast<FunctionDef>(unit);
        if (!funcDef) continue;

        IRFunction irFunc;
        irFunc.name = funcDef->name;
        irFunc.paramCount = (int)funcDef->params.size();
        irFunc.isVoid = (funcDef->returnType == ValueType::Void);

        // 构造新的函数级上下文
        FuncContext ctx;
        fctx = &ctx;
        labelCounter = 0;
        loopStack.clear();
        currentReturnLabel = ".ret_" + funcDef->name;

        // 进入函数最外层作用域（参数 + 函数体共用）
        scopePush();

        // 参数：登记 + 从 aN 寄存器存入各自栈槽（≥9 个参数先不支持）
        if (funcDef->params.size() > 8) {
            throw std::runtime_error(
                "Function '" + funcDef->name + "' has " +
                std::to_string(funcDef->params.size()) +
                " parameters; currently only up to 8 parameters are supported."
            );
        }
        for (size_t i = 0; i < funcDef->params.size(); ++i) {
            int off = declareVar(funcDef->params[i].name);
            std::string reg = "a" + std::to_string(i);
            irFunc.instrs.emplace_back(IRInstrType::MV, "t0", reg);
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(off));
        }

        if (funcDef->body) {
            buildStmt(funcDef->body, irFunc);
        }

        // 退出最外层作用域
        scopePop();

        // 放统一返回标签：函数体内任何 return 跳到这里，然后自然掉到尾声
        irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", currentReturnLabel);

        // 保存局部区域总字节数，供后端据此设置栈帧
        irFunc.localSize = ctx.nextOffset;

        fctx = nullptr;
        program.functions.push_back(std::move(irFunc));
    }

    currentChecker = nullptr;
    return program;
}

} // namespace toycc
