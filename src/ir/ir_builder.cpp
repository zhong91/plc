#include "ir/ir_builder.h"
#include "semantic/semantic_checker.h"
#include <iostream>
#include <stdexcept>
#include <map>
#include <set>
#include <vector>
#include <functional>

namespace toycc {

// ★ 这些仍然保留，用于测试模式（checker 为空时）
static std::map<std::string, int> mockOffsetMap;
static int nextOffset = 0;
static int labelCounter = 0;
static std::set<std::string> globalNames;
static std::map<std::string, int> functionParamCounts;

// 循环栈
static std::vector<std::pair<std::string, std::string>> loopStack;

// ★ 当前使用的 SemanticChecker（由 IRBuilder 设置）
static const SemanticChecker* currentChecker = nullptr;

std::string newLabel() { return ".L" + std::to_string(labelCounter++); }

void buildStmt(const StmtPtr& stmt, IRFunction& irFunc);

// ★★★ 辅助：判断变量是否全局 ★★★
bool isGlobalVar(const std::string& name) {
    if (currentChecker) {
        return currentChecker->isGlobal(name);
    }
    // 测试模式：从 globalNames 查询
    return globalNames.find(name) != globalNames.end();
}

// ★★★ 辅助：获取变量栈偏移（仅局部变量）★★★
int getVarOffset(const std::string& name) {
    if (currentChecker) {
        // 如果有符号表，确保变量存在（语义检查已通过）
        if (!currentChecker->exists(name)) {
            throw std::runtime_error("Variable '" + name + "' not found in symbol table");
        }
        // 偏移量由后端分配，从 mockOffsetMap 查询
        auto it = mockOffsetMap.find(name);
        if (it == mockOffsetMap.end()) {
            throw std::runtime_error("No offset allocated for variable '" + name + "'");
        }
        return it->second;
    }
    // 测试模式：从 mockOffsetMap 查询
    auto it = mockOffsetMap.find(name);
    if (it == mockOffsetMap.end()) {
        throw std::runtime_error("Variable '" + name + "' not found (test mode)");
    }
    return it->second;
}

// ★★★ 辅助：获取常量值 ★★★
bool isConstantVar(const std::string& name) {
    if (currentChecker) {
        return currentChecker->isConstant(name);
    }
    return false;  // 测试模式不支持常量查询
}

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

// ★★★ 表达式处理（支持逻辑运算 && ||）★★★
void buildExpr(const ExprPtr& expr, IRFunction& irFunc) {
    if (auto num = std::dynamic_pointer_cast<NumberLiteral>(expr)) {
        irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(num->value));
        return;
    }
    
    if (auto var = std::dynamic_pointer_cast<Variable>(expr)) {
        // ★ 优先使用符号表判断是否为全局变量
        if (isGlobalVar(var->name)) {
            irFunc.instrs.emplace_back(IRInstrType::LOAD_GLOBAL, "t0", var->name);
        } else {
            // ★ 检查是否为常量（符号表模式）
            if (currentChecker && isConstantVar(var->name)) {
                // 常量直接加载值，不访问内存
                int val = getConstValue(var->name);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", std::to_string(val));
            } else {
                // 局部变量：从栈加载
                int offset = getVarOffset(var->name);
                irFunc.instrs.emplace_back(IRInstrType::LOAD, "t0", std::to_string(offset));
            }
        }
        return;
    }

    if (auto call = std::dynamic_pointer_cast<FunctionCall>(expr)) {
        for (size_t i = 0; i < call->args.size() && i < 8; ++i) {
            buildExpr(call->args[i], irFunc);
            std::string reg = "a" + std::to_string(i);
            irFunc.instrs.emplace_back(IRInstrType::MV, reg, "t0");
        }
        irFunc.instrs.emplace_back(IRInstrType::CALL, "", call->name);
        irFunc.instrs.emplace_back(IRInstrType::MV, "t0", "a0");
        return;
    }

    if (auto bin = std::dynamic_pointer_cast<BinaryExpr>(expr)) {
        // ★ 逻辑运算 && 和 ||
        if (bin->op == "&&" || bin->op == "||") {
            std::string shortLabel = newLabel();
            std::string endLabel = newLabel();
            buildExpr(bin->left, irFunc);
            if (bin->op == "&&") {
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "0");
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            } else { // ||
                irFunc.instrs.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", shortLabel);
                buildExpr(bin->right, irFunc);
                irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", endLabel);
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", shortLabel);
                irFunc.instrs.emplace_back(IRInstrType::LI, "t0", "1");
                irFunc.instrs.emplace_back(IRInstrType::LABEL, "", "", "", endLabel);
            }
            return;
        }

