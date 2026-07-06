#include "ast/ast_printer.h"

#include <ostream>

namespace toycc {

void ASTPrinter::print(const ASTNodePtr& node, std::ostream& os) {
    if (node == nullptr) {
        os << "<null ast>\n";
        return;
    }

    os << "AST printer is working.\n";
}

} // namespace toycc
