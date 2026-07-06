#pragma once

#include <iosfwd>

#include "ast/ast.h"

namespace toycc {

class ASTPrinter {
public:
    static void print(const ASTNodePtr& node, std::ostream& os);
};

} // namespace toycc