#pragma once
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include "ir/ir.h"

namespace toycc {

class RiscvGenerator {
private:
    std::ostream& out;
    int currentFrameSize = 0;
    int currentLocalBase = 0;
    std::unordered_map<int, std::string> currentPromotedSlots;
    bool optimize = false;

    void emitLine(const std::string& s) { out << s << "\n"; }
    void emitInstr(const IRInstr& instr);
    bool fitsImm12(int value) const;
    void emitAdjustSp(int delta);
    void emitLoadFromSp(const std::string& dest, int offset);
    void emitStoreToSp(const std::string& src, int offset);
    std::vector<std::pair<int, std::string>> choosePromotedSlots(const IRFunction& func) const;
    bool tryEmitSpillPeephole(const std::vector<IRInstr>& instrs, size_t& index);
    static bool isCoreBinaryOp(IRInstrType type);

public:
    explicit RiscvGenerator(std::ostream& os, bool enableOpt = false)
        : out(os), optimize(enableOpt) {}
    void generate(const IRProgram& program);
};

} // namespace toycc
