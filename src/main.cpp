#include <iostream>
#include <vector>

#include "common/token.h"
#include "parser/parser.h"

int main() {
    std::vector<toycc::Token> tokens = {
        toycc::Token(toycc::TokenType::KwInt, "int", 1, 1),
        toycc::Token(toycc::TokenType::Identifier, "main", 1, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 1, 9),
        toycc::Token(toycc::TokenType::RParen, ")", 1, 10),
        toycc::Token(toycc::TokenType::LBrace, "{", 1, 12),

        toycc::Token(toycc::TokenType::KwInt, "int", 2, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 2, 9),
        toycc::Token(toycc::TokenType::Assign, "=", 2, 11),
        toycc::Token(toycc::TokenType::Number, "1", 2, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 14),

        toycc::Token(toycc::TokenType::KwIf, "if", 3, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 3, 8),
        toycc::Token(toycc::TokenType::Identifier, "x", 3, 9),
        toycc::Token(toycc::TokenType::Greater, ">", 3, 11),
        toycc::Token(toycc::TokenType::Number, "0", 3, 13),
        toycc::Token(toycc::TokenType::RParen, ")", 3, 14),
        toycc::Token(toycc::TokenType::LBrace, "{", 3, 16),

        toycc::Token(toycc::TokenType::KwReturn, "return", 4, 9),
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 16),
        toycc::Token(toycc::TokenType::Semicolon, ";", 4, 17),

        toycc::Token(toycc::TokenType::RBrace, "}", 5, 5),
        toycc::Token(toycc::TokenType::KwElse, "else", 5, 7),
        toycc::Token(toycc::TokenType::LBrace, "{", 5, 12),

        toycc::Token(toycc::TokenType::KwReturn, "return", 6, 9),
        toycc::Token(toycc::TokenType::Number, "0", 6, 16),
        toycc::Token(toycc::TokenType::Semicolon, ";", 6, 17),

        toycc::Token(toycc::TokenType::RBrace, "}", 7, 5),
        toycc::Token(toycc::TokenType::RBrace, "}", 8, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 8, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed if else statement successfully.\n";
    }

    return 0;
}