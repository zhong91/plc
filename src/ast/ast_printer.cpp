#include "ast/ast_printer.h"

#include <memory>
#include <ostream>
#include <string>

namespace toycc {

namespace {

void indent(std::ostream& os, int level) {
    for (int i = 0; i < level; ++i) {
        os << "  ";
    }
}

std::string valueTypeToString(ValueType type) {
    switch (type) {
        case ValueType::Int:
            return "int";
        case ValueType::Void:
            return "void";
    }

    return "unknown";
}

void printNode(const ASTNodePtr& node, std::ostream& os, int level);
void printStmt(const StmtPtr& stmt, std::ostream& os, int level);
void printExpr(const ExprPtr& expr, std::ostream& os, int level);

void printNode(const ASTNodePtr& node, std::ostream& os, int level) {
    if (!node) {
        indent(os, level);
        os << "<null>\n";
        return;
    }

    switch (node->type) {
        case ASTNodeType::CompUnit: {
            auto compUnit = std::dynamic_pointer_cast<CompUnit>(node);
            indent(os, level);
            os << "CompUnit\n";

            for (const auto& unit : compUnit->units) {
                printNode(unit, os, level + 1);
            }
            break;
        }

        case ASTNodeType::Function: {
            auto func = std::dynamic_pointer_cast<FunctionDef>(node);
            indent(os, level);
            os << "Function "
               << valueTypeToString(func->returnType)
               << " "
               << func->name
               << "\n";

            if (!func->params.empty()) {
                indent(os, level + 1);
                os << "Params\n";

                for (const auto& param : func->params) {
                    indent(os, level + 2);
                    os << valueTypeToString(param.type)
                       << " "
                       << param.name
                       << "\n";
                }
            }

            printStmt(func->body, os, level + 1);
            break;
        }

        case ASTNodeType::Block:
        case ASTNodeType::ReturnStmt:
        case ASTNodeType::ExprStmt:
        case ASTNodeType::EmptyStmt:
        case ASTNodeType::VarDeclStmt:
        case ASTNodeType::AssignStmt:
        case ASTNodeType::IfStmt:
        case ASTNodeType::WhileStmt:
        case ASTNodeType::BreakStmt:
        case ASTNodeType::ContinueStmt:
            printStmt(std::dynamic_pointer_cast<Stmt>(node), os, level);
            break;

        case ASTNodeType::BinaryExpr:
        case ASTNodeType::UnaryExpr:
        case ASTNodeType::FunctionCall:
        case ASTNodeType::NumberLiteral:
        case ASTNodeType::Variable:
            printExpr(std::dynamic_pointer_cast<Expr>(node), os, level);
            break;
    }
}

void printStmt(const StmtPtr& stmt, std::ostream& os, int level) {
    if (!stmt) {
        indent(os, level);
        os << "<null stmt>\n";
        return;
    }

    switch (stmt->type) {
        case ASTNodeType::Block: {
            auto block = std::dynamic_pointer_cast<Block>(stmt);
            indent(os, level);
            os << "Block\n";

            for (const auto& child : block->statements) {
                printStmt(child, os, level + 1);
            }
            break;
        }

        case ASTNodeType::ReturnStmt: {
            auto ret = std::dynamic_pointer_cast<ReturnStmt>(stmt);
            indent(os, level);
            os << "ReturnStmt\n";

            if (ret->value) {
                printExpr(ret->value, os, level + 1);
            }
            break;
        }

        case ASTNodeType::ExprStmt: {
            auto exprStmt = std::dynamic_pointer_cast<ExprStmt>(stmt);
            indent(os, level);
            os << "ExprStmt\n";
            printExpr(exprStmt->expr, os, level + 1);
            break;
        }

        case ASTNodeType::EmptyStmt: {
            indent(os, level);
            os << "EmptyStmt\n";
            break;
        }

        case ASTNodeType::VarDeclStmt: {
            auto varDecl = std::dynamic_pointer_cast<VarDeclStmt>(stmt);
            indent(os, level);
            os << "VarDeclStmt "
               << (varDecl->isConst ? "const " : "")
               << varDecl->name
               << "\n";

            printExpr(varDecl->init, os, level + 1);
            break;
        }

        case ASTNodeType::AssignStmt: {
            auto assign = std::dynamic_pointer_cast<AssignStmt>(stmt);
            indent(os, level);
            os << "AssignStmt " << assign->name << "\n";
            printExpr(assign->value, os, level + 1);
            break;
        }

        case ASTNodeType::IfStmt: {
            auto ifStmt = std::dynamic_pointer_cast<IfStmt>(stmt);
            indent(os, level);
            os << "IfStmt\n";

            indent(os, level + 1);
            os << "Condition\n";
            printExpr(ifStmt->condition, os, level + 2);

            indent(os, level + 1);
            os << "Then\n";
            printStmt(ifStmt->thenBranch, os, level + 2);

            if (ifStmt->elseBranch) {
                indent(os, level + 1);
                os << "Else\n";
                printStmt(ifStmt->elseBranch, os, level + 2);
            }
            break;
        }

        case ASTNodeType::WhileStmt: {
            auto whileStmt = std::dynamic_pointer_cast<WhileStmt>(stmt);
            indent(os, level);
            os << "WhileStmt\n";

            indent(os, level + 1);
            os << "Condition\n";
            printExpr(whileStmt->condition, os, level + 2);

            indent(os, level + 1);
            os << "Body\n";
            printStmt(whileStmt->body, os, level + 2);
            break;
        }

        case ASTNodeType::BreakStmt: {
            indent(os, level);
            os << "BreakStmt\n";
            break;
        }

        case ASTNodeType::ContinueStmt: {
            indent(os, level);
            os << "ContinueStmt\n";
            break;
        }

        default: {
            indent(os, level);
            os << "<unknown stmt>\n";
            break;
        }
    }
}

void printExpr(const ExprPtr& expr, std::ostream& os, int level) {
    if (!expr) {
        indent(os, level);
        os << "<null expr>\n";
        return;
    }

    switch (expr->type) {
        case ASTNodeType::NumberLiteral: {
            auto number = std::dynamic_pointer_cast<NumberLiteral>(expr);
            indent(os, level);
            os << "NumberLiteral " << number->value << "\n";
            break;
        }

        case ASTNodeType::Variable: {
            auto variable = std::dynamic_pointer_cast<Variable>(expr);
            indent(os, level);
            os << "Variable " << variable->name << "\n";
            break;
        }

        case ASTNodeType::UnaryExpr: {
            auto unary = std::dynamic_pointer_cast<UnaryExpr>(expr);
            indent(os, level);
            os << "UnaryExpr " << unary->op << "\n";
            printExpr(unary->operand, os, level + 1);
            break;
        }

        case ASTNodeType::BinaryExpr: {
            auto binary = std::dynamic_pointer_cast<BinaryExpr>(expr);
            indent(os, level);
            os << "BinaryExpr " << binary->op << "\n";
            printExpr(binary->left, os, level + 1);
            printExpr(binary->right, os, level + 1);
            break;
        }

        case ASTNodeType::FunctionCall: {
            auto call = std::dynamic_pointer_cast<FunctionCall>(expr);
            indent(os, level);
            os << "FunctionCall " << call->name << "\n";

            for (const auto& arg : call->args) {
                printExpr(arg, os, level + 1);
            }
            break;
        }

        default: {
            indent(os, level);
            os << "<unknown expr>\n";
            break;
        }
    }
}

} // namespace

void ASTPrinter::print(const ASTNodePtr& node, std::ostream& os) {
    printNode(node, os, 0);
}

} // namespace toycc