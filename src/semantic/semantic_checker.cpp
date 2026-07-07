#include "semantic/semantic_checker.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toycc {
namespace {

enum class SymbolKind {
    Variable,
    Constant,
    Parameter,
    Function
};

struct FunctionSignature {
    ValueType returnType = ValueType::Void;
    std::vector<ValueType> parameterTypes;
};

struct Symbol {
    SymbolKind kind = SymbolKind::Variable;
    ValueType type = ValueType::Int;
    std::optional<std::int32_t> constantValue;
    FunctionSignature function;
};

struct FlowSummary {
    bool normal = false;
    bool returns = false;
    bool breaks = false;
    bool continues = false;
    bool diverges = false;

    static FlowSummary fallthrough() {
        FlowSummary flow;
        flow.normal = true;
        return flow;
    }

    static FlowSummary returned() {
        FlowSummary flow;
        flow.returns = true;
        return flow;
    }

    static FlowSummary broken() {
        FlowSummary flow;
        flow.breaks = true;
        return flow;
    }

    static FlowSummary continued() {
        FlowSummary flow;
        flow.continues = true;
        return flow;
    }
};

[[noreturn]] void fail(const std::string& message) {
    throw SemanticError("Semantic error: " + message);
}

std::string valueTypeName(ValueType type) {
    switch (type) {
        case ValueType::Int:
            return "int";
        case ValueType::Void:
            return "void";
    }
    return "<unknown>";
}

class Analyzer {
public:
    void check(const ASTNodePtr& root) {
        reset();

        if (!root) {
            fail("AST root is null.");
        }
        if (root->type != ASTNodeType::CompUnit) {
            fail("AST root must be a CompUnit.");
        }

        const auto compUnit = std::dynamic_pointer_cast<CompUnit>(root);
        if (!compUnit) {
            fail("AST node tagged as CompUnit has an invalid dynamic type.");
        }

        pushScope(); // global scope

        for (const auto& unit : compUnit->units) {
            if (!unit) {
                fail("compilation unit contains a null top-level node.");
            }

            switch (unit->type) {
                case ASTNodeType::VarDeclStmt:
                    analyzeVariableDeclaration(
                        checkedCast<VarDeclStmt>(unit, "top-level variable declaration")
                    );
                    break;

                case ASTNodeType::Function:
                    analyzeFunction(
                        checkedCast<FunctionDef>(unit, "function definition")
                    );
                    break;

                default:
                    fail("only declarations and function definitions are allowed at global scope.");
            }
        }

        validateMainFunction();
        popScope();
    }

private:
    std::vector<std::unordered_map<std::string, Symbol>> scopes_;
    ValueType currentReturnType_ = ValueType::Void;
    std::string currentFunctionName_;
    bool insideFunction_ = false;
    int loopDepth_ = 0;

    void reset() {
        scopes_.clear();
        currentReturnType_ = ValueType::Void;
        currentFunctionName_.clear();
        insideFunction_ = false;
        loopDepth_ = 0;
    }

    template <typename T, typename U>
    std::shared_ptr<T> checkedCast(
        const std::shared_ptr<U>& node,
        const std::string& description
    ) const {
        const auto casted = std::dynamic_pointer_cast<T>(node);
        if (!casted) {
            fail("invalid AST dynamic type for " + description + ".");
        }
        return casted;
    }

    void pushScope() {
        scopes_.emplace_back();
    }

    void popScope() {
        if (scopes_.empty()) {
            fail("internal scope stack underflow.");
        }
        scopes_.pop_back();
    }

