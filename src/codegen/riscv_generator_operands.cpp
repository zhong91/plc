#include "codegen/riscv_generator.h"
#include <string>
#include <vector>

namespace toycc {

// Eliminate the ubiquitous
//     LOAD t1, hotSlot
//     OP   t0, t1, t0
// pair when hotSlot already lives in a promoted physical register.  t0 is the
// value produced by the immediately preceding expression.  This is especially
// important for left-associated reductions such as
//     sum + i*j + i + j
// where the old backend inserted an `mv t1, sX` before every ADD.
bool RiscvGenerator::tryEmitPromotedOperandOp(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 1 >= v.size()) return false;
    const auto& ld = v[i];
    const auto& op = v[i + 1];

    if (ld.type != IRInstrType::LOAD || ld.dest != "t1") return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0") return false;

    const bool left = op.src1 == "t1" && op.src2 == "t0";
    const bool right = op.src1 == "t0" && op.src2 == "t1";
    if (!left && !right) return false;

    const std::string reg = promotedRegForSlot(std::stoi(ld.src1));
    if (reg.empty()) return false;

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

    if (left)
        emitLine("    " + std::string(mnemonic) + " t0, " + reg + ", t0");
    else
        emitLine("    " + std::string(mnemonic) + " t0, t0, " + reg);

    i += 1;
    return true;
}

} // namespace toycc
