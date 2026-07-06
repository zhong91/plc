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
    std::vector<Param> parseParams();
    Param parseParam();

    std::shared_ptr<Block> parseBlock();
    StmtPtr parseStmt();
    StmtPtr parseVarDeclStmt(bool isConst);
    StmtPtr parseAssignStmt();

    ExprPtr parseExpr();
    ExprPtr parseLOrExpr();
    ExprPtr parseLAndExpr();
    ExprPtr parseRelExpr();
    ExprPtr parseAddExpr();
    ExprPtr parseMulExpr();
    ExprPtr parseUnaryExpr();
    ExprPtr parsePrimaryExpr();

    std::vector<ExprPtr> parseArgs();

    const Token& peek() const;
    const Token& previous() const;

    bool isAtEnd() const;
    bool check(TokenType type) const;
    bool checkNext(TokenType type) const;
    bool match(TokenType type);

    const Token& advance();
    const Token& consume(TokenType type, const std::string& message);
};

} // namespace toycc