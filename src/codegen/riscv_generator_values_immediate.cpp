#include "codegen/riscv_generator.h"
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace toycc {

// Fold a value-only binary expression of the form
//   LI t0, imm; LOAD t1, slot; OP t0, t1, t0
// directly from the promoted hot register.  This implementation deliberately
// avoids local lambdas/optional/local classes because MSVC 14.43 can ICE on
// that combination in this peephole-heavy backend.
bool RiscvGenerator::tryEmitImmediateBinaryValue(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 2 >= v.size()) return false;

    const IRInstr& rhs = v[i];
    const IRInstr& lhs = v[i + 1];
    const IRInstr& op = v[i + 2];

    if (rhs.type != IRInstrType::LI || rhs.dest != "t0") return false;
    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t1") return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0") return false;

    const bool standard = op.src1 == "t1" && op.src2 == "t0"; // lhs OP imm
    const bool reversed = op.src1 == "t0" && op.src2 == "t1"; // imm OP lhs
    if (!standard && !reversed) return false;

    const std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    if (lhsReg.empty()) return false;

    const long long imm = std::stoll(rhs.src1);
    std::string cachedReg;
    if (imm >= std::numeric_limits<int>::min() && imm <= std::numeric_limits<int>::max()) {
        auto it = currentConstantRegs.find(static_cast<int>(imm));
        if (it != currentConstantRegs.end()) cachedReg = it->second;
    }

    switch (op.type) {
        case IRInstrType::ADD:
            if (imm == 0) {
                if (lhsReg != "t0") emitLine("    mv t0, " + lhsReg);
            } else if (imm >= -2048 && imm <= 2047) {
                emitLine("    addi t0, " + lhsReg + ", " + std::to_string(imm));
            } else if (!cachedReg.empty()) {
                emitLine("    add t0, " + lhsReg + ", " + cachedReg);
            } else {
                emitLine("    li t6, " + std::to_string(imm));
                emitLine("    add t0, " + lhsReg + ", t6");
            }
            break;

        case IRInstrType::SUB:
            if (standard) {
                if (imm == 0) {
                    if (lhsReg != "t0") emitLine("    mv t0, " + lhsReg);
                } else {
                    const long long negImm = -imm;
                    if (negImm >= -2048 && negImm <= 2047) {
                        emitLine("    addi t0, " + lhsReg + ", " + std::to_string(negImm));
                    } else if (!cachedReg.empty()) {
                        emitLine("    sub t0, " + lhsReg + ", " + cachedReg);
                    } else {
                        emitLine("    li t6, " + std::to_string(imm));
                        emitLine("    sub t0, " + lhsReg + ", t6");
                    }
                }
            } else {
                if (!cachedReg.empty()) {
                    emitLine("    sub t0, " + cachedReg + ", " + lhsReg);
                } else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    sub t0, t6, " + lhsReg);
                }
            }
            break;

        case IRInstrType::MUL: {
            int shift = -1;
            if (imm > 0 && (imm & (imm - 1)) == 0) {
                shift = 0;
                long long value = imm;
                while (value > 1) {
                    value >>= 1;
                    ++shift;
                }
            }

            if (imm == 0) {
                emitLine("    li t0, 0");
            } else if (imm == 1) {
                if (lhsReg != "t0") emitLine("    mv t0, " + lhsReg);
            } else if (imm == -1) {
                emitLine("    neg t0, " + lhsReg);
            } else if (shift >= 0) {
                emitLine("    slli t0, " + lhsReg + ", " + std::to_string(shift));
            } else if (!cachedReg.empty()) {
                emitLine("    mul t0, " + lhsReg + ", " + cachedReg);
            } else {
                emitLine("    li t6, " + std::to_string(imm));
                emitLine("    mul t0, " + lhsReg + ", t6");
            }
            break;
        }

        case IRInstrType::DIV:
            if (standard) {
                emitSignedDivConst("t0", lhsReg, static_cast<std::int32_t>(imm));
            } else {
                if (!cachedReg.empty()) emitLine("    div t0, " + cachedReg + ", " + lhsReg);
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    div t0, t6, " + lhsReg);
                }
            }
            break;

        case IRInstrType::REM:
            if (standard) {
                emitSignedRemConst("t0", lhsReg, static_cast<std::int32_t>(imm));
            } else {
                if (!cachedReg.empty()) emitLine("    rem t0, " + cachedReg + ", " + lhsReg);
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    rem t0, t6, " + lhsReg);
                }
            }
            break;

        case IRInstrType::SLT:
            if (standard && imm >= -2048 && imm <= 2047) {
                emitLine("    slti t0, " + lhsReg + ", " + std::to_string(imm));
            } else if (!cachedReg.empty()) {
                if (standard) emitLine("    slt t0, " + lhsReg + ", " + cachedReg);
                else emitLine("    slt t0, " + cachedReg + ", " + lhsReg);
            } else {
                emitLine("    li t6, " + std::to_string(imm));
                if (standard) {
                    emitLine("    slt t0, " + lhsReg + ", t6");
                } else {
                    emitLine("    slt t0, t6, " + lhsReg);
                }
            }
            break;

        default:
            return false;
    }

    i += 2;
    return true;
}

} // namespace toycc
