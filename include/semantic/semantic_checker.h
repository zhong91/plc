#pragma once

#include <stdexcept>
#include <string>

#include "ast/ast.h"

namespace toycc {

class SemanticError : public std::runtime_error {
public:
    explicit SemanticError(const std::string& message)
        : std::runtime_error(message) {}
};

class SemanticChecker {
public:
    // Checks the complete ToyC AST. The AST is not modified.
    // Throws SemanticError on the first semantic error.
    void check(const ASTNodePtr& root) const;
};

} // namespace toycc
