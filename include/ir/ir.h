#pragma once
#include <string>
#include <vector>
#include <utility>

namespace toycc {

enum class IRInstrType {
    LI, LOAD, STORE, LOAD_GLOBAL, STORE_GLOBAL,
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
};

struct IRProgram {
    std::vector<IRFunction> functions;
    std::vector<std::pair<std::string, int>> globalVars;
};

} // namespace toycc