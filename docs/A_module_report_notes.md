# A 模块开发过程总结与报告材料

## 0. 模块概述

本项目需要实现一个完整的 ToyC 编译器，最终目标是：

```text
ToyC 源程序
    ↓
词法分析 Lexer
    ↓
Token 序列
    ↓
语法分析 Parser
    ↓
AST
    ↓
语义分析 Semantic
    ↓
IR / CFG
    ↓
RISC-V32 后端
    ↓
RISC-V32 汇编
```

本小组分工中，A 模块负责：

```text
项目总控 + AST + Parser
```

A 模块当前已经完成：

1. 项目基础结构搭建；
2. CMake 构建系统；
3. 公共 Token 定义；
4. AST 节点体系；
5. Parser 递归下降语法分析；
6. 表达式优先级解析；
7. 函数定义、函数参数、函数调用解析；
8. 全局 / 局部变量和常量声明解析；
9. return、赋值、表达式语句、空语句解析；
10. if / else、while、break、continue 控制流语句解析；
11. AST 打印调试工具；
12. 面向 B / C / D 三个模块的接口说明文档。

当前 A 模块已经作为前端基础交付给其他同学，后续会在 Lexer、Semantic、IR 完成后进行主流程联调。

---

# 一、开发过程总结

## 1. 项目初始化

项目一开始采用 C++20 和 CMake 作为基础技术栈。

主要目录结构如下：

```text
plc/
├── include/
│   ├── ast/
│   ├── common/
│   ├── parser/
│   ├── lexer/
│   ├── semantic/
│   ├── ir/
│   └── codegen/
├── src/
│   ├── ast/
│   ├── parser/
│   ├── lexer/
│   ├── semantic/
│   ├── ir/
│   └── codegen/
├── docs/
├── tests/
├── CMakeLists.txt
└── README.md
```

项目首先完成了最小可编译版本：

```text
CMakeLists.txt
src/main.cpp
```

之后通过 MSYS2 UCRT64 环境完成编译测试：

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/compiler.exe
```

这一步的目标是保证项目能稳定编译和运行，为后续模块开发建立基础。

---

## 2. Git 提交流程

开发过程中，每完成一个稳定小功能，就进行一次本地 Git 提交。

本地提交记录大致包括：

```text
Set up parser skeleton
Parse minimal main return program
Parse arithmetic expressions
Parse logical expressions
Parse function parameters and calls
Parse variable declarations and assignments
Parse if else statements
Parse while statements
Parse break and continue statements
Parse global declarations
Add AST printer skeleton
Print AST tree
Add A module interface documentation
```

由于本机网络连接 GitHub 不稳定，push 暂时没有成功，但本地 commit 历史完整保留，并通过补丁包 / 压缩包方式完成交付。

---

## 3. 开发路线

A 模块开发采用逐步扩展的方式。

最开始只支持：

```c
int main() {
    return 1;
}
```

然后逐步扩展到：

```c
int main() {
    return 1 + 2 * 3 > 5 && 4 != 0;
}
```

之后继续支持：

```c
int add(int a, int b) {
    return a + b;
}

int main() {
    return add(1, 2);
}
```

再扩展到变量声明、赋值、控制流：

```c
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

    return x;
}
```

最后支持全局变量和全局常量：

```c
const int A = 1;
int g = 2;

int main() {
    return A + g;
}
```

这种开发方式的好处是每一步都有可运行的测试，避免一次性写太多代码后难以定位错误。

---

# 二、报告材料：A 模块设计与实现

## 1. 编译器总体架构

本项目采用典型的分阶段编译器架构：

```text
源程序
  ↓
Lexer
  ↓
Parser
  ↓
AST
  ↓
Semantic
  ↓
IR
  ↓
Optimizer
  ↓
RISC-V Backend
  ↓
目标汇编
```

A 模块主要位于前端阶段，负责把 Token 序列转换成 AST。

其中：

- Lexer 负责从源代码生成 Token；
- Parser 负责从 Token 生成 AST；
- Semantic 负责检查 AST 的语义合法性；
- IRBuilder 负责根据 AST 生成中间表示；
- Backend 负责把 IR 转换成 RISC-V32 汇编。

A 模块的输出 AST 是后续 Semantic 和 IR 模块的共同输入，因此 AST 设计需要尽量稳定、清晰，并且方便遍历。

---

## 2. Token 设计

Token 是 Lexer 和 Parser 之间的公共接口，定义在：

```text
include/common/token.h
```

Token 结构主要包括：

```cpp
struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
};
```

其中：

- `type` 表示 Token 类型；
- `lexeme` 保存源代码中的原始字符串；
- `line` 和 `column` 用于语法错误定位。

TokenType 覆盖了 ToyC 当前需要的基本类型，包括：

```text
EndOfFile
Identifier
Number