    Symbol* lookup(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    const Symbol* lookup(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

bool existsInCurrentScope(const std::string& name) const {
    if (scopes_.empty()) {
        return false;
    }

    const auto& currentScope = scopes_.back();
    return currentScope.find(name) != currentScope.end();
}

    void declare(const std::string& name, Symbol symbol) {
        if (scopes_.empty()) {
            fail("internal error: declaration without a scope.");
        }
        if (name.empty()) {
            fail("an identifier name cannot be empty.");
        }
        if (existsInCurrentScope(name)) {
            fail("duplicate declaration of '" + name + "' in the same scope.");
        }
        scopes_.back().emplace(name, std::move(symbol));
    }

    void analyzeFunction(const std::shared_ptr<FunctionDef>& function) {
        if (!function) {
            fail("function definition is null.");
        }
        if (function->name.empty()) {
            fail("function name cannot be empty.");
        }
        if (function->returnType != ValueType::Int &&
            function->returnType != ValueType::Void) {
            fail("function '" + function->name + "' has an unsupported return type.");
        }

        FunctionSignature signature;
        signature.returnType = function->returnType;
        signature.parameterTypes.reserve(function->params.size());
        for (const auto& parameter : function->params) {
            if (parameter.type != ValueType::Int) {
                fail("parameter '" + parameter.name + "' of function '" +
                     function->name + "' must have type int.");
            }
            signature.parameterTypes.push_back(parameter.type);
        }

        Symbol functionSymbol;
        functionSymbol.kind = SymbolKind::Function;
        functionSymbol.type = function->returnType;
        functionSymbol.function = signature;

        // Register before checking the body so direct recursion is legal, while
        // calls to functions appearing later in the file remain illegal.
        declare(function->name, std::move(functionSymbol));

        if (!function->body) {
            fail("function '" + function->name + "' has no body.");
        }

        const bool previousInsideFunction = insideFunction_;
        const ValueType previousReturnType = currentReturnType_;
        const std::string previousFunctionName = currentFunctionName_;
        const int previousLoopDepth = loopDepth_;

        insideFunction_ = true;
        currentReturnType_ = function->returnType;
        currentFunctionName_ = function->name;
        loopDepth_ = 0;

        pushScope();
        for (const auto& parameter : function->params) {
            Symbol parameterSymbol;
            parameterSymbol.kind = SymbolKind::Parameter;
            parameterSymbol.type = ValueType::Int;
            declare(parameter.name, std::move(parameterSymbol));
        }

        // The parameter scope and the outermost function body share one scope,
        // matching C's rule that a top-level local cannot redeclare a parameter.
        const FlowSummary flow = analyzeBlock(function->body, false);

        if (function->returnType == ValueType::Int && !allPathsReturn(flow)) {
            fail("int function '" + function->name +
                 "' does not return an int value on every possible execution path.");
        }

        popScope();

        insideFunction_ = previousInsideFunction;
        currentReturnType_ = previousReturnType;
        currentFunctionName_ = previousFunctionName;
        loopDepth_ = previousLoopDepth;
    }

    FlowSummary analyzeBlock(
        const std::shared_ptr<Block>& block,
        bool createScope
    ) {
        if (!block) {
            fail("block is null.");
        }

        if (createScope) {
            pushScope();
        }

        FlowSummary result = FlowSummary::fallthrough();

        for (const auto& statement : block->statements) {
            if (!statement) {
                fail("block contains a null statement.");
            }

            // Always analyze unreachable statements too, so invalid names and
            // illegal control-flow constructs cannot hide after a return.
            const FlowSummary next = analyzeStatement(statement);

            if (result.normal) {
                result.returns = result.returns || next.returns;
                result.breaks = result.breaks || next.breaks;
                result.continues = result.continues || next.continues;
                result.diverges = result.diverges || next.diverges;
                result.normal = next.normal;
            }
        }

        if (createScope) {
            popScope();
        }

        return result;
    }

    FlowSummary analyzeStatement(const StmtPtr& statement) {
        if (!statement) {
            fail("statement is null.");
        }

        switch (statement->type) {
            case ASTNodeType::Block:
                return analyzeBlock(
                    checkedCast<Block>(statement, "block statement"),
                    true
                );

            case ASTNodeType::ReturnStmt:
                analyzeReturnStatement(
                    checkedCast<ReturnStmt>(statement, "return statement")
                );
                return FlowSummary::returned();

            case ASTNodeType::ExprStmt: {
                const auto exprStmt = checkedCast<ExprStmt>(
                    statement,
                    "expression statement"
                );
                if (!exprStmt->expr) {
                    fail("expression statement has no expression.");
                }
                (void)analyzeExpression(exprStmt->expr, true);
                return FlowSummary::fallthrough();
            }

            case ASTNodeType::EmptyStmt:
                (void)checkedCast<EmptyStmt>(statement, "empty statement");
                return FlowSummary::fallthrough();

            case ASTNodeType::VarDeclStmt:
                analyzeVariableDeclaration(
                    checkedCast<VarDeclStmt>(statement, "variable declaration")
                );
                return FlowSummary::fallthrough();

            case ASTNodeType::AssignStmt:
                analyzeAssignment(
                    checkedCast<AssignStmt>(statement, "assignment statement")
                );
                return FlowSummary::fallthrough();

            case ASTNodeType::IfStmt:
                return analyzeIfStatement(
                    checkedCast<IfStmt>(statement, "if statement")
                );

            case ASTNodeType::WhileStmt:
                return analyzeWhileStatement(
                    checkedCast<WhileStmt>(statement, "while statement")
                );

            case ASTNodeType::BreakStmt:
                (void)checkedCast<BreakStmt>(statement, "break statement");
                if (loopDepth_ <= 0) {
                    fail("break statement is only allowed inside a while loop.");
                }
                return FlowSummary::broken();

            case ASTNodeType::ContinueStmt:
                (void)checkedCast<ContinueStmt>(statement, "continue statement");
                if (loopDepth_ <= 0) {
                    fail("continue statement is only allowed inside a while loop.");
                }
                return FlowSummary::continued();

            default:
                fail("unexpected AST node in statement position.");
        }
    }

    void analyzeVariableDeclaration(
        const std::shared_ptr<VarDeclStmt>& declaration
    ) {
        if (!declaration) {
            fail("variable declaration is null.");
        }
        if (declaration->name.empty()) {
            fail("variable name cannot be empty.");
        }
        if (existsInCurrentScope(declaration->name)) {
            fail("duplicate declaration of '" + declaration->name +
                 "' in the same scope.");
        }
        if (!declaration->init) {
            fail("declaration of '" + declaration->name +
                 "' must have an initializer.");
        }

        requireIntExpression(
            declaration->init,
            "initializer of '" + declaration->name + "'"
        );

        Symbol symbol;
        symbol.type = ValueType::Int;

        if (declaration->isConst) {
            ensureConstantExpression(declaration->init);
            symbol.kind = SymbolKind::Constant;
            symbol.constantValue = evaluateConstantExpression(declaration->init);
        } else {
            symbol.kind = SymbolKind::Variable;
        }

        // Insert only after the initializer has been checked. Therefore the
        // identifier cannot refer to itself and all uses occur after declaration.
        declare(declaration->name, std::move(symbol));
    }

    void analyzeAssignment(const std::shared_ptr<AssignStmt>& assignment) {
        if (!assignment) {
            fail("assignment statement is null.");
        }
        Symbol* symbol = lookup(assignment->name);
        if (!symbol) {
            fail("assignment to undefined identifier '" + assignment->name + "'.");
        }
        if (symbol->kind == SymbolKind::Function) {
            fail("function '" + assignment->name + "' is not an assignable object.");
        }
        if (symbol->kind == SymbolKind::Constant) {
            fail("cannot assign to constant '" + assignment->name + "'.");
        }
        if (!assignment->value) {
            fail("assignment to '" + assignment->name + "' has no right-hand side.");
        }
        requireIntExpression(
            assignment->value,
            "right-hand side of assignment to '" + assignment->name + "'"
        );
    }

    void analyzeReturnStatement(const std::shared_ptr<ReturnStmt>& statement) {
        if (!insideFunction_) {
            fail("return statement appears outside a function.");
        }

        if (currentReturnType_ == ValueType::Int) {
            if (!statement->value) {
                fail("int function '" + currentFunctionName_ +
                     "' must return an int value.");
            }
            requireIntExpression(
                statement->value,
                "return value of function '" + currentFunctionName_ + "'"
            );
            return;
        }

        if (statement->value) {
            fail("void function '" + currentFunctionName_ +
                 "' cannot return a value.");
        }
    }

    FlowSummary analyzeIfStatement(const std::shared_ptr<IfStmt>& statement) {
        if (!statement->condition) {
            fail("if statement has no condition.");
        }
        if (!statement->thenBranch) {
            fail("if statement has no then branch.");
        }

        requireIntExpression(statement->condition, "if condition");

        const std::optional<std::int32_t> conditionValue =
            tryEvaluateConstantExpression(statement->condition);

        const FlowSummary thenFlow = analyzeStatement(statement->thenBranch);
        const FlowSummary elseFlow = statement->elseBranch
            ? analyzeStatement(statement->elseBranch)
            : FlowSummary::fallthrough();

        // Both branches are checked semantically, but a compile-time constant
        // condition removes the unreachable branch from control-flow analysis.
        if (conditionValue.has_value()) {
            return *conditionValue != 0 ? thenFlow : elseFlow;
        }

        FlowSummary result;
        result.normal = thenFlow.normal || elseFlow.normal;
        result.returns = thenFlow.returns || elseFlow.returns;
        result.breaks = thenFlow.breaks || elseFlow.breaks;
        result.continues = thenFlow.continues || elseFlow.continues;
        result.diverges = thenFlow.diverges || elseFlow.diverges;
        return result;
    }

    FlowSummary analyzeWhileStatement(
        const std::shared_ptr<WhileStmt>& statement
    ) {
        if (!statement->condition) {
            fail("while statement has no condition.");
        }
        if (!statement->body) {
            fail("while statement has no body.");
        }

        requireIntExpression(statement->condition, "while condition");
        const std::optional<std::int32_t> conditionValue =
            tryEvaluateConstantExpression(statement->condition);

        ++loopDepth_;
        const FlowSummary bodyFlow = analyzeStatement(statement->body);
        --loopDepth_;

        if (conditionValue.has_value() && *conditionValue == 0) {
            return FlowSummary::fallthrough();
        }

        FlowSummary result;
        result.returns = bodyFlow.returns;

        if (conditionValue.has_value()) {
            // The condition is known true. break exits normally; a body path
            // that falls through, continues, or already diverges can loop forever.
            result.normal = bodyFlow.breaks;
            result.diverges = bodyFlow.normal ||
                              bodyFlow.continues ||
                              bodyFlow.diverges;
        } else {
            // An unknown condition may be false before the first iteration.
            result.normal = true;
            result.diverges = bodyFlow.normal ||
                              bodyFlow.continues ||
                              bodyFlow.diverges;
        }

        // break and continue are consumed by the loop.
        result.breaks = false;
        result.continues = false;
        return result;
    }

    ValueType analyzeExpression(const ExprPtr& expression, bool evaluated) {
        if (!expression) {
            fail("expression is null.");
        }

        switch (expression->type) {
            case ASTNodeType::NumberLiteral:
                (void)checkedCast<NumberLiteral>(expression, "number literal");
                return ValueType::Int;

            case ASTNodeType::Variable:
                return analyzeVariableReference(
                    checkedCast<Variable>(expression, "variable reference")
                );

            case ASTNodeType::FunctionCall:
                return analyzeFunctionCall(
                    checkedCast<FunctionCall>(expression, "function call"),
                    evaluated
                );

            case ASTNodeType::UnaryExpr:
                return analyzeUnaryExpression(
                    checkedCast<UnaryExpr>(expression, "unary expression"),
                    evaluated
                );

            case ASTNodeType::BinaryExpr:
                return analyzeBinaryExpression(
                    checkedCast<BinaryExpr>(expression, "binary expression"),
                    evaluated
                );

            default:
                fail("unexpected AST node in expression position.");
        }
    }

    ValueType analyzeVariableReference(const std::shared_ptr<Variable>& variable) {
        const Symbol* symbol = lookup(variable->name);
        if (!symbol) {
            fail("use of undefined identifier '" + variable->name + "'.");
        }
        if (symbol->kind == SymbolKind::Function) {
            fail("function '" + variable->name + "' cannot be used as an int value.");
        }
        return ValueType::Int;
    }

    ValueType analyzeFunctionCall(
        const std::shared_ptr<FunctionCall>& call,
        bool evaluated
    ) {
        const Symbol* symbol = lookup(call->name);
        if (!symbol) {
            fail("call to undefined function '" + call->name +
                 "' (functions must be defined before use, except recursion).");
        }
        if (symbol->kind != SymbolKind::Function) {
            fail("identifier '" + call->name + "' is not a function.");
        }

        if (call->args.size() != symbol->function.parameterTypes.size()) {
            std::ostringstream oss;
            oss << "function '" << call->name << "' expects "
                << symbol->function.parameterTypes.size() << " argument(s), but "
                << call->args.size() << " were provided.";
            fail(oss.str());
        }

        for (std::size_t index = 0; index < call->args.size(); ++index) {
            if (!call->args[index]) {
                fail("function call to '" + call->name +
                     "' contains a null argument.");
            }
            const ValueType actualType = analyzeExpression(call->args[index], evaluated);
            const ValueType expectedType = symbol->function.parameterTypes[index];
            if (actualType != expectedType) {
                std::ostringstream oss;
                oss << "argument " << (index + 1) << " of function '"
                    << call->name << "' has type " << valueTypeName(actualType)
                    << ", expected " << valueTypeName(expectedType) << ".";
                fail(oss.str());
            }
        }

        return symbol->function.returnType;
    }

    ValueType analyzeUnaryExpression(
        const std::shared_ptr<UnaryExpr>& expression,
        bool evaluated
    ) {
        if (expression->op != "+" &&
            expression->op != "-" &&
            expression->op != "!") {
            fail("unsupported unary operator '" + expression->op + "'.");
        }

        const ValueType operandType =
            analyzeExpression(expression->operand, evaluated);
        if (operandType != ValueType::Int) {
            fail("unary operator '" + expression->op +
                 "' requires an int operand.");
        }

        if (evaluated && expression->op == "-") {
            const auto constant = tryEvaluateConstantExpression(expression->operand);
            if (constant.has_value() &&
                *constant == std::numeric_limits<std::int32_t>::min()) {
                fail("constant unary negation overflows 32-bit signed int.");
            }
        }

        return ValueType::Int;
    }

    ValueType analyzeBinaryExpression(
        const std::shared_ptr<BinaryExpr>& expression,
        bool evaluated
    ) {
        if (!isSupportedBinaryOperator(expression->op)) {
            fail("unsupported binary operator '" + expression->op + "'.");
        }

        const ValueType leftType = analyzeExpression(expression->left, evaluated);
        if (leftType != ValueType::Int) {
            fail("left operand of '" + expression->op + "' must have type int.");
        }

        bool rightEvaluated = evaluated;
        if (evaluated && (expression->op == "&&" || expression->op == "||")) {
            const auto leftValue = tryEvaluateConstantExpression(expression->left);
            if (leftValue.has_value()) {
                if (expression->op == "&&" && *leftValue == 0) {
                    rightEvaluated = false;
                }
                if (expression->op == "||" && *leftValue != 0) {
                    rightEvaluated = false;
                }
            }
        }

        const ValueType rightType =
            analyzeExpression(expression->right, rightEvaluated);
        if (rightType != ValueType::Int) {
            fail("right operand of '" + expression->op + "' must have type int.");
        }

        if (rightEvaluated &&
            (expression->op == "/" || expression->op == "%")) {
            const auto divisor = tryEvaluateConstantExpression(expression->right);
            if (divisor.has_value() && *divisor == 0) {
                fail("division or remainder by zero.");
            }
        }

        if (evaluated) {
            // Evaluating a fully constant expression here also catches signed
            // overflow such as INT_MAX + 1 and INT_MIN / -1.
            (void)tryEvaluateConstantExpression(expression);
        }

        return ValueType::Int;
    }

    void requireIntExpression(
        const ExprPtr& expression,
        const std::string& context
    ) {
        const ValueType type = analyzeExpression(expression, true);
        if (type != ValueType::Int) {
            fail(context + " requires an int value, but the expression has type " +
                 valueTypeName(type) + ".");
        }
    }

    void ensureConstantExpression(const ExprPtr& expression) const {
        if (!expression) {
            fail("constant initializer contains a null expression.");
        }

        switch (expression->type) {
            case ASTNodeType::NumberLiteral:
                (void)checkedCast<NumberLiteral>(expression, "constant number literal");
                return;

            case ASTNodeType::Variable: {
                const auto variable = checkedCast<Variable>(
                    expression,
                    "constant variable reference"
                );
                const Symbol* symbol = lookup(variable->name);
                if (!symbol) {
                    fail("constant expression uses undefined identifier '" +
                         variable->name + "'.");
                }
                if (symbol->kind != SymbolKind::Constant ||
                    !symbol->constantValue.has_value()) {
                    fail("constant expression may only reference previously declared "
                         "constants; '" + variable->name + "' is not one.");
                }
                return;
            }

            case ASTNodeType::UnaryExpr: {
                const auto unary = checkedCast<UnaryExpr>(
                    expression,
                    "constant unary expression"
                );
                ensureConstantExpression(unary->operand);
                return;
            }

            case ASTNodeType::BinaryExpr: {
                const auto binary = checkedCast<BinaryExpr>(
                    expression,
                    "constant binary expression"
                );
                ensureConstantExpression(binary->left);
                ensureConstantExpression(binary->right);
                return;
            }

            case ASTNodeType::FunctionCall:
                fail("function calls are not allowed in constant expressions.");

            default:
                fail("unsupported node in constant expression.");
        }
    }

    std::int32_t evaluateConstantExpression(const ExprPtr& expression) const {
        const auto value = tryEvaluateConstantExpression(expression);
        if (!value.has_value()) {
            fail("expression is not a compile-time constant.");
        }
        return *value;
    }

    std::optional<std::int32_t> tryEvaluateConstantExpression(
        const ExprPtr& expression
    ) const {
        if (!expression) {
            return std::nullopt;
        }

        switch (expression->type) {
            case ASTNodeType::NumberLiteral: {
                const auto literal = checkedCast<NumberLiteral>(
                    expression,
                    "number literal"
                );
                return static_cast<std::int32_t>(literal->value);
            }

            case ASTNodeType::Variable: {
                const auto variable = checkedCast<Variable>(
                    expression,
                    "variable reference"
                );
                const Symbol* symbol = lookup(variable->name);
                if (!symbol || symbol->kind != SymbolKind::Constant) {
                    return std::nullopt;
                }
                return symbol->constantValue;
            }

            case ASTNodeType::FunctionCall:
                return std::nullopt;

            case ASTNodeType::UnaryExpr: {
                const auto unary = checkedCast<UnaryExpr>(
                    expression,
                    "unary expression"
                );
                const auto operand = tryEvaluateConstantExpression(unary->operand);
                if (!operand.has_value()) {
                    return std::nullopt;
                }
                if (unary->op == "+") {
                    return *operand;
                }
                if (unary->op == "-") {
                    if (*operand == std::numeric_limits<std::int32_t>::min()) {
                        fail("constant unary negation overflows 32-bit signed int.");
                    }
                    return static_cast<std::int32_t>(-*operand);
                }
                if (unary->op == "!") {
                    return static_cast<std::int32_t>(*operand == 0);
                }
                return std::nullopt;
            }

            case ASTNodeType::BinaryExpr: {
                const auto binary = checkedCast<BinaryExpr>(
                    expression,
                    "binary expression"
                );
                const auto left = tryEvaluateConstantExpression(binary->left);
                if (!left.has_value()) {
                    return std::nullopt;
                }

                if (binary->op == "&&" && *left == 0) {
                    return static_cast<std::int32_t>(0);
                }
                if (binary->op == "||" && *left != 0) {
                    return static_cast<std::int32_t>(1);
                }

                const auto right = tryEvaluateConstantExpression(binary->right);
                if (!right.has_value()) {
                    return std::nullopt;
                }

                return evaluateBinary(binary->op, *left, *right);
            }

            default:
                return std::nullopt;
        }
    }

    std::int32_t evaluateBinary(
        const std::string& op,
        std::int32_t left,
        std::int32_t right
    ) const {
        if (op == "+") {
            return checkedIntegerResult(
                static_cast<std::int64_t>(left) + right,
                "constant addition"
            );
        }
        if (op == "-") {
            return checkedIntegerResult(
                static_cast<std::int64_t>(left) - right,
                "constant subtraction"
            );
        }
        if (op == "*") {
            return checkedIntegerResult(
                static_cast<std::int64_t>(left) * right,
                "constant multiplication"
            );
        }
        if (op == "/") {
            if (right == 0) {
                fail("division by zero in constant expression.");
            }
            if (left == std::numeric_limits<std::int32_t>::min() && right == -1) {
                fail("constant division overflows 32-bit signed int.");
            }
            return static_cast<std::int32_t>(left / right);
        }
        if (op == "%") {
            if (right == 0) {
                fail("remainder by zero in constant expression.");
            }
            if (left == std::numeric_limits<std::int32_t>::min() && right == -1) {
                fail("constant remainder overflows 32-bit signed int.");
            }
            return static_cast<std::int32_t>(left % right);
        }
        if (op == "<") {
            return static_cast<std::int32_t>(left < right);
        }
        if (op == ">") {
            return static_cast<std::int32_t>(left > right);
        }
        if (op == "<=") {
            return static_cast<std::int32_t>(left <= right);
        }
        if (op == ">=") {
            return static_cast<std::int32_t>(left >= right);
        }
        if (op == "==") {
            return static_cast<std::int32_t>(left == right);
        }
        if (op == "!=") {
            return static_cast<std::int32_t>(left != right);
        }
        if (op == "&&") {
            return static_cast<std::int32_t>(left != 0 && right != 0);
        }
        if (op == "||") {
            return static_cast<std::int32_t>(left != 0 || right != 0);
        }

        return 0;
    }

    std::int32_t checkedIntegerResult(
        std::int64_t value,
        const std::string& operation
    ) const {
        if (value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max()) {
            fail(operation + " overflows 32-bit signed int.");
        }
        return static_cast<std::int32_t>(value);
    }

    bool isSupportedBinaryOperator(const std::string& op) const {
        return op == "+" || op == "-" || op == "*" || op == "/" ||
               op == "%" || op == "<" || op == ">" || op == "<=" ||
               op == ">=" || op == "==" || op == "!=" || op == "&&" ||
               op == "||";
    }

    bool allPathsReturn(const FlowSummary& flow) const {
        // A non-terminating path cannot fall off the end of the function.
        // Therefore it does not require an additional return statement; what
        // matters is that every path that can reach the function end has
        // already returned a value.
        return !flow.normal &&
               !flow.breaks &&
               !flow.continues;
    }

    void validateMainFunction() const {
        if (scopes_.empty()) {
            fail("internal error: missing global scope.");
        }

        const auto found = scopes_.front().find("main");
        if (found == scopes_.front().end()) {
            fail("program must define an entry function 'int main()'.");
        }

        const Symbol& symbol = found->second;
        if (symbol.kind != SymbolKind::Function) {
            fail("global identifier 'main' must be a function.");
        }
        if (symbol.function.returnType != ValueType::Int) {
            fail("main function must return int.");
        }
        if (!symbol.function.parameterTypes.empty()) {
            fail("main function must have an empty parameter list.");
        }
    }
};

} // namespace

void SemanticChecker::check(const ASTNodePtr& root) const {
    Analyzer analyzer;
    analyzer.check(root);
}

} // namespace toycc
