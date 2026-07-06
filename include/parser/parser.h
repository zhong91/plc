#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "ast/ast.h"
#include "common/token.h"

namespace toycc {

class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    ASTNodePtr parse();

private:
    std::vector<Token> tokens;
    std::size_t current = 0;

    std::shared_ptr<CompUnit> parseCompUnit();
    std::shared_ptr<FunctionDef> parseFunctionDef();
    ValueType parseType();
    std::shared_ptr<Block> parseBlock();
    StmtPtr parseStmt();
    ExprPtr parseExpr();
    ExprPtr parsePrimaryExpr();

    const Token& peek() const;
    const Token& previous() const;

    bool isAtEnd() const;
    bool check(TokenType type) const;
    bool match(TokenType type);

    const Token& advance();
    const Token& consume(TokenType type, const std::string& message);
};

} // namespace toycc