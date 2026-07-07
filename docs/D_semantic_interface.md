# D 模块接口说明：Semantic

## 1. 文件位置

```text
include/semantic/semantic_checker.h
src/semantic/semantic_checker.cpp
tests/semantic_checker_tests.cpp
```

Semantic 接收 Parser 生成的 AST，不修改 AST。公共类型均位于 `toycc` 命名空间。

## 2. 调用接口

```cpp
#include "semantic/semantic_checker.h"

toycc::SemanticChecker checker;
checker.check(ast);
```

检查成功时 `check` 正常返回；遇到第一个语义错误时抛出：

```cpp
toycc::SemanticError
```

错误信息统一以 `Semantic error:` 开头。当前 AST 节点没有保存源代码行列，因此错误信息以标识符、函数名和语义上下文定位，暂时不能精确报告行号。

## 3. 已实现的检查

- AST 根节点必须为 `CompUnit`；
- 程序必须存在 `int main()`，且参数列表为空；
- 全局作用域只能包含变量/常量声明与函数定义；
- 变量、常量、参数和函数在同一普通标识符命名空间中；
- 同一作用域禁止重复声明，内层 Block 允许屏蔽外层同名对象；
- 变量/常量必须在声明之后使用；
- 声明必须包含 `int` 类型初始化表达式；
- 常量初始化式只能包含数字、此前声明的常量和一元/二元运算；
- 常量不能作为赋值左值；
- 函数只能调用此前已定义的函数，但允许函数直接递归调用自身；
- 函数调用检查实参数量以及每个实参的 `int` 类型；
- `void` 函数调用可作为表达式语句，但不能作为条件、初始化值、赋值右值、实参或 `int` 返回值；
- `int`/`void` 函数的 return 形式必须与返回类型一致；
- `int` 函数不能沿普通控制流落到函数末尾；
- `break`、`continue` 只能出现在 `while` 内；
- `if`、`while` 条件必须为 `int`；
- 一元和二元运算的操作数必须为 `int`；
- 检测可在编译期确定的除零、取模零和 32 位有符号常量运算溢出；
- 常量求值遵守 `&&`、`||` 的短路规则。

## 4. 作用域规则

Semantic 使用作用域栈：

```text
全局作用域
  └── 函数参数/函数最外层语句块作用域
        └── 嵌套 Block 作用域
              └── 更深层 Block ...
```

函数参数与函数体最外层声明处于同一作用域，因此下面代码会报重复声明：

```c
int f(int x) {
    int x = 1;
    return x;
}
```

嵌套 Block 可以屏蔽：

```c
int main() {
    int x = 1;
    {
        int x = 2;
    }
    return x;
}
```

## 5. 函数声明顺序

ToyC 要求函数调用写在被调函数定义之后，并允许递归。因此分析函数时先把当前函数签名加入全局符号表，再遍历函数体：

```c
int self(int x) {       // 合法：递归
    if (x == 0) return 0;
    return self(x - 1);
}

int first() {           // 非法：later 尚未定义
    return later();
}

int later() {
    return 1;
}
```

## 6. 给 IR/后端的对接方式

```cpp
auto ast = parser.parse();

toycc::SemanticChecker checker;
checker.check(ast);

// checker 正常返回后，AST 已通过语义检查。
toycc::IRBuilder builder;
auto ir = builder.build(ast);
```

当前 Semantic 不给 AST 增加注解。IR 阶段仍从 AST 节点读取名字、返回类型、参数和表达式结构。

## 7. 构建与测试

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

也可直接运行：

```bash
./build/semantic_checker_tests
```

Windows 多配置生成器通常为：

```cmd
build\Debug\semantic_checker_tests.exe
```
