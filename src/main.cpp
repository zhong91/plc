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
        toycc::Token(toycc::TokenType::Greater, ">", 2, 22),
        toycc::Token(toycc::TokenType::Number, "5", 2, 24),
        toycc::Token(toycc::TokenType::LogicalAnd, "&&", 2, 26),
        toycc::Token(toycc::TokenType::Number, "4", 2, 29),
        toycc::Token(toycc::TokenType::NotEqual, "!=", 2, 31),
        toycc::Token(toycc::TokenType::Number, "0", 2, 34),
        toycc::Token(toycc::TokenType::Semicolon, ";", 2, 35),

        toycc::Token(toycc::TokenType::RBrace, "}", 3, 1),
        toycc::Token(toycc::TokenType::EndOfFile, "", 3, 2)
    };

    toycc::Parser parser(tokens);
    toycc::ASTNodePtr ast = parser.parse();

    if (ast != nullptr) {
        std::cerr << "Parser parsed logical expression successfully.\n";
    }

    return 0;
}