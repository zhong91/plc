#pragma once

#include <string>
#include <utility>

namespace toycc {

enum class TokenType {
    EndOfFile,

    Identifier,
    Number,

    KwConst,
    KwInt,
    KwVoid,
    KwIf,
    KwElse,
    KwWhile,
    KwBreak,
    KwContinue,
    KwReturn,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    Assign,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,

    LogicalAnd,
    LogicalOr,
    LogicalNot,

    LParen,
    RParen,
    LBrace,
    RBrace,
    Comma,
    Semicolon,

    Unknown
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;

    Token(TokenType type, std::string lexeme, int line, int column)
        : type(type), lexeme(std::move(lexeme)), line(line), column(column) {}
};

} // namespace toycc