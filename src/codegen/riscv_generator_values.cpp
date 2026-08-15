#include "codegen/riscv_generator.h"
#include <string>
#include <vector>

namespace toycc {

// Value-only binary expression whose two operands are local slots.  Keep this
// small and straightforward so MSVC does not have to optimize several large
// peephole matchers in one translation unit.
bool RiscvGenerator::tryEmitRegisterBinaryValue(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 2 >= v.size()) return false;

    const IRInstr& rhs = v[i];
    const IRInstr& lhs = v[i + 1];
    const IRInstr& op = v[i + 2];

    if (rhs.type != IRInstrType::LOAD || rhs.dest != "t0") return false;
    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t1") return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0") return false;

    const bool normalOrder = op.src1 == "t1" && op.src2 == "t0";
    const bool reverseOrder = op.src1 == "t0" && op.src2 == "t1";
    if (!normalOrder && !reverseOrder) return false;

    const std::string rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
    const std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    if (rhsReg.empty() || lhsReg.empty()) return false;

    const std::string first = normalOrder ? lhsReg : rhsReg;
    const std::string second = normalOrder ? rhsReg : lhsReg;

    const char* mnemonic = nullptr;
    switch (op.type) {
        case IRInstrType::ADD: mnemonic = "add"; break;
        case IRInstrType::SUB: mnemonic = "sub"; break;
        case IRInstrType::MUL: mnemonic = "mul"; break;
        case IRInstrType::DIV: mnemonic = "div"; break;
        case IRInstrType::REM: mnemonic = "rem"; break;
        case IRInstrType::SLT: mnemonic = "slt"; break;
        default: return false;
    }

    emitLine(std::string("    ") + mnemonic + " t0, " + first + ", " + second);
    i += 2;
    return true;
}

} // namespace toycc