KwConst
KwInt
KwVoid
KwIf
KwElse
KwWhile
KwBreak
KwContinue
KwReturn

Plus
Minus
Star
Slash
Percent

Assign
Equal
NotEqual
Less
LessEqual
Greater
GreaterEqual

LogicalAnd
LogicalOr
LogicalNot

LParen
RParen
LBrace
RBrace
Comma
Semicolon
Unknown
```

Parser 只依赖 Token 序列，不依赖 Lexer 的内部实现。这样 Lexer 同学只要保证输出正确的 `std::vector<Token>`，Parser 就可以直接接入。

Token 序列最后必须带有：

```cpp
TokenType::EndOfFile
```

这是 Parser 判断输入结束的重要依据。

---

## 3. AST 节点设计

AST 定义在：

```text
include/ast/ast.h
```

AST 采用面向对象方式组织，所有节点继承自基础类：

```cpp
class ASTNode {
public:
    ASTNodeType type;
    virtual ~ASTNode() = default;
};
```

为了区分表达式和语句，又设计了两个中间基类：

```cpp
class Expr : public ASTNode {};
class Stmt : public ASTNode {};
```

常用指针类型为：

```cpp
using ASTNodePtr = std::shared_ptr<ASTNode>;
using ExprPtr = std::shared_ptr<Expr>;
using StmtPtr = std::shared_ptr<Stmt>;
```

当前 AST 节点大致分为三类。

### 3.1 顶层结构节点

```text
CompUnit
FunctionDef
Block
```

`CompUnit` 表示整个程序，里面保存所有顶层声明和函数：

```cpp
class CompUnit : public ASTNode {
public:
    std::vector<ASTNodePtr> units;
};
```

`FunctionDef` 表示函数定义：

```cpp
class FunctionDef : public ASTNode {
public:
    ValueType returnType;
    std::string name;
    std::vector<Param> params;
    std::shared_ptr<Block> body;
};
```

`Block` 表示代码块：

```cpp
class Block : public Stmt {
public:
    std::vector<StmtPtr> statements;
};
```

### 3.2 语句节点

当前支持的语句节点包括：

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

例如变量声明：

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

赋值语句：

```cpp
class AssignStmt : public Stmt {
public:
    std::string name;
    ExprPtr value;
};
```

对应：

```c
x = x + 1;
```

条件语句：

```cpp
class IfStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};
```

循环语句：

```cpp
class WhileStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr body;
};
```

### 3.3 表达式节点

当前支持的表达式节点包括：

```text
NumberLiteral
Variable
UnaryExpr
BinaryExpr
FunctionCall
```

数字字面量：

```cpp
class NumberLiteral : public Expr {
public:
    int value;
};
```

变量引用：

```cpp
class Variable : public Expr {
public:
    std::string name;
};
```

一元表达式：

```cpp
class UnaryExpr : public Expr {
public:
    std::string op;
    ExprPtr operand;
};
```

二元表达式：

```cpp
class BinaryExpr : public Expr {
public:
    std::string op;
    ExprPtr left;
    ExprPtr right;
};
```

函数调用：

```cpp
class FunctionCall : public Expr {
public:
    std::string name;
    std::vector<ExprPtr> args;
};
```

这种 AST 设计使得语义分析和 IR 生成可以通过递归遍历节点完成。

---

## 4. Parser 递归下降设计

Parser 定义在：

```text
include/parser/parser.h
src/parser/parser.cpp
```

Parser 的输入是：

```cpp
std::vector<Token>
```

输出是：

```cpp
ASTNodePtr
```

基本使用方式：

```cpp
toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

Parser 采用手写递归下降方式实现。  
递归下降的特点是：每一个语法规则基本对应一个解析函数。

当前主要解析函数包括：

```cpp
parseCompUnit()
parseFunctionDef()
parseBlock()
parseStmt()
parseExpr()
parseLOrExpr()
parseLAndExpr()
parseRelExpr()
parseAddExpr()
parseMulExpr()
parseUnaryExpr()
parsePrimaryExpr()
```

辅助函数包括：

```cpp
peek()
previous()
isAtEnd()
check()
checkNext()
checkAhead()
match()
advance()
consume()
```

其中：

- `peek()` 查看当前 Token；
- `advance()` 前进到下一个 Token；
- `match()` 如果当前 Token 类型匹配则前进；
- `consume()` 要求当前 Token 必须是指定类型，否则抛出语法错误；
- `checkAhead()` 用于向前查看若干个 Token，主要用于区分全局变量声明和函数定义。

