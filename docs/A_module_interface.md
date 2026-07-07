##  注意事项!!!

1. 所有公共类型都在 `namespace toycc` 下。
2. 头文件统一放在 `include/` 下。
3. 源文件统一放在 `src/` 下。
4. Parser 只依赖 Token，不依赖 Lexer 内部实现。
5. Semantic 和 IR 都应该从 AST 根节点 `CompUnit` 开始遍历。
6. 调试信息输出到 `std::cerr`。
7. 最终 RISC-V 汇编必须输出到 `std::cout`。
8. 当前 Parser 报错方式是抛出 `std::runtime_error`。
9. 当前 AST 使用 `std::shared_ptr` 管理节点。
10. 当前 main.cpp 只是测试入口，不是最终主流程。

# A 模块接口说明：项目总控 + AST + Parser

本文档用于说明 A 模块目前已经完成的内容，以及 B / C / D 三个模块后续如何与 A 模块对接。

当前小组分工：

- A：项目总控 + AST + Parser
- B：IR + RISC-V 后端
- C：Lexer + 测试
- D：Semantic + 报告

---

## 1. A 模块当前职责

A 模块目前已经完成：

- 公共 Token 定义
- AST 节点定义
- Parser 语法分析
- AST 打印调试工具
- 临时 main.cpp 测试入口

当前 Parser 的输入是 Token 序列，输出是 AST。

基本调用方式：

```cpp
std::vector<toycc::Token> tokens = ...;

toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

---

## 2. 当前项目结构

项目采用 C++20 + CMake。

主要目录结构如下：

```text
plc/
├── include/
│   ├── ast/
│   │   ├── ast.h
│   │   └── ast_printer.h
│   ├── common/
│   │   └── token.h
│   ├── lexer/
│   ├── parser/
│   │   └── parser.h
│   ├── semantic/
│   ├── ir/
│   └── codegen/
├── src/
│   ├── ast/
│   │   └── ast_printer.cpp
│   ├── lexer/
│   ├── parser/
│   │   └── parser.cpp
│   ├── semantic/
│   ├── ir/
│   ├── codegen/
│   └── main.cpp
├── docs/
├── tests/
└── CMakeLists.txt
```

---

## 3. 总体编译流程

最终完整流程设计如下：

```text
ToyC 源代码
    ↓
Lexer
    ↓
Token 序列
    ↓
Parser
    ↓
AST
    ↓
Semantic
    ↓
IR / CFG
    ↓
RISC-V Backend
    ↓
RISC-V32 汇编
```

当前 A 模块已经完成：

```text
Token 定义
Parser
AST
ASTPrinter
```

目前 `src/main.cpp` 仍然是临时测试入口，里面手动构造 Token。

等 C 同学的 Lexer 完成后，`main.cpp` 会改成从 stdin 读取 ToyC 源代码，再调用 Lexer。

---

## 4. 公共 Token 接口

Token 定义在：

```text
include/common/token.h
```

核心结构如下：

```cpp
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
};

}
```

---

## 5. 给 C 同学：Lexer 接口要求

C 同学负责 Lexer。

Lexer 的任务是把 ToyC 源代码转换成：

```cpp
std::vector<toycc::Token>
```

建议提供如下接口：

```cpp
#pragma once

#include <string>
#include <vector>

#include "common/token.h"

namespace toycc {

class Lexer {
public:
    explicit Lexer(const std::string& source);

    std::vector<Token> tokenize();

private:
    std::string source;
};

}
```

建议文件位置：

```text
include/lexer/lexer.h
src/lexer/lexer.cpp
```

Parser 对 Lexer 的唯一要求是：

```cpp
std::vector<toycc::Token> tokens = lexer.tokenize();

toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

Lexer 输出的 Token 序列最后必须有：

```cpp
TokenType::EndOfFile
```

例如：

```cpp
tokens.emplace_back(TokenType::EndOfFile, "", line, column);
```

Lexer 需要识别的关键字：

```text
const
int
void
if
else
while
break
continue
return
```

Lexer 需要识别的运算符：

```text
+
-
*
/
%
=
==
!=
<
<=
>
>=
&&
||
!
```

