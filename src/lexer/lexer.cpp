#include "lexer/lexer.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace toycc {

Lexer::Lexer(std::string source)
    : source_(std::move(source)) {}

// ---------- 辅助函数 ----------

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[pos_];
}

char Lexer::peekNext() const {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

bool Lexer::isAtEnd() const {
    return pos_ >= source_.size();
}

// 读一个字符并前进，遇到换行要更新行列号
char Lexer::advance() {
    char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

// 跳过空格、制表符、换行，以及 // 和 /* */ 注释
void Lexer::skipWhitespaceAndComments() {
    while (!isAtEnd()) {
        char c = peek();

        // 普通空白字符
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }

        // 行注释 //
        if (c == '/' && peekNext() == '/') {
            // 一直读到行尾
            while (!isAtEnd() && peek() != '\n') {
                advance();
            }
            continue;
        }

        // 块注释 /* ... */
        if (c == '/' && peekNext() == '*') {
            advance(); // 吃掉 '/'
            advance(); // 吃掉 '*'
            while (!isAtEnd()) {
                if (peek() == '*' && peekNext() == '/') {
                    advance(); // 吃掉 '*'
                    advance(); // 吃掉 '/'
                    break;
                }
                advance();
            }
            continue;
        }

        // 不是空白也不是注释，退出
        break;
    }
}

// 关键字表：把单词字符串映射到 TokenType
TokenType Lexer::keywordType(const std::string& word) {
    static const std::unordered_map<std::string, TokenType> table = {
        {"const",    TokenType::KwConst},
        {"int",      TokenType::KwInt},
        {"void",     TokenType::KwVoid},
        {"if",       TokenType::KwIf},
        {"else",     TokenType::KwElse},
        {"while",    TokenType::KwWhile},
        {"break",    TokenType::KwBreak},
        {"continue", TokenType::KwContinue},
        {"return",   TokenType::KwReturn},
    };

    auto it = table.find(word);
    if (it != table.end()) {
        return it->second;
    }
    return TokenType::Identifier; // 不是关键字就是普通标识符
}

// ---------- 生成 Token 的子函数 ----------

// 处理字母/下划线开头的单词
Token Lexer::makeIdentifierOrKeyword() {
    int startLine = line_;
    int startCol = column_;
    std::string word;

    // 字母、数字、下划线都可以作为标识符内部字符
    while (!isAtEnd() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
        word += advance();
    }

    TokenType type = keywordType(word);
    return Token(type, word, startLine, startCol);
}

// 处理数字（目前只支持非负整数）
Token Lexer::makeNumber() {
    int startLine = line_;
    int startCol = column_;
    std::string num;

    while (!isAtEnd() && std::isdigit(static_cast<unsigned char>(peek()))) {
        num += advance();
    }

    return Token(TokenType::Number, num, startLine, startCol);
}

// 处理运算符和分隔符
Token Lexer::makeOperatorOrPunctuator() {
    int startLine = line_;
    int startCol = column_;
    char c = advance(); // 先读掉首字符

    switch (c) {
        // 单字符运算符 / 分隔符
        case '+': return Token(TokenType::Plus, "+", startLine, startCol);
        case '-': return Token(TokenType::Minus, "-", startLine, startCol);
        case '*': return Token(TokenType::Star, "*", startLine, startCol);
        case '/': return Token(TokenType::Slash, "/", startLine, startCol);
        case '%': return Token(TokenType::Percent, "%", startLine, startCol);
        case '(': return Token(TokenType::LParen, "(", startLine, startCol);
        case ')': return Token(TokenType::RParen, ")", startLine, startCol);
        case '{': return Token(TokenType::LBrace, "{", startLine, startCol);
        case '}': return Token(TokenType::RBrace, "}", startLine, startCol);
        case ',': return Token(TokenType::Comma, ",", startLine, startCol);
        case ';': return Token(TokenType::Semicolon, ";", startLine, startCol);

        // 可能是双字符的运算符
        case '=':
            if (peek() == '=') {
                advance();
                return Token(TokenType::Equal, "==", startLine, startCol);
            }
            return Token(TokenType::Assign, "=", startLine, startCol);

        case '!':
            if (peek() == '=') {
                advance();
                return Token(TokenType::NotEqual, "!=", startLine, startCol);
            }
            return Token(TokenType::LogicalNot, "!", startLine, startCol);

        case '<':
            if (peek() == '=') {
                advance();
                return Token(TokenType::LessEqual, "<=", startLine, startCol);
            }
            return Token(TokenType::Less, "<", startLine, startCol);

        case '>':
            if (peek() == '=') {
                advance();
                return Token(TokenType::GreaterEqual, ">=", startLine, startCol);
            }
            return Token(TokenType::Greater, ">", startLine, startCol);

        case '&':
            if (peek() == '&') {
                advance();
                return Token(TokenType::LogicalAnd, "&&", startLine, startCol);
            }
            break; // 单个 & 不支持，报错

        case '|':
            if (peek() == '|') {
                advance();
                return Token(TokenType::LogicalOr, "||", startLine, startCol);
            }
            break; // 单个 | 不支持，报错

        default:
            break;
    }

    // 走到这里说明遇到了无法识别的字符
    throw std::runtime_error(
        "Lexer error at line " + std::to_string(startLine) +
        ", column " + std::to_string(startCol) +
        ": unexpected character '" + std::string(1, c) + "'"
    );
}

// ---------- 主入口 ----------

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    while (true) {
        skipWhitespaceAndComments();
        if (isAtEnd()) break;

        char c = peek();

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(makeIdentifierOrKeyword());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(makeNumber());
        } else {
            tokens.push_back(makeOperatorOrPunctuator());
        }
    }

    // 文档要求：末尾必须有一个 EndOfFile Token
    tokens.emplace_back(TokenType::EndOfFile, "", line_, column_);
    return tokens;
}

} // namespace toycc