        // 算术运算
        if (bin->op == "+" || bin->op == "-" || bin->op == "*" ||
            bin->op == "/" || bin->op == "%") {
            buildExpr(bin->left, irFunc);
            irFunc.instrs.emplace_back(IRInstrType::MV, "t1", "t0");
            buildExpr(bin->right, irFunc);
            IRInstrType opType;
            if (bin->op == "+") opType = IRInstrType::ADD;
            else if (bin->op == "-") opType = IRInstrType::SUB;
            else if (bin->op == "*") opType = IRInstrType::MUL;
            else if (bin->op == "/") opType = IRInstrType::DIV;
            else if (bin->op == "%") opType = IRInstrType::REM;
            irFunc.instrs.emplace_back(opType, "t0", "t1", "t0");
            return;
        }
        // 比较运算
        buildExpr(bin->left, irFunc);
        irFunc.instrs.emplace_back(IRInstrType::MV, "t1", "t0");
        buildExpr(bin->right, irFunc);
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

// ★★★ 语句处理 ★★★
void buildStmt(const StmtPtr& stmt, IRFunction& irFunc) {
    if (!stmt) return;

    if (auto block = std::dynamic_pointer_cast<Block>(stmt)) {
        for (const auto& s : block->statements) {
            buildStmt(s, irFunc);
        }
        return;
    }

    // ★ 变量声明
    if (auto varDecl = std::dynamic_pointer_cast<VarDeclStmt>(stmt)) {
        // 分配栈偏移量（只对局部变量，全局变量已在顶层收集）
        int offset = nextOffset;
        nextOffset += 4;
        mockOffsetMap[varDecl->name] = offset;

        // 处理初始化表达式
        if (varDecl->init) {
            buildExpr(varDecl->init, irFunc);
            irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
        }
        return;
    }

    // ★ 赋值语句
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

    // ★ Return 语句
    if (auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmt)) {
        if (ret->value) {
            buildExpr(ret->value, irFunc);
            irFunc.instrs.emplace_back(IRInstrType::MV, "a0", "t0");
        }
        irFunc.instrs.emplace_back(IRInstrType::RET);
        return;
    }

    // ★ If 语句
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

    // ★ While 语句
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

    // ★ Break
    if (auto breakStmt = std::dynamic_pointer_cast<BreakStmt>(stmt)) {
        if (loopStack.empty()) {
            throw std::runtime_error("break outside loop");
        }
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", loopStack.back().second);
        return;
    }

    // ★ Continue
    if (auto continueStmt = std::dynamic_pointer_cast<ContinueStmt>(stmt)) {
        if (loopStack.empty()) {
            throw std::runtime_error("continue outside loop");
        }
        irFunc.instrs.emplace_back(IRInstrType::JUMP, "", "", "", loopStack.back().first);
        return;
    }

    // ★ ExprStmt（表达式语句）
    if (auto exprStmt = std::dynamic_pointer_cast<ExprStmt>(stmt)) {
        buildExpr(exprStmt->expr, irFunc);
        return;
    }
}

// ★★★ IRBuilder::build ★★★
IRProgram IRBuilder::build(const ASTNodePtr& root, const SemanticChecker* checker) {
    // ★ 设置当前 checker（供辅助函数使用）
    currentChecker = checker;

    IRProgram program;
    auto compUnit = std::dynamic_pointer_cast<CompUnit>(root);
    if (!compUnit) throw std::runtime_error("Root is not CompUnit");

    globalNames.clear();
    functionParamCounts.clear();

    // 第一遍：收集全局变量和函数信息
    for (const auto& unit : compUnit->units) {
        if (auto varDecl = std::dynamic_pointer_cast<VarDeclStmt>(unit)) {
            // 如果有符号表，从符号表获取常量值；否则从 AST 读取
            int initValue = 0;
            if (checker && varDecl->isConst) {
                // 从符号表获取常量值
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

    // 第二遍：处理函数
    for (const auto& unit : compUnit->units) {
        if (auto funcDef = std::dynamic_pointer_cast<FunctionDef>(unit)) {
            IRFunction irFunc;
            irFunc.name = funcDef->name;
            irFunc.paramCount = funcDef->params.size();
            irFunc.isVoid = (funcDef->returnType == ValueType::Void);

            // ★ 重置后端状态（每个函数独立）
            mockOffsetMap.clear();
            nextOffset = 0;
            labelCounter = 0;
            loopStack.clear();

            // 参数作为局部变量（分配偏移量）
            for (size_t i = 0; i < funcDef->params.size() && i < 8; ++i) {
                int offset = nextOffset;
                nextOffset += 4;
                mockOffsetMap[funcDef->params[i].name] = offset;
                std::string reg = "a" + std::to_string(i);
                irFunc.instrs.emplace_back(IRInstrType::MV, "t0", reg);
                irFunc.instrs.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(offset));
            }

            if (funcDef->body) {
                buildStmt(funcDef->body, irFunc);
            }

            // 如果函数没有 return，自动添加 RET
            if (irFunc.instrs.empty() || irFunc.instrs.back().type != IRInstrType::RET) {
                irFunc.instrs.emplace_back(IRInstrType::RET);
            }

            program.functions.push_back(irFunc);
        }
    }

    // ★ 清理
    currentChecker = nullptr;
    return program;
}

} // namespace toycc