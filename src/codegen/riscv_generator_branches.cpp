#include "codegen/riscv_generator.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace toycc {

namespace {
bool hasAdditionalLoadOfSlotSplit(const std::vector<IRInstr>& instrs,
                                  const std::string& slot,
                                  size_t immediateLoadIndex) {
    // Follow real CFG successors, not just textual order. A value spilled inside
    // a loop can be loaded on the next iteration through a backward edge even
    // when there is no later textual LOAD. Stop a path as soon as the slot is
    // redefined; that STORE starts a new value version.
    std::unordered_map<std::string, size_t> labels;
    for (size_t k = 0; k < instrs.size(); ++k) {
        if (instrs[k].type == IRInstrType::LABEL) labels[instrs[k].label] = k;
    }
    std::vector<unsigned char> seen(instrs.size(), 0);
    std::vector<size_t> work;
    if (immediateLoadIndex + 1 < instrs.size()) work.push_back(immediateLoadIndex + 1);
    while (!work.empty()) {
        const size_t j = work.back(); work.pop_back();
        if (j >= instrs.size() || seen[j]) continue;
        seen[j] = 1;
        const auto& in = instrs[j];
        if (in.type == IRInstrType::LOAD && in.src1 == slot) return true;
        if (in.type == IRInstrType::STORE && in.src2 == slot) continue;
        if (in.type == IRInstrType::RET) continue;
        if (in.type == IRInstrType::JUMP) {
            auto it = labels.find(in.label);
            if (it != labels.end()) work.push_back(it->second);
            continue;
        }
        if (in.type == IRInstrType::BRANCH_ZERO || in.type == IRInstrType::BRANCH_NONZERO) {
            auto it = labels.find(in.label);
            if (it != labels.end()) work.push_back(it->second);
            if (j + 1 < instrs.size()) work.push_back(j + 1);
            continue;
        }
        if (j + 1 < instrs.size()) work.push_back(j + 1);
    }
    return false;
}
} // namespace

bool RiscvGenerator::tryEmitOptimizedCompareBranch(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 3 >= v.size()) return false;
    const auto& rhs = v[i];
    const auto& lhs = v[i + 1];
    const auto& op = v[i + 2];
    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t1") return false;
    if (!(rhs.dest == "t0" && (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
                               rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL))) return false;

    std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    if (lhsReg.empty()) return false;

    std::string rhsReg;
    auto prepareRhs = [&]() -> bool {
        if (rhs.type == IRInstrType::LI) {
            int value = static_cast<int>(std::stoll(rhs.src1));
            if (value == 0) { rhsReg = "zero"; return true; }
            auto c = currentConstantRegs.find(value);
            if (c != currentConstantRegs.end()) { rhsReg = c->second; return true; }
            emitLine("    li t6, " + rhs.src1);
            rhsReg = "t6";
            return true;
        }
        if (rhs.type == IRInstrType::LOAD) {
            rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
            if (rhsReg.empty()) {
                emitLoadFromSp("t6", physicalSlotOffset(std::stoi(rhs.src1)));
                rhsReg = "t6";
            }
            return true;
        }
        if (rhs.type == IRInstrType::LOAD_ARG) {
            emitLoadFromSp("t6", currentFrameSize + std::stoi(rhs.src1));
            rhsReg = "t6";
            return true;
        }
        if (rhs.type == IRInstrType::LOAD_GLOBAL) {
            emitLine("    la t6, " + rhs.src1);
            emitLine("    lw t6, 0(t6)");
            rhsReg = "t6";
            return true;
        }
        return false;
    };

    std::string cond;
    size_t branchIndex = 0;
    if (op.type == IRInstrType::SLT && op.dest == "t0") {
        if (i + 4 < v.size() && v[i + 3].type == IRInstrType::SEQZ &&
            v[i + 3].dest == "t0" && v[i + 3].src1 == "t0") {
            if (op.src1 == "t0" && op.src2 == "t1") cond = "le";
            else if (op.src1 == "t1" && op.src2 == "t0") cond = "ge";
            else return false;
            branchIndex = i + 4;
        } else {
            if (op.src1 == "t1" && op.src2 == "t0") cond = "lt";
            else if (op.src1 == "t0" && op.src2 == "t1") cond = "gt";
            else return false;
            branchIndex = i + 3;
        }
    } else if (op.type == IRInstrType::SUB && op.dest == "t0" &&
               op.src1 == "t1" && op.src2 == "t0" && i + 4 < v.size()) {
        const auto& norm = v[i + 3];
        if (norm.dest != "t0" || norm.src1 != "t0") return false;
        if (norm.type == IRInstrType::SEQZ) cond = "eq";
        else if (norm.type == IRInstrType::SNEZ) cond = "ne";
        else return false;
        branchIndex = i + 4;
    } else {
        return false;
    }

    if (branchIndex >= v.size()) return false;
    const auto& br = v[branchIndex];
    if ((br.type != IRInstrType::BRANCH_ZERO && br.type != IRInstrType::BRANCH_NONZERO) || br.src1 != "t0") return false;
    if (!prepareRhs()) return false;

    bool whenTrue = br.type == IRInstrType::BRANCH_NONZERO;
    std::string mnemonic, a = lhsReg, b = rhsReg;
    if (cond == "eq") mnemonic = whenTrue ? "beq" : "bne";
    else if (cond == "ne") mnemonic = whenTrue ? "bne" : "beq";
    else if (cond == "lt") mnemonic = whenTrue ? "blt" : "bge";
    else if (cond == "gt") { mnemonic = whenTrue ? "blt" : "bge"; std::swap(a, b); }
    else if (cond == "le") { mnemonic = whenTrue ? "bge" : "blt"; std::swap(a, b); }
    else if (cond == "ge") mnemonic = whenTrue ? "bge" : "blt";
    else return false;

    emitLine("    " + mnemonic + " " + a + ", " + b + ", " + br.label);
    i = branchIndex;
    return true;
}

