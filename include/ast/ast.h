#pragma once

#include <memory>
#include <string>
#include <vector>

namespace toycc {

enum class ASTNodeType {
    CompUnit,
    Function,
    Block,

    ReturnStmt,

    BinaryExpr,
    UnaryExpr,

    NumberLiteral,
    Variable
};

class ASTNode {
public:
    ASTNodeType type;

    explicit ASTNode(ASTNodeType type)
        : type(type) {}

    virtual ~ASTNode() = default;
};

class NumberLiteral : public ASTNode {
public:
    int value;

    explicit NumberLiteral(int value)
        : ASTNode(ASTNodeType::NumberLiteral), value(value) {}
};

class Variable : public ASTNode {
public:
    std::string name;

    explicit Variable(const std::string& name)
        : ASTNode(ASTNodeType::Variable), name(name) {}
};

using ASTNodePtr = std::shared_ptr<ASTNode>;

} // namespace toycc