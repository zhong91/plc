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

    std::vector<Param> params;
    if (!check(TokenType::RParen)) {
        params = parseParams();
    }

    consume(TokenType::RParen, "Expected ')' after function parameters.");

    auto body = parseBlock();

    return std::make_shared<FunctionDef>(
        returnType,
        nameToken.lexeme,
        params,
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

std::vector<Param> Parser::parseParams() {
    std::vector<Param> params;

    params.push_back(parseParam());

    while (match(TokenType::Comma)) {
        params.push_back(parseParam());
    }

    return params;
}

Param Parser::parseParam() {
    consume(TokenType::KwInt, "Expected 'int' in function parameter.");

    Token nameToken = consume(TokenType::Identifier, "Expected parameter name.");

    return Param(ValueType::Int, nameToken.lexeme);
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
    if (match(TokenType::Semicolon)) {
        return std::make_shared<EmptyStmt>();
    }

    if (match(TokenType::KwReturn)) {
        ExprPtr value = nullptr;

        if (!check(TokenType::Semicolon)) {
            value = parseExpr();
        }

        consume(TokenType::Semicolon, "Expected ';' after return statement.");

        return std::make_shared<ReturnStmt>(value);
    }

    if (match(TokenType::KwIf)) {
    return parseIfStmt();
}

    if (match(TokenType::KwConst)) {
        return parseVarDeclStmt(true);
    }

    if (check(TokenType::KwInt)) {
        return parseVarDeclStmt(false);
    }

    if (check(TokenType::LBrace)) {
        return parseBlock();
    }

    if (check(TokenType::Identifier) && checkNext(TokenType::Assign)) {
        return parseAssignStmt();
    }

    ExprPtr expr = parseExpr();
    consume(TokenType::Semicolon, "Expected ';' after expression statement.");
    return std::make_shared<ExprStmt>(expr);
}

StmtPtr Parser::parseIfStmt() {
    consume(TokenType::LParen, "Expected '(' after if.");

    ExprPtr condition = parseExpr();

    consume(TokenType::RParen, "Expected ')' after if condition.");

    StmtPtr thenBranch = parseStmt();

    StmtPtr elseBranch = nullptr;
    if (match(TokenType::KwElse)) {
        elseBranch = parseStmt();
    }

    return std::make_shared<IfStmt>(
        condition,
        thenBranch,
        elseBranch
    );
}

StmtPtr Parser::parseVarDeclStmt(bool isConst) {
    consume(TokenType::KwInt, "Expected 'int' in variable declaration.");

    Token nameToken = consume(TokenType::Identifier, "Expected variable name.");

    consume(TokenType::Assign, "Expected '=' in variable declaration.");

    ExprPtr init = parseExpr();

    consume(TokenType::Semicolon, "Expected ';' after variable declaration.");

    return std::make_shared<VarDeclStmt>(
        isConst,
        nameToken.lexeme,
        init
    );
}

StmtPtr Parser::parseAssignStmt() {
    Token nameToken = consume(TokenType::Identifier, "Expected variable name.");

    consume(TokenType::Assign, "Expected '=' in assignment statement.");

    ExprPtr value = parseExpr();

    consume(TokenType::Semicolon, "Expected ';' after assignment statement.");

    return std::make_shared<AssignStmt>(
        nameToken.lexeme,
        value
    );
}

ExprPtr Parser::parseExpr() {
    return parseLOrExpr();
}

ExprPtr Parser::parseLOrExpr() {
    ExprPtr expr = parseLAndExpr();

    while (match(TokenType::LogicalOr)) {
        ExprPtr right = parseLAndExpr();
        expr = std::make_shared<BinaryExpr>("||", expr, right);
    }

    return expr;
}

ExprPtr Parser::parseLAndExpr() {
    ExprPtr expr = parseRelExpr();

    while (match(TokenType::LogicalAnd)) {
        ExprPtr right = parseRelExpr();
        expr = std::make_shared<BinaryExpr>("&&", expr, right);
    }

    return expr;
}

ExprPtr Parser::parseRelExpr() {
    ExprPtr expr = parseAddExpr();

    while (true) {
        if (match(TokenType::Less)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>("<", expr, right);
        } else if (match(TokenType::Greater)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>(">", expr, right);
        } else if (match(TokenType::LessEqual)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>("<=", expr, right);
        } else if (match(TokenType::GreaterEqual)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>(">=", expr, right);
        } else if (match(TokenType::Equal)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>("==", expr, right);
        } else if (match(TokenType::NotEqual)) {
            ExprPtr right = parseAddExpr();
            expr = std::make_shared<BinaryExpr>("!=", expr, right);
        } else {
            break;
        }
    }

    return expr;
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
        std::string name = previous().lexeme;

        if (match(TokenType::LParen)) {
            std::vector<ExprPtr> args;

            if (!check(TokenType::RParen)) {
                args = parseArgs();
            }

            consume(TokenType::RParen, "Expected ')' after function arguments.");

            return std::make_shared<FunctionCall>(name, args);
        }

        return std::make_shared<Variable>(name);
    }

    if (match(TokenType::LParen)) {
        ExprPtr expr = parseExpr();
        consume(TokenType::RParen, "Expected ')' after expression.");
        return expr;
    }

    throw std::runtime_error("Expected expression.");
}

std::vector<ExprPtr> Parser::parseArgs() {
    std::vector<ExprPtr> args;

    args.push_back(parseExpr());

    while (match(TokenType::Comma)) {
        args.push_back(parseExpr());
    }

    return args;
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

bool Parser::checkNext(TokenType type) const {
    if (current + 1 >= tokens.size()) {
        return false;
    }

    return tokens[current + 1].type == type;
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