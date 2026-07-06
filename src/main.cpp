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

        // while (x < 10) {
        toycc::Token(toycc::TokenType::KwWhile, "while", 3, 5),
        toycc::Token(toycc::TokenType::LParen, "(", 3, 11),
        toycc::Token(toycc::TokenType::Identifier, "x", 3, 12),
        toycc::Token(toycc::TokenType::Less, "<", 3, 14),
        toycc::Token(toycc::TokenType::Number, "10", 3, 16),
        toycc::Token(toycc::TokenType::RParen, ")", 3, 18),
        toycc::Token(toycc::TokenType::LBrace, "{", 3, 20),

        // x = x + 1;
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 9),
        toycc::Token(toycc::TokenType::Assign, "=", 4, 11),
        toycc::Token(toycc::TokenType::Identifier, "x", 4, 13),
        toycc::Token(toycc::TokenType::Plus, "+", 4, 15),
        toycc::Token(toycc::TokenType::Number, "1", 4, 17),
        toycc::Token(toycc::TokenType::Semicolon, ";", 4, 18),

        // if (x == 3) {
        toycc::Token(toycc::TokenType::KwIf, "if", 5, 9),
        toycc::Token(toycc::TokenType::LParen, "(", 5, 12),
        toycc::Token(toycc::TokenType::Identifier, "x", 5, 13),
        toycc::Token(toycc::TokenType::Equal, "==", 5, 15),
        toycc::Token(toycc::TokenType::Number, "3", 5, 18),
        toycc::Token(toycc::TokenType::RParen, ")", 5, 19),
        toycc::Token(toycc::TokenType::LBrace, "{", 5, 21),

        // continue;
        toycc::Token(toycc::TokenType::KwContinue, "continue", 6, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 6, 21),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 7, 9),

        // if (x == 5) {
        toycc::Token(toycc::TokenType::KwIf, "if", 8, 9),
        toycc::Token(toycc::TokenType::LParen, "(", 8, 12),
        toycc::Token(toycc::TokenType::Identifier, "x", 8, 13),
        toycc::Token(toycc::TokenType::Equal, "==", 8, 15),
        toycc::Token(toycc::TokenType::Number, "5", 8, 18),
        toycc::Token(toycc::TokenType::RParen, ")", 8, 19),
        toycc::Token(toycc::TokenType::LBrace, "{", 8, 21),

        // break;
        toycc::Token(toycc::TokenType::KwBreak, "break", 9, 13),
        toycc::Token(toycc::TokenType::Semicolon, ";", 9, 18),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 10, 9),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 11, 5),

        // return x;
        toycc::Token(toycc::TokenType::KwReturn, "return", 12, 5),
        toycc::Token(toycc::TokenType::Identifier, "x", 12, 12),
        toycc::Token(toycc::TokenType::Semicolon, ";", 12, 13),

        // }
        toycc::Token(toycc::TokenType::RBrace, "}", 13, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 13, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed break and continue statements successfully.\n";
    }

    return 0;
}