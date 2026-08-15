#include "codegen/riscv_generator.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace toycc {

bool RiscvGenerator::tryEmitMulAccumulateUpdate(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 5 >= v.size()) return false;

    const IRInstr& a = v[i];
    const IRInstr& b = v[i + 1];
    const IRInstr& mul = v[i + 2];
    const IRInstr& accLd = v[i + 3];
    const IRInstr& sum = v[i + 4];
    const IRInstr& accSt = v[i + 5];

    if (a.dest != "t0" || b.dest != "t1") return false;
    if (!((a.type == IRInstrType::LOAD || a.type == IRInstrType::LI) &&
          (b.type == IRInstrType::LOAD || b.type == IRInstrType::LI))) return false;
    if (mul.type != IRInstrType::MUL || mul.dest != "t0") return false;

    const bool mulNormal = mul.src1 == "t1" && mul.src2 == "t0";
    const bool mulReverse = mul.src1 == "t0" && mul.src2 == "t1";
    if (!mulNormal && !mulReverse) return false;

    if (accLd.type != IRInstrType::LOAD || accLd.dest != "t1") return false;
    if ((sum.type != IRInstrType::ADD && sum.type != IRInstrType::SUB) ||
        sum.dest != "t0" || sum.src1 != "t1" || sum.src2 != "t0") return false;
    if (accSt.type != IRInstrType::STORE || accSt.src1 != "t0" || accSt.src2 != accLd.src1) return false;

    const std::string accReg = promotedRegForSlot(std::stoi(accLd.src1));
    if (accReg.empty()) return false;

    const bool aImm = a.type == IRInstrType::LI;
    const bool bImm = b.type == IRInstrType::LI;
    if (aImm && bImm) return false; // IR constant folding should already handle this.

    long long aValue = 0;
    long long bValue = 0;
    std::string aReg;
    std::string bReg;

    if (aImm) {
        aValue = std::stoll(a.src1);
    } else {
        aReg = promotedRegForSlot(std::stoi(a.src1));
        if (aReg.empty()) return false;
    }

    if (bImm) {
        bValue = std::stoll(b.src1);
    } else {
        bReg = promotedRegForSlot(std::stoi(b.src1));
        if (bReg.empty()) return false;
    }

    std::string productReg = "t6";

    if (aImm || bImm) {
        const long long constant = aImm ? aValue : bValue;
        const std::string& variableReg = aImm ? bReg : aReg;

        if (constant == 0) {
            i += 5;
            return true; // acc +/- 0
        }

        if (constant == 1) {
            productReg = variableReg;
        } else if (constant == -1) {
            emitLine("    neg t6, " + variableReg);
        } else {
            bool powerOfTwo = constant > 0 && (constant & (constant - 1)) == 0;
            if (powerOfTwo) {
                int shift = 0;
                long long value = constant;
                while (value > 1) {
                    value >>= 1;
                    ++shift;
                }
                emitLine("    slli t6, " + variableReg + ", " + std::to_string(shift));
            } else {
                const auto it = currentConstantRegs.find(static_cast<int>(constant));
                if (it != currentConstantRegs.end()) {
                    emitLine("    mul t6, " + variableReg + ", " + it->second);
                } else {
                    emitLine("    li t1, " + std::to_string(constant));
                    emitLine("    mul t6, " + variableReg + ", t1");
                }
            }
        }
    } else {
        emitLine("    mul t6, " + aReg + ", " + bReg);
    }

    if (sum.type == IRInstrType::ADD) {
        emitLine("    add " + accReg + ", " + accReg + ", " + productReg);
    } else {
        emitLine("    sub " + accReg + ", " + accReg + ", " + productReg);
    }

    i += 5;
    return true;
}

} // namespace toycc
