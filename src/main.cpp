#include <iostream>
#include <vector>

#include "common/token.h"
#include "parser/parser.h"

int main() {
    std::vector<toycc::Token> tokens = {
        // int add(int a, int b) {
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 1),
        toycc::Token(toycc::TokenType::Identifier, "add", 1, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 1, 8),
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 9),
        toycc::Token(toycc::TokenType::Identifier, "a", 1, 13),
        toycc::Token(toycc::TokenType::Comma, ",", 1, 14),
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 16),
        toycc::Token(toycc::TokenType::Identifier, "b", 1, 20),
        toycc::Token(toycc::TokenType::RParen, ")", 1, 21),
        toycc::Token(toycc::TokenType::LBrace, "{", 1, 23),

        // return a + b;
        toycc::Token(toycc::TokenType::KwReturn, "return", 2, 5),
        toycc::Token(toycc::TokenType::Identifier, "a", 2, 12),
        toycc::Token(toycc::TokenType::Plus, "+", 2, 14),
        toycc::Token(toycc::TokenType::Identifier, "b", 2, 16),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 17),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 3, 1),

        // int main() {
        toycc::Token(toycc::TokenType::KwInt, "int", 5, 1),
        toycc::Token(toycc::TokenType::Identifier, "main", 5, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 5, 9),
        toycc::Token(toycc::TokenType::RParen, ")", 5, 10),
        toycc::Token(toycc::TokenType::LBrace, "{", 5, 12),

        // return add(1, 2);
        toycc::Token(toycc::TokenType::KwReturn, "return", 6, 5),
        toycc::Token(toycc::TokenType::Identifier, "add", 6, 12),
        toycc::Token(toycc::TokenType::LParen, "(", 6, 15),
        toycc::Token(toycc::TokenType::Number, "1", 6, 16),
        toycc::Token(toycc::TokenType::Comma, ",", 6, 17),
        toycc::Token(toycc::TokenType::Number, "2", 6, 19),
        toycc::Token(toycc::TokenType::RParen, ")", 6, 20),
        toycc::Token(toycc::TokenType::Semicolon, ";", 6, 21),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 7, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 7, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed function parameters and calls successfully.\n";
    }

    return 0;
}