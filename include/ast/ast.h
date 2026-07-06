#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace toycc {

enum class ASTNodeType {
    CompUnit,
    Function,
    Block,

    ReturnStmt,
    ExprStmt,
    EmptyStmt,

    BinaryExpr,
    UnaryExpr,
    FunctionCall,

    NumberLiteral,
    Variable
};

enum class ValueType {
    Int,
    Void
};

class ASTNode {
public:
    ASTNodeType type;

    explicit ASTNode(ASTNodeType type)
        : type(type) {}

    virtual ~ASTNode() = default;
};

class Expr : public ASTNode {
public:
    explicit Expr(ASTNodeType type)
        : ASTNode(type) {}
};

class Stmt : public ASTNode {
public:
    explicit Stmt(ASTNodeType type)
        : ASTNode(type) {}
};

using ASTNodePtr = std::shared_ptr<ASTNode>;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;

struct Param {
    ValueType type;
    std::string name;

    Param(ValueType type, std::string name)
        : type(type), name(std::move(name)) {}
};

class CompUnit : public ASTNode {
public:
    std::vector<ASTNodePtr> units;

    CompUnit()
        : ASTNode(ASTNodeType::CompUnit) {}
};

class Block : public Stmt {
public:
    std::vector<StmtPtr> statements;

    Block()
        : Stmt(ASTNodeType::Block) {}
};

class FunctionDef : public ASTNode {
public:
    ValueType returnType;
    std::string name;
    std::vector<Param> params;
    std::shared_ptr<Block> body;

    FunctionDef(
        ValueType returnType,
        std::string name,
        std::vector<Param> params,
        std::shared_ptr<Block> body
    )
        : ASTNode(ASTNodeType::Function),
          returnType(returnType),
          name(std::move(name)),
          params(std::move(params)),
          body(std::move(body)) {}
};

class ReturnStmt : public Stmt {
public:
    ExprPtr value;

    explicit ReturnStmt(ExprPtr value)
        : Stmt(ASTNodeType::ReturnStmt), value(std::move(value)) {}
};

class NumberLiteral : public Expr {
public:
    int value;

    explicit NumberLiteral(int value)
        : Expr(ASTNodeType::NumberLiteral), value(value) {}
};

class Variable : public Expr {
public:
    std::string name;

    explicit Variable(std::string name)
        : Expr(ASTNodeType::Variable), name(std::move(name)) {}
};

class UnaryExpr : public Expr {
public:
    std::string op;
    ExprPtr operand;

    UnaryExpr(std::string op, ExprPtr operand)
        : Expr(ASTNodeType::UnaryExpr),
          op(std::move(op)),
          operand(std::move(operand)) {}
};

class BinaryExpr : public Expr {
public:
    std::string op;
    ExprPtr left;
    ExprPtr right;

    BinaryExpr(std::string op, ExprPtr left, ExprPtr right)
        : Expr(ASTNodeType::BinaryExpr),
          op(std::move(op)),
          left(std::move(left)),
          right(std::move(right)) {}
};

class FunctionCall : public Expr {
public:
    std::string name;
    std::vector<ExprPtr> args;

    FunctionCall(std::string name, std::vector<ExprPtr> args)
        : Expr(ASTNodeType::FunctionCall),
          name(std::move(name)),
          args(std::move(args)) {}
};

} // namespace toycc