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