// 把“算出 0/1 再 beqz/bnez”融合成 RISC-V 直接比较分支。
bool RiscvGenerator::tryEmitCompareBranch(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 5 >= v.size()) return false;
    const auto& lhs = v[i];
    const auto& spill = v[i + 1];
    const auto& rhs = v[i + 2];
    const auto& reload = v[i + 3];
    const auto& op = v[i + 4];
    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t0") return false;
    if (spill.type != IRInstrType::STORE || spill.src1 != "t0") return false;
    if (reload.type != IRInstrType::LOAD || reload.dest != "t1" || reload.src1 != spill.src2) return false;
    if (hasAdditionalLoadOfSlotSplit(v, spill.src2, i + 3)) return false;

    std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    if (lhsReg.empty()) return false;

    std::string rhsReg;
    auto prepareRhs = [&]() -> bool {
        if (rhs.type == IRInstrType::LI && rhs.dest == "t0") {
            int value = static_cast<int>(std::stoll(rhs.src1));
            if (value == 0) { rhsReg = "zero"; return true; }
            auto c = currentConstantRegs.find(value);
            if (c != currentConstantRegs.end()) { rhsReg = c->second; return true; }
            emitLine("    li t1, " + rhs.src1);
            rhsReg = "t1";
            return true;
        }
        if (rhs.type == IRInstrType::LOAD && rhs.dest == "t0") {
            rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
            if (rhsReg.empty()) {
                emitLoadFromSp("t1", physicalSlotOffset(std::stoi(rhs.src1)));
                rhsReg = "t1";
            }
            return true;
        }
        return false;
    };

    std::string cond;
    size_t branchIndex = 0;
    if (op.type == IRInstrType::SLT && op.dest == "t0") {
        // < 和 > 没有后续规范化；<=/>= 会紧跟 SEQZ。
        if (i + 6 < v.size() && v[i + 5].type == IRInstrType::SEQZ &&
            v[i + 5].dest == "t0" && v[i + 5].src1 == "t0") {
            if (op.src1 == "t0" && op.src2 == "t1") cond = "le";       // !(rhs < lhs)
            else if (op.src1 == "t1" && op.src2 == "t0") cond = "ge";  // !(lhs < rhs)
            else return false;
            branchIndex = i + 6;
        } else {
            if (op.src1 == "t1" && op.src2 == "t0") cond = "lt";
            else if (op.src1 == "t0" && op.src2 == "t1") cond = "gt";
            else return false;
            branchIndex = i + 5;
        }
    } else if (op.type == IRInstrType::SUB && op.dest == "t0" &&
               op.src1 == "t1" && op.src2 == "t0" && i + 6 < v.size()) {
        const auto& norm = v[i + 5];
        if (norm.dest != "t0" || norm.src1 != "t0") return false;
        if (norm.type == IRInstrType::SEQZ) cond = "eq";
        else if (norm.type == IRInstrType::SNEZ) cond = "ne";
        else return false;
        branchIndex = i + 6;
    } else {
        return false;
    }

    if (branchIndex >= v.size()) return false;
    const auto& br = v[branchIndex];
    if ((br.type != IRInstrType::BRANCH_ZERO && br.type != IRInstrType::BRANCH_NONZERO) || br.src1 != "t0") return false;
    if (!prepareRhs()) return false;

    bool whenTrue = br.type == IRInstrType::BRANCH_NONZERO;
    std::string mnemonic, a = lhsReg, b = rhsReg;
    if (cond == "eq") mnemonic = whenTrue ? "beq" : "bne";
    else if (cond == "ne") mnemonic = whenTrue ? "bne" : "beq";
    else if (cond == "lt") mnemonic = whenTrue ? "blt" : "bge";
    else if (cond == "gt") { mnemonic = whenTrue ? "blt" : "bge"; std::swap(a, b); }
    else if (cond == "le") { mnemonic = whenTrue ? "bge" : "blt"; std::swap(a, b); }
    else if (cond == "ge") mnemonic = whenTrue ? "bge" : "blt";
    else return false;

    emitLine("    " + mnemonic + " " + a + ", " + b + ", " + br.label);
    i = branchIndex;
    return true;
}

// if (x) / while (x) 这类条件直接在提升后的寄存器上分支。
bool RiscvGenerator::tryEmitDirectValueBranch(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 1 >= v.size()) return false;
    const auto& val = v[i];
    const auto& br = v[i + 1];
    if ((br.type != IRInstrType::BRANCH_ZERO && br.type != IRInstrType::BRANCH_NONZERO) || br.src1 != "t0") return false;

    if (val.type == IRInstrType::LOAD && val.dest == "t0") {
        std::string reg = promotedRegForSlot(std::stoi(val.src1));
        if (reg.empty()) return false;
        emitLine(std::string("    ") + (br.type == IRInstrType::BRANCH_ZERO ? "beqz " : "bnez ") + reg + ", " + br.label);
        i += 1;
        return true;
    }
    if (val.type == IRInstrType::LI && val.dest == "t0") {
        long long x = std::stoll(val.src1);
        bool jump = (br.type == IRInstrType::BRANCH_ZERO) ? (x == 0) : (x != 0);
        if (jump) emitLine("    j " + br.label);
        i += 1;
        return true;
    }
    return false;
}


// Fuse the common nested-expression reduction
//   x * y; acc +/-= product
// when the hot operands/accumulator already live in promoted registers.

} // namespace toycc
