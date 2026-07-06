#pragma once

#include <memory>
#include <vector>
#include <string>

#include "common/token.h"
#include "ast/ast.h"

namespace toycc {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    ASTNodePtr parse();

private:
    std::vector<Token> tokens;
    std::size_t current = 0;

    const Token& peek() const;
    const Token& previous() const;

    bool isAtEnd() const;
    bool check(TokenType type) const;
    bool match(TokenType type);

    const Token& advance();
    const Token& consume(TokenType type, const std::string& message);
};

} // namespace toycc