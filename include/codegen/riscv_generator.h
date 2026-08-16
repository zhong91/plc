#pragma once
#include <iostream>
#include <cstdint>
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
    std::unordered_map<int, int> currentPhysicalSlots;
    std::unordered_map<int, std::string> currentConstantRegs;
    bool optimize = false;

    void emitLine(const std::string& s) { out << s << "\n"; }
    void emitInstr(const IRInstr& instr);
    bool fitsImm12(int value) const;
    void emitAdjustSp(int delta);
    void emitLoadFromSp(const std::string& dest, int offset);
    void emitStoreToSp(const std::string& src, int offset);
    bool emitSignedDivConst(const std::string& dest, const std::string& src, std::int32_t divisor);
    bool emitSignedRemConst(const std::string& dest, const std::string& src, std::int32_t divisor);
    std::vector<std::pair<int, std::string>> choosePromotedSlots(const IRFunction& func) const;
    bool tryEmitSpillPeephole(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitSimplePair(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitDirectBinaryUpdate(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitOptimizedBinaryUpdate(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitCompareBranch(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitOptimizedCompareBranch(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitDirectValueBranch(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitImmediateBinaryValue(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitRegisterBinaryValue(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitPromotedOperandOp(const std::vector<IRInstr>& instrs, size_t& index);
    bool tryEmitMulAccumulateUpdate(const std::vector<IRInstr>& instrs, size_t& index);
    bool emitSimpleValueToReg(const IRInstr& instr, const std::string& dest);
    std::string promotedRegForSlot(int logicalOffset) const;
    int physicalSlotOffset(int logicalOffset) const;
    static bool isCoreBinaryOp(IRInstrType type);

public:
    explicit RiscvGenerator(std::ostream& os, bool enableOpt = false)
        : out(os), optimize(enableOpt) {}
    void generate(const IRProgram& program);
};

} // namespace toycc
