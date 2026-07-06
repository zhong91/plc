#include <iostream>
#include <vector>

#include "ast/ast_printer.h"
#include "common/token.h"
#include "parser/parser.h"

int main() {
    std::vector<toycc::Token> tokens = {
        // const int A = 1;
        toycc::Token(toycc::TokenType::KwConst, "const", 1, 1),
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 7),
        toycc::Token(toycc::TokenType::Identifier, "A", 1, 11),
        toycc::Token(toycc::TokenType::Assign, "=", 1, 13),
        toycc::Token(toycc::TokenType::Number, "1", 1, 15),
        toycc::Token(toycc::TokenType::Semicolon, ";", 1, 16),

        // int g = 2;
        toycc::Token(toycc::TokenType::KwInt, "int", 2, 1),
        toycc::Token(toycc::TokenType::Identifier, "g", 2, 5),
        toycc::Token(toycc::TokenType::Assign, "=", 2, 7),
        toycc::Token(toycc::TokenType::Number, "2", 2, 9),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 10),

        // int main() {
        toycc::Token(toycc::TokenType::KwInt, "int", 4, 1),
        toycc::Token(toycc::TokenType::Identifier, "main", 4, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 4, 9),
        toycc::Token(toycc::TokenType::RParen, ")", 4, 10),
        toycc::Token(toycc::TokenType::LBrace, "{", 4, 12),

        // return A + g;
        toycc::Token(toycc::TokenType::KwReturn, "return", 5, 5),
        toycc::Token(toycc::TokenType::Identifier, "A", 5, 12),
        toycc::Token(toycc::TokenType::Plus, "+", 5, 14),
        toycc::Token(toycc::TokenType::Identifier, "g", 5, 16),
        toycc::Token(toycc::TokenType::Semicolon, ";", 5, 17),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 6, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 6, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();
    toycc::ASTPrinter::print(ast, std::cerr);

    if (ast != nullptr) {
        std::cerr << "Parser parsed global declarations successfully.\n";
    }

    return 0;
}