例如在顶层遇到：

```c
int g = 2;
```

和：

```c
int main() { ... }
```

都以 `int Identifier` 开头。  
Parser 通过查看后面是否为左括号来区分：

```text
int Identifier (   => 函数定义
int Identifier =   => 变量声明
```

---

## 5. 表达式优先级处理

表达式解析是 Parser 中最重要的一部分。

本项目没有使用自动生成 Parser，而是手写多层递归下降函数来表达优先级。

当前优先级从低到高为：

```text
||
&&
< > <= >= == !=
+ -
* / %
一元运算 + - !
数字 / 变量 / 函数调用 / 括号
```

对应解析函数为：

```cpp
parseExpr()
  ↓
parseLOrExpr()
  ↓
parseLAndExpr()
  ↓
parseRelExpr()
  ↓
parseAddExpr()
  ↓
parseMulExpr()
  ↓
parseUnaryExpr()
  ↓
parsePrimaryExpr()
```

例如表达式：

```c
1 + 2 * 3 > 5 && 4 != 0
```

会先解析 `2 * 3`，再解析 `1 + ...`，再解析关系表达式，最后解析逻辑与表达式。

这样可以保证表达式优先级正确。

二元表达式会生成 `BinaryExpr` 节点：

```cpp
expr = std::make_shared<BinaryExpr>("+", left, right);
```

一元表达式会生成 `UnaryExpr` 节点：

```cpp
return std::make_shared<UnaryExpr>("-", operand);
```

括号表达式会在 `parsePrimaryExpr()` 中递归调用 `parseExpr()`，从而支持任意嵌套表达式。

---

## 6. 控制流语句解析

当前 Parser 支持以下控制流语句：

```text
if / else
while
break
continue
return
```

### 6.1 if / else

ToyC 代码：

```c
if (x > 0) {
    return x;
} else {
    return 0;
}
```

对应 AST：

```cpp
class IfStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr thenBranch;
    StmtPtr elseBranch;
};
```

解析流程为：

```text
读取 if
读取 (
解析条件表达式
读取 )
解析 then 分支语句
如果存在 else，则解析 else 分支语句
生成 IfStmt
```

如果没有 else，则：

```cpp
elseBranch == nullptr
```

### 6.2 while

ToyC 代码：

```c
while (x < 10) {
    x = x + 1;
}
```

对应 AST：

```cpp
class WhileStmt : public Stmt {
public:
    ExprPtr condition;
    StmtPtr body;
};
```

解析流程为：

```text
读取 while
读取 (
解析循环条件
读取 )
解析循环体语句
生成 WhileStmt
```

### 6.3 break / continue

ToyC 代码：

```c
break;
continue;
```

对应 AST：

```cpp
class BreakStmt : public Stmt {};
class ContinueStmt : public Stmt {};
```

当前 Parser 只负责识别语法。  
至于 break / continue 是否出现在 while 内部，需要由 Semantic 模块检查。

### 6.4 return

ToyC 代码：

```c
return x;
return 0;
return;
```

对应 AST：

```cpp
class ReturnStmt : public Stmt {
public:
    ExprPtr value;
};
```

如果是空 return：

```cpp
value == nullptr
```

return 类型是否符合函数返回类型，也由 Semantic 模块检查。

---

## 7. 函数定义和调用解析

### 7.1 函数定义

Parser 支持如下函数定义：

```c
int add(int a, int b) {
    return a + b;
}
```

对应 AST：

```cpp
class FunctionDef : public ASTNode {
public:
    ValueType returnType;
    std::string name;
    std::vector<Param> params;
    std::shared_ptr<Block> body;
};
```

函数参数使用：

```cpp
struct Param {
    ValueType type;
    std::string name;
};
```

解析流程为：

```text
解析返回类型
解析函数名
解析 (
解析参数列表
解析 )
解析函数体 Block
生成 FunctionDef
```

当前支持 `int` 和 `void` 两种函数返回类型。

### 7.2 函数调用

Parser 支持如下函数调用：

```c
add(1, 2)
foo()
```

对应 AST：

```cpp
class FunctionCall : public Expr {
public:
    std::string name;
    std::vector<ExprPtr> args;
};
```

在解析表达式时，如果遇到 Identifier，Parser 会继续查看后面是否跟着 `(`。

如果是：

```text
Identifier (
```

就解析为函数调用。

如果不是，则解析为普通变量引用。

例如：

```c
add(1, 2)
```

会生成：

```text
FunctionCall add
  NumberLiteral 1
  NumberLiteral 2
```

函数是否存在、参数数量是否匹配、参数类型是否匹配，由 Semantic 模块检查。

---

## 8. AST 打印器设计

