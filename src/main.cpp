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

        // int x = 0;
        toycc::Token(toycc::TokenType::KwInt, "int", 2, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 2, 9),
        toycc::Token(toycc::TokenType::Assign, "=", 2, 11),
        toycc::Token(toycc::TokenType::Number, "0", 2, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 14),

        // while (x < 3) {
        toycc::Token(toycc::TokenType::KwWhile, "while", 3, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 3, 11),
        toycc::Token(toycc::TokenType::Identifier, "x", 3, 12),
        toycc::Token(toycc::TokenType::Less, "<", 3, 14),
        toycc::Token(toycc::TokenType::Number, "3", 3, 16),
        toycc::Token(toycc::TokenType::RParen, ")", 3, 17),
        toycc::Token(toycc::TokenType::LBrace, "{", 3, 19),

        // x = x + 1;
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 9),
        toycc::Token(toycc::TokenType::Assign, "=", 4, 11),
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 13),
        toycc::Token(toycc::TokenType::Plus, "+", 4, 15),
        toycc::Token(toycc::TokenType::Number, "1", 4, 17),
        toycc::Token(toycc::TokenType::Semicolon, ";", 4, 18),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 5, 5),

        // return x;
        toycc::Token(toycc::TokenType::KwReturn, "return", 6, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 6, 12),
        toycc::Token(toycc::TokenType::Semicolon, ";", 6, 13),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 7, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 7, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed while statement successfully.\n";
    }

    return 0;
}