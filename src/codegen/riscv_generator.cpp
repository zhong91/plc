#include "codegen/riscv_generator.h"
#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace toycc {

bool RiscvGenerator::fitsImm12(int value) const {
    return value >= -2048 && value <= 2047;
}

void RiscvGenerator::emitAdjustSp(int delta) {
    if (delta == 0) return;
    if (fitsImm12(delta)) {
        emitLine("    addi sp, sp, " + std::to_string(delta));
        return;
    }
    emitLine("    li t6, " + std::to_string(delta));
    emitLine("    add sp, sp, t6");
}

void RiscvGenerator::emitLoadFromSp(const std::string& dest, int offset) {
    if (fitsImm12(offset)) {
        emitLine("    lw " + dest + ", " + std::to_string(offset) + "(sp)");
        return;
    }
    emitLine("    li t6, " + std::to_string(offset));
    emitLine("    add t6, sp, t6");
    emitLine("    lw " + dest + ", 0(t6)");
}

void RiscvGenerator::emitStoreToSp(const std::string& src, int offset) {
    if (fitsImm12(offset)) {
        emitLine("    sw " + src + ", " + std::to_string(offset) + "(sp)");
        return;
    }
    emitLine("    li t6, " + std::to_string(offset));
    emitLine("    add t6, sp, t6");
    emitLine("    sw " + src + ", 0(t6)");
}

bool RiscvGenerator::isCoreBinaryOp(IRInstrType type) {
    return type == IRInstrType::ADD || type == IRInstrType::SUB ||
           type == IRInstrType::MUL || type == IRInstrType::DIV ||
           type == IRInstrType::REM || type == IRInstrType::SLT;
}

std::vector<std::pair<int, std::string>>
RiscvGenerator::choosePromotedSlots(const IRFunction& func) const {
    std::unordered_map<std::string, size_t> labelIndex;
    for (size_t i = 0; i < func.instrs.size(); ++i) {
        if (func.instrs[i].type == IRInstrType::LABEL) {
            labelIndex[func.instrs[i].label] = i;
        }
    }

    // 先找出后续窥孔优化会完全消掉的临时 spill 槽，避免浪费 s 寄存器保存它们。
    std::unordered_map<int, bool> eliminatedSpills;
    for (size_t i = 0; i + 3 < func.instrs.size(); ++i) {
        const auto& st = func.instrs[i];
        const auto& rhs = func.instrs[i + 1];
        const auto& ld = func.instrs[i + 2];
        const auto& op = func.instrs[i + 3];
        if (st.type != IRInstrType::STORE || st.src1 != "t0") continue;
        if (ld.type != IRInstrType::LOAD || ld.dest != "t1" || ld.src1 != st.src2) continue;
        if (!isCoreBinaryOp(op.type) || op.dest != "t0") continue;
        if (!((op.src1 == "t1" && op.src2 == "t0") ||
              (op.src1 == "t0" && op.src2 == "t1"))) continue;
        const bool simpleRhs = rhs.dest == "t0" &&
            (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
             rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL);
        if (simpleRhs) eliminatedSpills[std::stoi(st.src2)] = true;
    }

    std::vector<int> loopWeight(func.instrs.size(), 1);
    for (size_t i = 0; i < func.instrs.size(); ++i) {
        const auto& ins = func.instrs[i];
        if (ins.type != IRInstrType::JUMP) continue;
        auto it = labelIndex.find(ins.label);
        if (it == labelIndex.end() || it->second >= i) continue;
        for (size_t j = it->second; j <= i; ++j) {
            loopWeight[j] += 32;
        }
    }

    std::unordered_map<int, long long> score;
    std::unordered_map<int, int> rawCount;
    for (size_t i = 0; i < func.instrs.size(); ++i) {
        const auto& ins = func.instrs[i];
        int off = -1;
        if (ins.type == IRInstrType::LOAD) {
            off = std::stoi(ins.src1);
        } else if (ins.type == IRInstrType::STORE) {
            off = std::stoi(ins.src2);
        }
        if (off >= 0 && !eliminatedSpills.count(off)) {
            score[off] += loopWeight[i];
            rawCount[off]++;
        }
    }

    struct Candidate { int offset; long long score; int count; };
    std::vector<Candidate> candidates;
    for (const auto& [off, sc] : score) {
        if (rawCount[off] >= 2) candidates.push_back({off, sc, rawCount[off]});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.count != b.count) return a.count > b.count;
        return a.offset < b.offset;
    });

    static const char* regs[] = {
        "s0", "s1", "s2", "s3", "s4", "s5",
        "s6", "s7", "s8", "s9", "s10", "s11"
    };
    const size_t n = std::min<size_t>(candidates.size(), 12);
    std::vector<std::pair<int, std::string>> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++i) result.push_back({candidates[i].offset, regs[i]});
    return result;
}