为了方便调试 Parser，A 模块实现了 AST 打印工具。

文件位置：

```text
include/ast/ast_printer.h
src/ast/ast_printer.cpp
```

调用方式：

```cpp
toycc::ASTPrinter::print(ast, std::cerr);
```

ASTPrinter 会递归遍历 AST，并以缩进形式打印树状结构。

例如 ToyC 代码：

```c
const int A = 1;
int g = 2;

int main() {
    return A + g;
}
```

打印结果为：

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

ASTPrinter 主要用于：

```text
检查 Parser 是否正确生成 AST
辅助 Lexer 和 Parser 联调
辅助 Semantic 同学理解 AST 结构
辅助 IR 同学编写 AST 遍历逻辑
```

注意：ASTPrinter 的调试输出应写到：

```cpp
std::cerr
```

最终 RISC-V 汇编必须写到：

```cpp
std::cout
```

这样可以避免调试信息污染最终输出。

---

## 9. 和 Lexer / Semantic / IR 的接口

### 9.1 和 Lexer 的接口

Lexer 同学需要提供：

```cpp
toycc::Lexer lexer(source);
std::vector<toycc::Token> tokens = lexer.tokenize();
```

Parser 接收 Token：

```cpp
toycc::Parser parser(tokens);
toycc::ASTNodePtr ast = parser.parse();
```

Parser 不依赖 Lexer 内部实现，只依赖 TokenType 和 Token 序列。

Lexer 必须保证：

```text
Token 序列最后有 EndOfFile
Token 的 type 正确
Token 的 lexeme 正确
Token 的 line / column 尽量准确
```

### 9.2 和 Semantic 的接口

Semantic 同学接收 Parser 输出的 AST：

```cpp
toycc::ASTNodePtr ast = parser.parse();
```

建议 SemanticChecker 接口：

```cpp
class SemanticChecker {
public:
    void check(const ASTNodePtr& root);
};
```

Semantic 可以从根节点开始遍历：

```cpp
auto compUnit = std::dynamic_pointer_cast<toycc::CompUnit>(ast);
```

Semantic 主要检查：

```text
变量是否重复定义
变量是否未定义就使用
常量是否被赋值
函数是否重复定义
函数调用参数数量是否匹配
函数调用参数类型是否匹配
return 类型是否符合函数返回类型
break / continue 是否在 while 内部
main 函数是否存在
作用域是否正确
```

### 9.3 和 IR 的接口

IR 同学在语义分析通过后接收 AST：

```cpp
toycc::ASTNodePtr ast = parser.parse();

toycc::SemanticChecker checker;
checker.check(ast);

toycc::IRBuilder builder;
auto ir = builder.build(ast);
```

建议 IRBuilder 接口：

```cpp
class IRBuilder {
public:
    IRProgram build(const ASTNodePtr& root);
};
```

IRBuilder 需要重点遍历：

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

后端最终接口建议为：

```cpp
class RiscvGenerator {
public:
    void generate(const IRProgram& program, std::ostream& os);
};
```

最终输出：

```cpp
generator.generate(ir, std::cout);
```

---

# 三、当前状态与后续计划

## 1. 当前状态

A 模块已经完成第一版交付。

当前状态：

```text
Token 定义完成
AST 节点完成
Parser 主体完成
ASTPrinter 完成
接口文档完成
```

当前 main.cpp 仍然是测试入口，里面手动构造 Token。  
这是为了在 Lexer 尚未完成时，提前测试 Parser 和 AST。

---

## 2. 后续计划

等 C 同学完成 Lexer 后，A 模块需要把 `main.cpp` 改成真正主流程：

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

在后续联调阶段，A 模块主要负责：

```text
维护 AST / Parser 接口稳定
配合 C 同学接入 Lexer
配合 D 同学遍历 AST 做语义分析
配合 B 同学根据 AST 生成 IR
整理测试样例
协助总控 main.cpp 拼接完整编译流程
```

---

# 四、阶段性总结

A 模块的核心工作是把 ToyC 程序从 Token 序列解析成结构化 AST。

当前实现采用手写递归下降 Parser，整体结构清晰，便于调试和扩展。  
表达式部分通过分层解析函数解决优先级问题；语句部分通过不同 AST 节点表示 return、变量声明、赋值、if、while、break、continue 等结构；顶层解析支持全局变量、全局常量和函数定义。

ASTPrinter 的实现使得 Parser 的结果可以直观展示出来，方便小组后续联调。  
同时，A 模块已经提供了面向 Lexer、Semantic 和 IR 的接口说明，为后续模块拼接打下基础。

目前 A 模块不宜继续大改 AST / Parser 接口，后续应以接口维护、联调和测试为主。