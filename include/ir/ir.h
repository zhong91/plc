#pragma once
#include <string>
#include <vector>
#include <utility>

namespace toycc {

enum class IRInstrType {
    LI, LOAD, STORE, LOAD_GLOBAL, STORE_GLOBAL,
    // LOAD_ARG: 读取调用者栈上传入的第 9+ 个参数；src1 为相对调用者 sp 的字节偏移
    // STORE_ARG: 调用前把第 9+ 个实参写到当前函数预留的 outgoing-arg 区；src2 为字节偏移
    LOAD_ARG, STORE_ARG,
    MV,
    ADD, SUB, MUL, DIV, REM,
    SLT, SEQZ, SNEZ,
    LABEL, JUMP, BRANCH_ZERO, BRANCH_NONZERO,
    CALL,
    RET,
};

struct IRInstr {
    IRInstrType type;
    std::string dest;
    std::string src1;
    std::string src2;
    std::string label;
    IRInstr(IRInstrType t, const std::string& d = "",
            const std::string& s1 = "", const std::string& s2 = "",
            const std::string& lbl = "")
        : type(t), dest(d), src1(s1), src2(s2), label(lbl) {}
};

struct IRFunction {
    std::string name;
    std::vector<IRInstr> instrs;
    int paramCount = 0;
    bool isVoid = false;

    // 逻辑局部区大小：局部变量 + 参数副本 + 临时 spill 槽。
    // 后端会把它们统一放在 outgoing 参数区之后。
    int localSize = 0;

    // 调用其它函数时，为第 9 个及之后的实参预留的最大栈空间。
    // 该区域位于当前 sp 的最低地址处，满足 RISC-V 调用约定。
    int outgoingArgSize = 0;
};

struct IRProgram {
    std::vector<IRFunction> functions;
    std::vector<std::pair<std::string, int>> globalVars;
};

} // namespace toycc