void RiscvGenerator::emitInstr(const IRInstr& instr) {
    switch (instr.type) {
        case IRInstrType::LI:
            emitLine("    li " + instr.dest + ", " + instr.src1);
            break;
        case IRInstrType::LOAD: {
            int logicalOffset = std::stoi(instr.src1);
            auto p = currentPromotedSlots.find(logicalOffset);
            if (p != currentPromotedSlots.end()) {
                if (instr.dest != p->second) emitLine("    mv " + instr.dest + ", " + p->second);
            } else {
                emitLoadFromSp(instr.dest, currentLocalBase + logicalOffset);
            }
            break;
        }
        case IRInstrType::STORE: {
            int logicalOffset = std::stoi(instr.src2);
            auto p = currentPromotedSlots.find(logicalOffset);
            if (p != currentPromotedSlots.end()) {
                if (instr.src1 != p->second) emitLine("    mv " + p->second + ", " + instr.src1);
            } else {
                emitStoreToSp(instr.src1, currentLocalBase + logicalOffset);
            }
            break;
        }
        case IRInstrType::LOAD_ARG: {
            int argOffset = std::stoi(instr.src1);
            emitLoadFromSp(instr.dest, currentFrameSize + argOffset);
            break;
        }
        case IRInstrType::STORE_ARG: {
            int argOffset = std::stoi(instr.src2);
            emitStoreToSp(instr.src1, argOffset);
            break;
        }
        case IRInstrType::MV:
            if (!optimize || instr.dest != instr.src1) emitLine("    mv " + instr.dest + ", " + instr.src1);
            break;
        case IRInstrType::ADD:
            emitLine("    add " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SUB:
            emitLine("    sub " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::MUL:
            emitLine("    mul " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::DIV:
            emitLine("    div " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::REM:
            emitLine("    rem " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SLT:
            emitLine("    slt " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SEQZ:
            emitLine("    seqz " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::SNEZ:
            emitLine("    snez " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::LABEL:
            emitLine(instr.label + ":"); break;
        case IRInstrType::JUMP:
            emitLine("    j " + instr.label); break;
        case IRInstrType::BRANCH_ZERO:
            emitLine("    beqz " + instr.src1 + ", " + instr.label); break;
        case IRInstrType::BRANCH_NONZERO:
            emitLine("    bnez " + instr.src1 + ", " + instr.label); break;
        case IRInstrType::LOAD_GLOBAL: {
            const std::string addrReg = optimize ? "t6" : "t1";
            emitLine("    la " + addrReg + ", " + instr.src1);
            emitLine("    lw " + instr.dest + ", 0(" + addrReg + ")");
            break;
        }
        case IRInstrType::STORE_GLOBAL: {
            const std::string addrReg = optimize ? "t6" : "t1";
            emitLine("    la " + addrReg + ", " + instr.src1);
            emitLine("    sw " + instr.src2 + ", 0(" + addrReg + ")");
            break;
        }
        case IRInstrType::CALL:
            emitLine("    call " + instr.src1); break;
        case IRInstrType::RET:
            break;
        default:
            break;
    }
}

bool RiscvGenerator::tryEmitSpillPeephole(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 3 >= v.size()) return false;
    const auto& st = v[i];
    const auto& rhs = v[i + 1];
    const auto& ld = v[i + 2];
    const auto& op = v[i + 3];
    if (st.type != IRInstrType::STORE || st.src1 != "t0") return false;
    if (ld.type != IRInstrType::LOAD || ld.dest != "t1" || ld.src1 != st.src2) return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0") return false;
    if (!((op.src1 == "t1" && op.src2 == "t0") ||
          (op.src1 == "t0" && op.src2 == "t1"))) return false;

    const bool simpleRhs = rhs.dest == "t0" &&
        (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
         rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL);
    if (!simpleRhs) return false;

    if (rhs.type == IRInstrType::LI) {
        emitLine("    li t1, " + rhs.src1);
        IRInstr mapped = op;
        auto mapSrc = [](const std::string& s) {
            if (s == "t1") return std::string("t0");
            if (s == "t0") return std::string("t1");
            return s;
        };
        mapped.src1 = mapSrc(op.src1);
        mapped.src2 = mapSrc(op.src2);
        emitInstr(mapped);
    } else {
        emitLine("    mv t1, t0");
        emitInstr(rhs);
        emitInstr(op);
    }
    i += 3;
    return true;
}


std::string RiscvGenerator::promotedRegForSlot(int logicalOffset) const {
    auto it = currentPromotedSlots.find(logicalOffset);
    return it == currentPromotedSlots.end() ? std::string{} : it->second;
}

bool RiscvGenerator::emitSimpleValueToReg(const IRInstr& instr, const std::string& dest) {
    switch (instr.type) {
        case IRInstrType::LI:
            if (instr.dest != "t0") return false;
            emitLine("    li " + dest + ", " + instr.src1);
            return true;
        case IRInstrType::LOAD: {
            if (instr.dest != "t0") return false;
            int logicalOffset = std::stoi(instr.src1);
            std::string srcReg = promotedRegForSlot(logicalOffset);
            if (!srcReg.empty()) {
                if (dest != srcReg) emitLine("    mv " + dest + ", " + srcReg);
            } else {
                emitLoadFromSp(dest, currentLocalBase + logicalOffset);
            }
            return true;
        }
        case IRInstrType::LOAD_ARG: {
            if (instr.dest != "t0") return false;
            int argOffset = std::stoi(instr.src1);
            emitLoadFromSp(dest, currentFrameSize + argOffset);
            return true;
        }
        case IRInstrType::LOAD_GLOBAL:
            if (instr.dest != "t0") return false;
            emitLine("    la t6, " + instr.src1);
            emitLine("    lw " + dest + ", 0(t6)");
            return true;
        case IRInstrType::MV:
            if (instr.dest != "t0") return false;
            if (dest != instr.src1) emitLine("    mv " + dest + ", " + instr.src1);
            return true;
        default:
            return false;
    }
}

// 合并常见的“产生 t0 -> 立刻写槽/返回”模式，避免无意义的 t0 中转。
bool RiscvGenerator::tryEmitSimplePair(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 1 >= v.size()) return false;
    const auto& a = v[i];
    const auto& b = v[i + 1];

    if (b.type == IRInstrType::STORE && b.src1 == "t0") {
        int dstOff = std::stoi(b.src2);
        std::string dstReg = promotedRegForSlot(dstOff);
        if (!dstReg.empty() && emitSimpleValueToReg(a, dstReg)) {
            i += 1;
            return true;
        }

        // LOAD x -> STORE y 可以直接在寄存器/内存之间搬运，不必经过 t0。
        if (a.type == IRInstrType::LOAD && a.dest == "t0") {
            int srcOff = std::stoi(a.src1);
            std::string srcReg = promotedRegForSlot(srcOff);
            if (!srcReg.empty()) {
                if (dstReg.empty()) emitStoreToSp(srcReg, currentLocalBase + dstOff);
                else if (dstReg != srcReg) emitLine("    mv " + dstReg + ", " + srcReg);
                i += 1;
                return true;
            }
        }
    }

    // return / 参数装载：LOAD promoted -> MV aX,t0 直接使用 s 寄存器。
    if (b.type == IRInstrType::MV && b.src1 == "t0") {
        if (a.type == IRInstrType::LOAD && a.dest == "t0") {
            std::string srcReg = promotedRegForSlot(std::stoi(a.src1));
            if (!srcReg.empty()) {
                if (b.dest != srcReg) emitLine("    mv " + b.dest + ", " + srcReg);
                i += 1;
                return true;
            }
        }
        if (a.type == IRInstrType::LI && a.dest == "t0") {
            emitLine("    li " + b.dest + ", " + a.src1);
            i += 1;
            return true;
        }
    }
    return false;
}

// 识别 IRBuilder 的完整赋值模式：
// LOAD lhs; STORE spill; RHS; LOAD spill->t1; OP; STORE dst
// 对寄存器提升后的热点变量直接在 s 寄存器上运算。
bool RiscvGenerator::tryEmitDirectBinaryUpdate(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 5 >= v.size()) return false;
    const auto& lhs = v[i];
    const auto& spill = v[i + 1];
    const auto& rhs = v[i + 2];
    const auto& reload = v[i + 3];
    const auto& op = v[i + 4];
    const auto& store = v[i + 5];

    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t0") return false;
    if (spill.type != IRInstrType::STORE || spill.src1 != "t0") return false;
    if (reload.type != IRInstrType::LOAD || reload.dest != "t1" || reload.src1 != spill.src2) return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0" || op.src1 != "t1" || op.src2 != "t0") return false;
    if (store.type != IRInstrType::STORE || store.src1 != "t0") return false;

    std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    std::string dstReg = promotedRegForSlot(std::stoi(store.src2));
    if (lhsReg.empty() || dstReg.empty()) return false;

    auto copyLhs = [&]() {
        if (dstReg != lhsReg) emitLine("    mv " + dstReg + ", " + lhsReg);
    };

    if (rhs.type == IRInstrType::LI && rhs.dest == "t0") {
        long long imm = std::stoll(rhs.src1);
        switch (op.type) {
            case IRInstrType::ADD:
                if (imm == 0) { copyLhs(); }
                else if (imm >= -2048 && imm <= 2047) {
                    emitLine("    addi " + dstReg + ", " + lhsReg + ", " + std::to_string(imm));
                } else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    add " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            case IRInstrType::SUB: {
                long long neg = -imm;
                if (imm == 0) { copyLhs(); }
                else if (neg >= -2048 && neg <= 2047) {
                    emitLine("    addi " + dstReg + ", " + lhsReg + ", " + std::to_string(neg));
                } else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    sub " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            }
            case IRInstrType::MUL:
                if (imm == 0) emitLine("    li " + dstReg + ", 0");
                else if (imm == 1) copyLhs();
                else if (imm == -1) emitLine("    neg " + dstReg + ", " + lhsReg);
                else if (imm > 0 && (imm & (imm - 1)) == 0) {
                    int sh = 0; long long x = imm; while (x > 1) { ++sh; x >>= 1; }
                    emitLine("    slli " + dstReg + ", " + lhsReg + ", " + std::to_string(sh));
                } else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    mul " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            case IRInstrType::DIV:
                if (imm == 1) copyLhs();
                else if (imm == -1) emitLine("    neg " + dstReg + ", " + lhsReg);
                else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    div " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            case IRInstrType::REM:
                if (imm == 1 || imm == -1) emitLine("    li " + dstReg + ", 0");
                else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    rem " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            case IRInstrType::SLT:
                if (imm >= -2048 && imm <= 2047) {
                    emitLine("    slti " + dstReg + ", " + lhsReg + ", " + std::to_string(imm));
                } else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    slt " + dstReg + ", " + lhsReg + ", t1");
                }
                break;
            default:
                return false;
        }
        i += 5;
        return true;
    }

    if (rhs.type == IRInstrType::LOAD && rhs.dest == "t0") {
        int rhsOff = std::stoi(rhs.src1);
        std::string rhsReg = promotedRegForSlot(rhsOff);
        if (rhsReg.empty()) {
            emitLoadFromSp("t1", currentLocalBase + rhsOff);
            rhsReg = "t1";
        }
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
        emitLine("    " + std::string(mnemonic) + " " + dstReg + ", " + lhsReg + ", " + rhsReg);
        i += 5;
        return true;
    }
    return false;
}


// v3 IR 优化后，二元赋值常被压成：
//   RHS -> t0; LOAD lhs -> t1; OP t0,t1,t0; STORE dst
// 直接在提升后的 s 寄存器上完成，继续保留 v2 的热点循环收益。
bool RiscvGenerator::tryEmitOptimizedBinaryUpdate(const std::vector<IRInstr>& v, size_t& i) {
    if (i + 3 >= v.size()) return false;
    const auto& rhs = v[i];
    const auto& lhs = v[i + 1];
    const auto& op = v[i + 2];
    const auto& store = v[i + 3];

    if (lhs.type != IRInstrType::LOAD || lhs.dest != "t1") return false;
    if (!isCoreBinaryOp(op.type) || op.dest != "t0" || op.src1 != "t1" || op.src2 != "t0") return false;
    if (store.type != IRInstrType::STORE || store.src1 != "t0") return false;
    if (!(rhs.dest == "t0" && (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
                               rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL))) return false;

    std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    std::string dstReg = promotedRegForSlot(std::stoi(store.src2));
    if (lhsReg.empty() || dstReg.empty()) return false;

    auto copyLhs = [&]() {
        if (dstReg != lhsReg) emitLine("    mv " + dstReg + ", " + lhsReg);
    };

    if (rhs.type == IRInstrType::LI) {
        long long imm = std::stoll(rhs.src1);
        switch (op.type) {
            case IRInstrType::ADD:
                if (imm == 0) copyLhs();
                else if (imm >= -2048 && imm <= 2047)
                    emitLine("    addi " + dstReg + ", " + lhsReg + ", " + std::to_string(imm));
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    add " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            case IRInstrType::SUB: {
                long long neg = -imm;
                if (imm == 0) copyLhs();
                else if (neg >= -2048 && neg <= 2047)
                    emitLine("    addi " + dstReg + ", " + lhsReg + ", " + std::to_string(neg));
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    sub " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            }
            case IRInstrType::MUL:
                if (imm == 0) emitLine("    li " + dstReg + ", 0");
                else if (imm == 1) copyLhs();
                else if (imm == -1) emitLine("    neg " + dstReg + ", " + lhsReg);
                else if (imm > 0 && (imm & (imm - 1)) == 0) {
                    int sh = 0; long long x = imm; while (x > 1) { ++sh; x >>= 1; }
                    emitLine("    slli " + dstReg + ", " + lhsReg + ", " + std::to_string(sh));
                } else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    mul " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            case IRInstrType::DIV:
                if (imm == 1) copyLhs();
                else if (imm == -1) emitLine("    neg " + dstReg + ", " + lhsReg);
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    div " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            case IRInstrType::REM:
                if (imm == 1 || imm == -1) emitLine("    li " + dstReg + ", 0");
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    rem " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            case IRInstrType::SLT:
                if (imm >= -2048 && imm <= 2047)
                    emitLine("    slti " + dstReg + ", " + lhsReg + ", " + std::to_string(imm));
                else {
                    emitLine("    li t6, " + std::to_string(imm));
                    emitLine("    slt " + dstReg + ", " + lhsReg + ", t6");
                }
                break;
            default:
                return false;
        }
        i += 3;
        return true;
    }

    std::string rhsReg;
    if (rhs.type == IRInstrType::LOAD) {
        rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
        if (rhsReg.empty()) {
            emitLoadFromSp("t6", currentLocalBase + std::stoi(rhs.src1));
            rhsReg = "t6";
        }
    } else if (rhs.type == IRInstrType::LOAD_ARG) {
        emitLoadFromSp("t6", currentFrameSize + std::stoi(rhs.src1));
        rhsReg = "t6";
    } else if (rhs.type == IRInstrType::LOAD_GLOBAL) {
        emitLine("    la t6, " + rhs.src1);
        emitLine("    lw t6, 0(t6)");
        rhsReg = "t6";
    } else {
        return false;
    }

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
    emitLine("    " + std::string(mnemonic) + " " + dstReg + ", " + lhsReg + ", " + rhsReg);
    i += 3;
    return true;
}

// v3 IR 优化会删除比较表达式的临时 spill，形成：
//   RHS -> t0; LOAD lhs -> t1; compare[/normalize]; branch
// 直接映射到 beq/bne/blt/bge，避免重新退化为 slt + beqz。
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
            emitLine("    li t6, " + rhs.src1);
            rhsReg = "t6";
            return true;
        }
        if (rhs.type == IRInstrType::LOAD) {
            rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
            if (rhsReg.empty()) {
                emitLoadFromSp("t6", currentLocalBase + std::stoi(rhs.src1));
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

    // v6 safety path: for normalized comparisons (==, !=, <=, >=), preserve
    // the IR operation order exactly instead of algebraically inverting the
    // condition into a single branch.  We still eliminate all stack spills.
    if (cond == "eq" || cond == "ne" || cond == "le" || cond == "ge") {
        auto mapSrc = [&](const std::string& r) -> std::string {
            if (r == "t1") return lhsReg;
            if (r == "t0") return rhsReg;
            return r;
        };
        if (op.type == IRInstrType::SUB) {
            emitLine("    sub t0, " + mapSrc(op.src1) + ", " + mapSrc(op.src2));
        } else if (op.type == IRInstrType::SLT) {
            emitLine("    slt t0, " + mapSrc(op.src1) + ", " + mapSrc(op.src2));
        } else {
            return false;
        }
        const auto& norm = v[branchIndex - 1];
        if (norm.type == IRInstrType::SEQZ) emitLine("    seqz t0, t0");
        else if (norm.type == IRInstrType::SNEZ) emitLine("    snez t0, t0");
        else return false;
        emitLine(std::string("    ") +
                 (br.type == IRInstrType::BRANCH_ZERO ? "beqz t0, " : "bnez t0, ") + br.label);
        i = branchIndex;
        return true;
    }

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

    std::string lhsReg = promotedRegForSlot(std::stoi(lhs.src1));
    if (lhsReg.empty()) return false;

    std::string rhsReg;
    auto prepareRhs = [&]() -> bool {
        if (rhs.type == IRInstrType::LI && rhs.dest == "t0") {
            emitLine("    li t1, " + rhs.src1);
            rhsReg = "t1";
            return true;
        }
        if (rhs.type == IRInstrType::LOAD && rhs.dest == "t0") {
            rhsReg = promotedRegForSlot(std::stoi(rhs.src1));
            if (rhsReg.empty()) {
                emitLoadFromSp("t1", currentLocalBase + std::stoi(rhs.src1));
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

    // Same safety rule for the original spill-form IR: keep strict < and >
    // direct branches, but execute normalized comparisons exactly as the IR.
    if (cond == "eq" || cond == "ne" || cond == "le" || cond == "ge") {
        // lhs is already available in its promoted s-register; rhs is in rhsReg.
        auto mapSrc = [&](const std::string& r) -> std::string {
            if (r == "t1") return lhsReg;
            if (r == "t0") return rhsReg;
            return r;
        };
        if (op.type == IRInstrType::SUB) {
            emitLine("    sub t0, " + mapSrc(op.src1) + ", " + mapSrc(op.src2));
        } else if (op.type == IRInstrType::SLT) {
            emitLine("    slt t0, " + mapSrc(op.src1) + ", " + mapSrc(op.src2));
        } else {
            return false;
        }
        const auto& norm = v[branchIndex - 1];
        if (norm.type == IRInstrType::SEQZ) emitLine("    seqz t0, t0");
        else if (norm.type == IRInstrType::SNEZ) emitLine("    snez t0, t0");
        else return false;
        emitLine(std::string("    ") +
                 (br.type == IRInstrType::BRANCH_ZERO ? "beqz t0, " : "bnez t0, ") + br.label);
        i = branchIndex;
        return true;
    }

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

void RiscvGenerator::generate(const IRProgram& program) {
    if (!program.globalVars.empty()) {
        emitLine(".data");
        for (const auto& [name, value] : program.globalVars) {
            emitLine(".globl " + name);
            emitLine(name + ":");
            emitLine("    .word " + std::to_string(value));
        }
    }
    emitLine(".section .text");
    for (const auto& func : program.functions) emitLine(".globl " + func.name);

    for (const auto& func : program.functions) {
        emitLine(func.name + ":");
        currentLocalBase = func.outgoingArgSize;

        auto promoted = optimize ? choosePromotedSlots(func)
                                : std::vector<std::pair<int, std::string>>{};
        currentPromotedSlots.clear();
        for (const auto& [off, reg] : promoted) currentPromotedSlots[off] = reg;

        bool hasCall = false;
        for (const auto& ins : func.instrs) {
            if (ins.type == IRInstrType::CALL) { hasCall = true; break; }
        }

        int savedBase = func.outgoingArgSize + func.localSize;
        int savedBytes = static_cast<int>(promoted.size()) * 4;
        int raOffset = 0;
        int frameSize = 0;
        if (optimize) {
            // 优化模式：叶函数不保存 ra；热栈槽对应的 s 寄存器保存区放在 locals 之后。
            raOffset = savedBase + savedBytes;
            frameSize = raOffset + (hasCall ? 4 : 0);
            if (frameSize % 16 != 0) frameSize += 16 - (frameSize % 16);
        } else {
            // 非优化模式保持原来的帧布局，最大限度保护现有功能测试行为。
            frameSize = func.outgoingArgSize + func.localSize + 4;
            if (frameSize % 16 != 0) frameSize += 16 - (frameSize % 16);
            raOffset = frameSize - 4;
            hasCall = true;
        }
        currentFrameSize = frameSize;

        emitAdjustSp(-frameSize);
        for (size_t r = 0; r < promoted.size(); ++r) {
            emitStoreToSp(promoted[r].second, savedBase + static_cast<int>(r) * 4);
        }
        if (hasCall) emitStoreToSp("ra", raOffset);

        for (size_t i = 0; i < func.instrs.size(); ++i) {
            if (optimize && tryEmitOptimizedCompareBranch(func.instrs, i)) continue;
            if (optimize && tryEmitCompareBranch(func.instrs, i)) continue;
            if (optimize && tryEmitOptimizedBinaryUpdate(func.instrs, i)) continue;
            if (optimize && tryEmitDirectBinaryUpdate(func.instrs, i)) continue;
            if (optimize && tryEmitDirectValueBranch(func.instrs, i)) continue;
            if (optimize && tryEmitSimplePair(func.instrs, i)) continue;
            if (optimize && tryEmitSpillPeephole(func.instrs, i)) continue;
            if (optimize && func.instrs[i].type == IRInstrType::JUMP &&
                i + 1 < func.instrs.size() &&
                func.instrs[i + 1].type == IRInstrType::LABEL &&
                func.instrs[i].label == func.instrs[i + 1].label) {
                continue;
            }
            emitInstr(func.instrs[i]);
        }

        if (hasCall) emitLoadFromSp("ra", raOffset);
        for (size_t r = promoted.size(); r > 0; --r) {
            emitLoadFromSp(promoted[r - 1].second, savedBase + static_cast<int>(r - 1) * 4);
        }
        emitAdjustSp(frameSize);
        emitLine("    ret");

        currentPromotedSlots.clear();
        currentFrameSize = 0;
        currentLocalBase = 0;
    }
}

} // namespace toycc
