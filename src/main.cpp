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

        toycc::Token(toycc::TokenType::KwReturn, "return", 2, 5),
        toycc::Token(toycc::TokenType::Number, "1", 2, 12),
        toycc::Token(toycc::TokenType::Plus, "+", 2, 14),
        toycc::Token(toycc::TokenType::Number, "2", 2, 16),
        toycc::Token(toycc::TokenType::Star, "*", 2, 18),
        toycc::Token(toycc::TokenType::Number, "3", 2, 20),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 21),

        toycc::Token(toycc::TokenType::RBrace, "}", 3, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 3, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed expression 1 + 2 * 3 successfully.\n";
    }

    return 0;
}