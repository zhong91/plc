#include <iostream>
#include <vector>

#include "common/token.h"
#include "parser/parser.h"

int main() {
    std::vector<toycc::Token> tokens = {
        toycc::Token(toycc::TokenType::EndOfFile, "", 1, 1)
    };

    toycc::Parser parser(tokens);
    parser.parse();

    std::cerr << "Parser basic test passed.\n";

    return 0;
}