Lexer 需要识别的分隔符：

```text
(
)
{
}
,
;
```

Lexer 需要识别的基本单词：

```text
Identifier
Number
```

例如 ToyC 源码：

```c
int main() {
    return 123;
}
```

应生成类似 Token 序列：

```text
KwInt       "int"
Identifier  "main"
LParen      "("
RParen      ")"
LBrace      "{"
KwReturn    "return"
Number      "123"
Semicolon   ";"
RBrace      "}"
EndOfFile   ""
```

---

## 6. Parser 接口

Parser 定义在：

```text
include/parser/parser.h
src/parser/parser.cpp
```

Parser 使用方式：

```cpp
#include "parser/parser.h"

std::vector<toycc::Token> tokens = ...;

toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

`parse()` 会解析整个 ToyC 程序，并返回 AST 根节点。

正常情况下，根节点类型是：

```cpp
ASTNodeType::CompUnit
```

如果语法错误，Parser 会抛出：

```cpp
std::runtime_error
```

错误信息中会包含行号和列号。

---

## 7. AST 公共接口

AST 定义在：

```text
include/ast/ast.h
```

所有 AST 节点都在命名空间：

```cpp
namespace toycc
```

AST 基类：

```cpp
class ASTNode {
public:
    ASTNodeType type;
    virtual ~ASTNode() = default;
};
```

常用智能指针别名：

```cpp
using ASTNodePtr = std::shared_ptr<ASTNode>;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
```

---

## 8. AST 节点类型

当前已有节点包括：

### 8.1 顶层节点

```text
CompUnit
FunctionDef
Block
```

### 8.2 语句节点

```text
ReturnStmt
ExprStmt
EmptyStmt
VarDeclStmt
AssignStmt
IfStmt
WhileStmt
BreakStmt
ContinueStmt
```

### 8.3 表达式节点

```text
NumberLiteral
Variable
UnaryExpr
BinaryExpr
FunctionCall
```

---

## 9. 重要 AST 结构说明

### 9.1 CompUnit：整个程序

```cpp
class CompUnit : public ASTNode {
public:
    std::vector<ASTNodePtr> units;
};
```

`units` 里面可以放：

```text
全局变量声明
全局常量声明
函数定义
```

---

### 9.2 FunctionDef：函数定义

```cpp
class FunctionDef : public ASTNode {
public:
    ValueType returnType;
    std::string name;
    std::vector<Param> params;
    std::shared_ptr<Block> body;
};
```

对应 ToyC 代码：

```c
int add(int a, int b) {
    return a + b;
}
```

---

### 9.3 Param：函数参数

```cpp
struct Param {
    ValueType type;
    std::string name;
};
```

对应 ToyC 代码：

```c
int a
```

---

### 9.4 Block：代码块

```cpp
class Block : public Stmt {
public:
    std::vector<StmtPtr> statements;
};
```

对应 ToyC 代码：

```c
{
    int x = 1;
    return x;
}
```

---

### 9.5 VarDeclStmt：变量 / 常量声明

```cpp
class VarDeclStmt : public Stmt {
public:
    bool isConst;
    std::string name;
    ExprPtr init;
};
```

对应 ToyC 代码：

```c
int x = 1;
const int A = 2;
```

注意：当前 Parser 要求变量声明必须带初始化表达式。

---

### 9.6 AssignStmt：赋值语句

```cpp
class AssignStmt : public Stmt {
public:
    std::string name;
    ExprPtr value;
};
```

对应 ToyC 代码：

```c
x = x + 1;
```

---

### 9.7 ReturnStmt：return 语句

```cpp
class ReturnStmt : public Stmt {
public:
    ExprPtr value;
};
```

对应 ToyC 代码：

```c
return x;
return 0;
return;
```

其中：

```cpp
value == nullptr
```

表示空 return。

---

### 9.8 IfStmt：if / else

```cpp
class IfStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};
```

对应 ToyC 代码：

```c
if (x > 0) {
    return x;
} else {
    return 0;
}
```

如果没有 else：

```cpp
elseBranch == nullptr
```

---

### 9.9 WhileStmt：while 循环

```cpp
class WhileStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr body;
};
```

对应 ToyC 代码：

```c
while (x < 10) {
    x = x + 1;
}
```

---

### 9.10 BreakStmt / ContinueStmt

```cpp
class BreakStmt : public Stmt {};
class ContinueStmt : public Stmt {};
```

对应 ToyC 代码：

```c
break;
continue;
```

---

### 9.11 BinaryExpr：二元表达式

```cpp
class BinaryExpr : public Expr {
public:
    std::string op;
    ExprPtr left;
    ExprPtr right;
};
```

对应 ToyC 代码：

```c
a + b
x * y
x > 0
a && b
```

`op` 可能为：

```text
+
-
*
/
%
<
>
<=
>=
==
!=
&&
||
```

---

### 9.12 UnaryExpr：一元表达式

```cpp
class UnaryExpr : public Expr {
public:
    std::string op;
    ExprPtr operand;
};
```

对应 ToyC 代码：

```c
-a
+a
!a
```

`op` 可能为：

```text
+
-
!
```

---

### 9.13 FunctionCall：函数调用

```cpp
class FunctionCall : public Expr {
public:
    std::string name;
    std::vector<ExprPtr> args;
};
```

对应 ToyC 代码：

```c
add(1, 2)
foo()
```

---

## 10. 当前 Parser 已支持的 ToyC 语法

当前 Parser 已经支持以下语法：

```c
const int A = 1;
int g = 2;

