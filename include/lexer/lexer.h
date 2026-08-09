#pragma once

#include <string>
#include <vector>

#include "common/token.h"

namespace toycc {

// 词法分析器：把 ToyC 源代码字符串拆成 Token 序列
class Lexer {
public:
    // 构造时传入完整源代码
    explicit Lexer(std::string source);

    // 执行词法分析，返回 Token 序列（末尾自动加 EndOfFile）
    std::vector<Token> tokenize();

private:
    std::string source_;
    std::size_t pos_ = 0;   // 当前扫描位置
    int line_ = 1;          // 当前行号
    int column_ = 1;        // 当前列号

    // 辅助函数
    char peek() const;              // 看当前字符（不前进）
    char peekNext() const;          // 看下一个字符（不前进）
    char advance();                 // 读一个字符并前进，同时更新行列号
    bool isAtEnd() const;
    void skipWhitespaceAndComments(); // 跳过空白和注释

    // 生成不同类型 Token 的子函数
    Token makeIdentifierOrKeyword();
    Token makeNumber();
    Token makeOperatorOrPunctuator();

    // 关键字表查表
    static TokenType keywordType(const std::string& word);
};

} // namespace toycc
