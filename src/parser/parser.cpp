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
    // 现在先返回空。
    // 后面我们会把这里改成真正解析整个 ToyC 程序。
    return nullptr;
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

    if (current == 0) {
        return tokens[0];
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