int add(int a, int b) {
    return a + b;
}

int main() {
    int x = 0;

    while (x < 10) {
        x = x + 1;

        if (x == 3) {
            continue;
        }

        if (x == 5) {
            break;
        }
    }

    return A + g + add(x, 1);
}
```

具体包括：

```text
全局变量声明
全局常量声明
函数定义
函数参数
函数调用
局部变量声明
局部常量声明
赋值语句
return 语句
if / else
while
break
continue
表达式语句
空语句
算术表达式
关系表达式
逻辑表达式
一元表达式
括号表达式
```

---

## 11. 表达式优先级

Parser 当前按照以下优先级解析表达式：

```text
最低优先级：
||
&&
< > <= >= == !=
+ -
* / %
一元运算 + - !
数字 / 变量 / 函数调用 / 括号
最高优先级
```

例如：

```c
1 + 2 * 3 > 5 && 4 != 0
```

会解析为：

```text
&&
├── >
│   ├── +
│   │   ├── 1
│   │   └── *
│   │       ├── 2
│   │       └── 3
│   └── 5
└── !=
    ├── 4
    └── 0
```

---

## 12. 给 D 同学：Semantic 接口说明

D 同学负责语义分析。

D 同学可以从 Parser 得到 AST：

```cpp
toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

然后从 CompUnit 开始遍历：

```cpp
auto compUnit = std::dynamic_pointer_cast<toycc::CompUnit>(ast);
```

建议 Semantic 提供接口：

```cpp
#pragma once

#include "ast/ast.h"

namespace toycc {

class SemanticChecker {
public:
    void check(const ASTNodePtr& root);
};

}
```

建议文件位置：

```text
include/semantic/semantic_checker.h
src/semantic/semantic_checker.cpp
```

Semantic 需要重点检查：

```text
变量是否重复定义
变量是否未定义就使用
常量是否被赋值
函数是否重复定义
函数调用参数数量是否匹配
函数调用参数类型是否匹配
return 类型是否符合函数返回类型
break / continue 是否出现在 while 内部
main 函数是否存在
作用域是否正确
```

常见访问方式示例：

```cpp
if (node->type == toycc::ASTNodeType::Function) {
    auto func = std::dynamic_pointer_cast<toycc::FunctionDef>(node);

    auto returnType = func->returnType;
    auto name = func->name;
    auto params = func->params;
    auto body = func->body;
}
```

例如遇到变量声明：

```cpp
auto varDecl = std::dynamic_pointer_cast<toycc::VarDeclStmt>(stmt);

bool isConst = varDecl->isConst;
std::string name = varDecl->name;
toycc::ExprPtr init = varDecl->init;
```

---

## 13. 给 B 同学：IR / 后端接口说明

B 同学负责 IR 和 RISC-V 后端。

