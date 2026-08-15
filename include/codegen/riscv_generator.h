#pragma once
#include <iostream>
#include <string>
#include "ir/ir.h"

namespace toycc {

class RiscvGenerator {
private:
    std::ostream& out;

    // 当前函数的信息。普通 LOAD/STORE 的逻辑局部偏移需要加上
    // outgoing-arg 区大小；LOAD_ARG 则要越过整个当前栈帧去读调用者栈。
    int currentFrameSize = 0;
    int currentLocalBase = 0;

    void emitLine(const std::string& s) { out << s << "\n"; }
    void emitInstr(const IRInstr& instr);

    bool fitsImm12(int value) const;
    void emitAdjustSp(int delta);
    void emitLoadFromSp(const std::string& dest, int offset);
    void emitStoreToSp(const std::string& src, int offset);

public:
    explicit RiscvGenerator(std::ostream& os) : out(os) {}
    void generate(const IRProgram& program);
};

} // namespace toycc
