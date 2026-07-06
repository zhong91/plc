#include "parser/parser.h"

#include <sstream>
#include <stdexcept>

namespace toycc {

Parser::Parser(const std::vector<Token>& tokens)
    : tokens(tokens) {
    if (this->tokens.empty() ||
        this->tokens.back().type != TokenType::EndOfFile) {
        this->tokens.emplace_back(TokenType::EndOfFile, "", 0, 0);
    }
}

ASTNodePtr Parser::parse() {
    return parseCompUnit();
}

std::shared_ptr<CompUnit> Parser::parseCompUnit() {
    auto compUnit = std::make_shared<CompUnit>();

    while (!isAtEnd()) {
        compUnit->units.push_back(parseFunctionDef());
    }

    return compUnit;
}

std::shared_ptr<FunctionDef> Parser::parseFunctionDef() {
    ValueType returnType = parseType();

    Token nameToken = consume(TokenType::Identifier, "Expected function name.");

    consume(TokenType::LParen, "Expected '(' after function name.");
    consume(TokenType::RParen, "Expected ')' after function parameters.");

    auto body = parseBlock();

    return std::make_shared<FunctionDef>(
        returnType,
        nameToken.lexeme,
        body
    );
}

ValueType Parser::parseType() {
    if (match(TokenType::KwInt)) {
        return ValueType::Int;
    }

    if (match(TokenType::KwVoid)) {
        return ValueType::Void;
    }

    throw std::runtime_error("Expected type name: int or void.");
}

std::shared_ptr<Block> Parser::parseBlock() {
    consume(TokenType::LBrace, "Expected '{' before block.");

    auto block = std::make_shared<Block>();

    while (!check(TokenType::RBrace) && !isAtEnd()) {
        block->statements.push_back(parseStmt());
    }

    consume(TokenType::RBrace, "Expected '}' after block.");

    return block;
}

StmtPtr Parser::parseStmt() {
    if (match(TokenType::KwReturn)) {
        ExprPtr value = nullptr;

        if (!check(TokenType::Semicolon)) {
            value = parseExpr();
        }

        consume(TokenType::Semicolon, "Expected ';' after return statement.");

        return std::make_shared<ReturnStmt>(value);
    }

    if (check(TokenType::LBrace)) {
        return parseBlock();
    }

    throw std::runtime_error("Unsupported statement.");
}

ExprPtr Parser::parseExpr() {
    return parseAddExpr();
}

ExprPtr Parser::parseAddExpr() {
    ExprPtr expr = parseMulExpr();

    while (true) {
        if (match(TokenType::Plus)) {
            ExprPtr right = parseMulExpr();
            expr = std::make_shared<BinaryExpr>("+", expr, right);
        } else if (match(TokenType::Minus)) {
            ExprPtr right = parseMulExpr();
            expr = std::make_shared<BinaryExpr>("-", expr, right);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::parseMulExpr() {
    ExprPtr expr = parseUnaryExpr();

    while (true) {
        if (match(TokenType::Star)) {
            ExprPtr right = parseUnaryExpr();
            expr = std::make_shared<BinaryExpr>("*", expr, right);
        } else if (match(TokenType::Slash)) {
            ExprPtr right = parseUnaryExpr();
            expr = std::make_shared<BinaryExpr>("/", expr, right);
        } else if (match(TokenType::Percent)) {
            ExprPtr right = parseUnaryExpr();
            expr = std::make_shared<BinaryExpr>("%", expr, right);
        } else {
            break;
        }
    }

    return expr;
}

ExprPtr Parser::parseUnaryExpr() {
    if (match(TokenType::Plus)) {
        ExprPtr operand = parseUnaryExpr();
        return std::make_shared<UnaryExpr>("+", operand);
    }

    if (match(TokenType::Minus)) {
        ExprPtr operand = parseUnaryExpr();
        return std::make_shared<UnaryExpr>("-", operand);
    }

    if (match(TokenType::LogicalNot)) {
        ExprPtr operand = parseUnaryExpr();
        return std::make_shared<UnaryExpr>("!", operand);
    }

    return parsePrimaryExpr();
}

ExprPtr Parser::parsePrimaryExpr() {
    if (match(TokenType::Number)) {
        int value = std::stoi(previous().lexeme);
        return std::make_shared<NumberLiteral>(value);
    }

    if (match(TokenType::Identifier)) {
        return std::make_shared<Variable>(previous().lexeme);
    }

    if (match(TokenType::LParen)) {
        ExprPtr expr = parseExpr();
        consume(TokenType::RParen, "Expected ')' after expression.");
        return expr;
    }

    throw std::runtime_error("Expected expression.");
}

const Token& Parser::peek() const {
    return tokens[current];
}

const Token& Parser::previous() const {
    return tokens[current - 1];
}

bool Parser::isAtEnd() const {
    return peek().type == TokenType::EndOfFile;
}

bool Parser::check(TokenType type) const {
    if (isAtEnd()) {
        return type == TokenType::EndOfFile;
    }

    return peek().type == type;
}

bool Parser::match(TokenType type) {
    if (check(type)) {
        advance();
        return true;
    }

    return false;
}

const Token& Parser::advance() {
    if (!isAtEnd()) {
        current++;
    }

    return previous();
}

const Token& Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) {
        return advance();
    }

    const Token& token = peek();

    std::ostringstream oss;
    oss << "Parser error at line "
        << token.line
        << ", column "
        << token.column
        << ": "
        << message;

    throw std::runtime_error(oss.str());
}

} // namespace toycc