B 同学可以在 Semantic 通过后接收 AST：

```cpp
toycc::ASTNodePtr ast = parser.parse();

toycc::SemanticChecker checker;
checker.check(ast);

toycc::IRBuilder builder;
auto ir = builder.build(ast);
```

建议 IRBuilder 接口：

```cpp
#pragma once

#include "ast/ast.h"

namespace toycc {

class IRProgram;

class IRBuilder {
public:
    IRProgram build(const ASTNodePtr& root);
};

}
```

建议文件位置：

```text
include/ir/ir.h
include/ir/ir_builder.h
src/ir/ir.cpp
src/ir/ir_builder.cpp
```

B 同学生成 IR 时主要需要遍历：

```text
CompUnit
FunctionDef
Block
VarDeclStmt
AssignStmt
ReturnStmt
IfStmt
WhileStmt
BreakStmt
ContinueStmt
ExprStmt
NumberLiteral
Variable
UnaryExpr
BinaryExpr
FunctionCall
```

RISC-V 后端建议接口：

```cpp
#pragma once

#include <iosfwd>

#include "ir/ir.h"

namespace toycc {

class RiscvGenerator {
public:
    void generate(const IRProgram& program, std::ostream& os);
};

}
```

建议文件位置：

```text
include/codegen/riscv_generator.h
src/codegen/riscv_generator.cpp
```

最终后端输出应写到：

```cpp
std::cout
```

因为最终编译器要求从 stdin 读取 ToyC 程序，从 stdout 输出 RISC-V32 汇编。

---

## 14. AST 打印工具

AST 打印器定义在：

```text
include/ast/ast_printer.h
src/ast/ast_printer.cpp
```

调用方式：

```cpp
#include "ast/ast_printer.h"

toycc::ASTPrinter::print(ast, std::cerr);
```

示例输出：

```text
CompUnit
  VarDeclStmt const A
    NumberLiteral 1
  VarDeclStmt g
    NumberLiteral 2
  Function int main
    Block
      ReturnStmt
        BinaryExpr +
          Variable A
          Variable g
```

ASTPrinter 主要用于调试 Parser 和后续联调。

注意：调试信息建议输出到：

```cpp
std::cerr
```

最终 RISC-V 汇编必须输出到：

```cpp
std::cout
```

---

## 15. 当前 main.cpp 状态

当前 `src/main.cpp` 还是临时测试入口，里面手动构造 Token。

当前用途是：

```text
测试 Parser
测试 ASTPrinter
验证 AST 结构
```

等 C 同学的 Lexer 完成后，main.cpp 会改成真正编译器主流程：

```cpp
#include <iostream>
#include <sstream>

#include "lexer/lexer.h"
#include "parser/parser.h"
#include "semantic/semantic_checker.h"
#include "ir/ir_builder.h"
#include "codegen/riscv_generator.h"

int main() {
    std::ostringstream buffer;
    buffer << std::cin.rdbuf();

    std::string source = buffer.str();

    toycc::Lexer lexer(source);
    auto tokens = lexer.tokenize();

    toycc::Parser parser(tokens);
    auto ast = parser.parse();

    toycc::SemanticChecker checker;
    checker.check(ast);

    toycc::IRBuilder builder;
    auto ir = builder.build(ast);

    toycc::RiscvGenerator generator;
    generator.generate(ir, std::cout);

    return 0;
}
```

---

## 16. B / C / D 对接总结

### C 同学 Lexer 需要提供

```cpp
toycc::Lexer lexer(source);
std::vector<toycc::Token> tokens = lexer.tokenize();
```

并保证最后一个 Token 是：

```cpp
TokenType::EndOfFile
```

---

### D 同学 Semantic 需要接收

```cpp
toycc::ASTNodePtr ast
```

建议接口：

```cpp
toycc::SemanticChecker checker;
checker.check(ast);
```

---

### B 同学 IR / Backend 需要接收

```cpp
toycc::ASTNodePtr ast
```

建议接口：

```cpp
toycc::IRBuilder builder;
auto ir = builder.build(ast);

toycc::RiscvGenerator generator;
generator.generate(ir, std::cout);
```

---

