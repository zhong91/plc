#include <iostream>
#include <vector>

#include "common/token.h"
#include "parser/parser.h"

int main() {
    std::vector<toycc::Token> tokens = {
        // int main() {
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 1),
        toycc::Token(toycc::TokenType::Identifier, "main", 1, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 1, 9),
        toycc::Token(toycc::TokenType::RParen, ")", 1, 10),
        toycc::Token(toycc::TokenType::LBrace, "{", 1, 12),

        // int x = 1;
        toycc::Token(toycc::TokenType::KwInt, "int", 2, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 2, 9),
        toycc::Token(toycc::TokenType::Assign, "=", 2, 11),
        toycc::Token(toycc::TokenType::Number, "1", 2, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 14),

        // x = x + 2;
        toycc::Token(toycc::TokenType::Identifier, "x", 3, 5),
        toycc::Token(toycc::TokenType::Assign, "=", 3, 7),
        toycc::Token(toycc::TokenType::Identifier, "x", 3, 9),
        toycc::Token(toycc::TokenType::Plus, "+", 3, 11),
        toycc::Token(toycc::TokenType::Number, "2", 3, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 3, 14),

        // return x;
        toycc::Token(toycc::TokenType::KwReturn, "return", 4, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 12),
        toycc::Token(toycc::TokenType::Semicolon, ";", 4, 13),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 5, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 5, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed variable declaration and assignment successfully.\n";
    }

    return 0;
}