# Parser 测试样例

本文档用于记录 Parser 阶段的 ToyC 测试样例。  
这些样例主要用于后续 Lexer + Parser 联调，也可以作为报告中“测试与结果”部分的参考材料。

当前 Parser 的目标是：  
把 Lexer 输出的 Token 序列解析成 AST。

---

## 1. 最小 main 函数

### ToyC 源码

```c
int main() {
    return 1;
}
```

### 测试目标

检查 Parser 是否能解析：

- 函数定义
- 空参数列表
- Block
- return 语句
- 数字字面量

### 期望 AST 结构

```text
CompUnit
  Function int main
    Block
      ReturnStmt
        NumberLiteral 1
```

---

## 2. 算术表达式优先级

### ToyC 源码

```c
int main() {
    return 1 + 2 * 3;
}
```

### 测试目标

检查 Parser 是否能正确处理：

- 加法
- 乘法
- 表达式优先级

### 期望 AST 结构

```text
CompUnit
  Function int main
    Block
      ReturnStmt
        BinaryExpr +
          NumberLiteral 1
          BinaryExpr *
            NumberLiteral 2
            NumberLiteral 3
```

---

## 3. 逻辑表达式和关系表达式

### ToyC 源码

```c
int main() {
    return 1 + 2 * 3 > 5 && 4 != 0;
}
```

### 测试目标

检查 Parser 是否能解析：

- 算术表达式
- 关系表达式
- 相等 / 不等表达式
- 逻辑与表达式

### 期望重点

表达式应按照以下优先级解析：

```text
*
+
>
!=
&&
```

---

## 4. 函数定义、参数和函数调用

### ToyC 源码

```c
int add(int a, int b) {
    return a + b;
}

int main() {
    return add(1, 2);
}
```

### 测试目标

检查 Parser 是否能解析：

- 多函数定义
- 函数参数
- 函数调用
- 实参列表
- 参数变量引用

### 期望 AST 结构重点

```text
CompUnit
  Function int add
    Params
      int a
      int b
    Block
      ReturnStmt
        BinaryExpr +
          Variable a
          Variable b
  Function int main
    Block
      ReturnStmt
        FunctionCall add
          NumberLiteral 1
          NumberLiteral 2
```

---

## 5. 全局变量和全局常量

### ToyC 源码

```c
const int A = 1;
int g = 2;

int main() {
    return A + g;
}
```

### 测试目标

检查 Parser 是否能解析：

- 全局常量声明
- 全局变量声明
- 顶层函数定义
- 全局变量引用

### 期望 AST 结构

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

---

## 6. 局部变量声明和赋值语句

### ToyC 源码

```c
int main() {
    int x = 1;
    x = x + 2;
    return x;
}
```

### 测试目标

检查 Parser 是否能解析：

- 局部变量声明
- 赋值语句
- 变量引用
- return 变量

### 期望 AST 结构重点

```text
Function int main
  Block
    VarDeclStmt x
      NumberLiteral 1
    AssignStmt x
      BinaryExpr +
        Variable x
        NumberLiteral 2
    ReturnStmt
      Variable x
```

---

## 7. if / else 语句

### ToyC 源码

```c
int main() {
    int x = 1;

    if (x > 0) {
        return x;
    } else {
        return 0;
    }
}
```

### 测试目标

检查 Parser 是否能解析：

- if 条件
- then 分支
- else 分支
- 嵌套 Block

### 期望 AST 结构重点

```text
IfStmt
  Condition
    BinaryExpr >
      Variable x
      NumberLiteral 0
  Then
    Block
      ReturnStmt
        Variable x
  Else
    Block
      ReturnStmt
        NumberLiteral 0
```

---

## 8. while 循环

### ToyC 源码

```c
int main() {
    int x = 0;

    while (x < 3) {
        x = x + 1;
    }

    return x;
}
```

### 测试目标

检查 Parser 是否能解析：

- while 条件
- while 循环体
- 循环内赋值
- 循环后 return

### 期望 AST 结构重点

```text
WhileStmt
  Condition
    BinaryExpr <
      Variable x
      NumberLiteral 3
  Body
    Block
      AssignStmt x
        BinaryExpr +
          Variable x
          NumberLiteral 1
```

---

## 9. break 和 continue

### ToyC 源码

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

### 测试目标

检查 Parser 是否能解析：

- while
- if
- break
- continue
- 多层嵌套语句

### 说明

Parser 只负责识别 `break;` 和 `continue;` 的语法。  
至于它们是否真的出现在循环内部，需要由 Semantic 阶段检查。

---

## 10. 空语句和表达式语句

### ToyC 源码

```c
int foo() {
    return 1;
}

int main() {
    ;
    foo();
    return 0;
}
```

### 测试目标

检查 Parser 是否能解析：

- 空语句 `;`
- 函数调用表达式语句
- 多函数程序

### 期望 AST 结构重点

```text
Function int main
  Block
    EmptyStmt
    ExprStmt
      FunctionCall foo
    ReturnStmt
      NumberLiteral 0
```

---

## 11. 一元表达式

### ToyC 源码

```c
int main() {
    int x = 1;
    return -x + !0;
}
```

### 测试目标

检查 Parser 是否能解析：

- 一元负号
- 逻辑非
- 一元表达式和二元表达式混合

### 期望 AST 结构重点

```text
ReturnStmt
  BinaryExpr +
    UnaryExpr -
      Variable x
    UnaryExpr !
      NumberLiteral 0
```

---

## 12. 括号表达式

### ToyC 源码

```c
int main() {
    return (1 + 2) * 3;
}
```

### 测试目标

检查 Parser 是否能解析括号表达式，并正确改变优先级。

### 期望 AST 结构重点

```text
ReturnStmt
  BinaryExpr *
    BinaryExpr +
      NumberLiteral 1
      NumberLiteral 2
    NumberLiteral 3
```

---

## 13. 综合样例

### ToyC 源码

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

### 测试目标

这是当前 Parser 支持能力的综合测试样例，覆盖：

- 全局常量
- 全局变量
- 函数定义
- 函数参数
- 函数调用
- 局部变量声明
- while
- if
- break
- continue
- return
- 复杂表达式

---

## 14. 当前 Parser 暂未重点支持或后续需确认的内容

以下内容后续需要根据 ToyC 实验文档进一步确认：

```text
变量声明是否允许不初始化，例如 int x;
函数参数是否允许 void
是否支持多个变量连续声明，例如 int a = 1, b = 2;
是否支持数组
是否支持 for
是否支持 do while
是否支持输入输出函数
是否支持注释
是否支持十六进制数字
```

当前 A 模块 Parser 第一版主要覆盖 ToyC 核心语法，为后续 Semantic、IR 和 Backend 提供 AST 输入。