#include "codegen/riscv_generator.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toycc {

namespace {
struct SignedMagic { std::int32_t m=0; int shift=0; };
SignedMagic signedMagic(std::int32_t d) {
    const std::uint64_t two31=0x80000000ULL;
    const std::uint64_t ad=d<0 ? (0ULL-static_cast<std::uint32_t>(d)) : static_cast<std::uint32_t>(d);
    const std::uint64_t t=two31+(static_cast<std::uint32_t>(d)>>31);
    const std::uint64_t anc=t-1-(t%ad);
    int p=31;std::uint64_t q1=two31/anc,r1=two31-q1*anc,q2=two31/ad,r2=two31-q2*ad;
    std::uint64_t delta=0;
    do{++p;q1*=2;r1*=2;if(r1>=anc){++q1;r1-=anc;}q2*=2;r2*=2;if(r2>=ad){++q2;r2-=ad;}delta=ad-r2;}while(q1<delta||(q1==delta&&r1==0));
    std::uint32_t um=static_cast<std::uint32_t>(q2+1);if(d<0)um=0u-um;
    return {static_cast<std::int32_t>(um),p-32};
}
int positivePow2Shift(std::uint32_t x){if(!x||(x&(x-1)))return-1;int s=0;while(x>1){x>>=1;++s;}return s;}
}

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

bool RiscvGenerator::emitSignedDivConst(const std::string& dest, const std::string& src, std::int32_t d) {
    if (d==0) return false;
    if (d==1) { if(dest!=src)emitLine("    mv "+dest+", "+src); return true; }
    if (d==-1) { emitLine("    neg "+dest+", "+src); return true; }
    if (d==std::numeric_limits<std::int32_t>::min()) {
        emitLine("    li t6, -2147483648");
        emitLine("    sub "+dest+", "+src+", t6");
        emitLine("    seqz "+dest+", "+dest);
        return true;
    }
    const std::uint32_t ad=d<0?0u-static_cast<std::uint32_t>(d):static_cast<std::uint32_t>(d);
    const int sh2=positivePow2Shift(ad);
    std::string source=src;
    if(dest==src){emitLine("    mv t1, "+src);source="t1";}
    if(sh2>=0){
        emitLine("    srai t6, "+source+", 31");
        emitLine("    srli t6, t6, "+std::to_string(32-sh2));
        emitLine("    add "+dest+", "+source+", t6");
        emitLine("    srai "+dest+", "+dest+", "+std::to_string(sh2));
        if(d<0)emitLine("    neg "+dest+", "+dest);
        return true;
    }
    const auto mg=signedMagic(d);
    emitLine("    li t6, "+std::to_string(mg.m));
    emitLine("    mulh "+dest+", "+source+", t6");
    if(d>0&&mg.m<0)emitLine("    add "+dest+", "+dest+", "+source);
    if(d<0&&mg.m>0)emitLine("    sub "+dest+", "+dest+", "+source);
    if(mg.shift>0)emitLine("    srai "+dest+", "+dest+", "+std::to_string(mg.shift));
    emitLine("    srli t6, "+dest+", 31");
    emitLine("    add "+dest+", "+dest+", t6");
    return true;
}

bool RiscvGenerator::emitSignedRemConst(const std::string& dest, const std::string& src, std::int32_t d) {
    if(d==0)return false;
    if(d==1||d==-1){emitLine("    li "+dest+", 0");return true;}
    if(src!="t0")emitLine("    mv t0, "+src);
    if(!emitSignedDivConst("t1","t0",d))return false;
    const std::uint32_t ad=d<0?0u-static_cast<std::uint32_t>(d):static_cast<std::uint32_t>(d);
    const int sh=positivePow2Shift(ad);
    if(sh>=0){emitLine("    slli t1, t1, "+std::to_string(sh));if(d<0)emitLine("    neg t1, t1");}
    else {emitLine("    li t6, "+std::to_string(d));emitLine("    mul t1, t1, t6");}
    emitLine("    sub "+dest+", t0, t1");return true;
}

bool RiscvGenerator::isCoreBinaryOp(IRInstrType type) {
    return type == IRInstrType::ADD || type == IRInstrType::SUB ||
           type == IRInstrType::MUL || type == IRInstrType::DIV ||
           type == IRInstrType::REM || type == IRInstrType::SLT;
}

// A spill-store may only be removed when the matching immediate reload is its
// last use.  IR optimization can make an originally one-shot temporary slot
// visible again later (e.g. copy/CSE propagation).  In that case omitting the
// physical store would leave the later LOAD reading stale stack contents.
static bool hasAdditionalLoadOfSlot(const std::vector<IRInstr>& instrs,
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
                emitLoadFromSp(instr.dest, physicalSlotOffset(logicalOffset));
            }
            break;
        }
        case IRInstrType::STORE: {
            int logicalOffset = std::stoi(instr.src2);
            auto p = currentPromotedSlots.find(logicalOffset);
            if (p != currentPromotedSlots.end()) {
                if (instr.src1 != p->second) emitLine("    mv " + p->second + ", " + instr.src1);
            } else {
                emitStoreToSp(instr.src1, physicalSlotOffset(logicalOffset));
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

    const bool spillLeft = op.src1 == "t1" && op.src2 == "t0";
    const bool rhsLeft = op.src1 == "t0" && op.src2 == "t1";
    if (!spillLeft && !rhsLeft) return false;

    // The store is elided by this peephole, so the immediate reload must be
    // the final LOAD of that slot in the optimized IR.
    if (hasAdditionalLoadOfSlot(v, st.src2, i + 2)) return false;

    const bool simpleRhs = rhs.dest == "t0" &&
        (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
         rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL);
    if (!simpleRhs) return false;

    // CSE commonly exposes `STORE tmp; LOAD tmp -> t0; LOAD tmp -> t1; OP`.
    // The spill store is intentionally elided by this peephole, so reloading
    // that same logical slot would read stale stack memory.  Both operands are
    // already the value currently held in t0; operate on it directly instead.
    // Besides fixing the stale-reload corner case, this removes the entire
    // temporary slot from common-subexpression hot paths.
    if (rhs.type == IRInstrType::LOAD && rhs.src1 == st.src2) {
        switch (op.type) {
            case IRInstrType::ADD:
                emitLine("    slli t0, t0, 1");
                break;
            case IRInstrType::SUB:
                emitLine("    li t0, 0");
                break;
            case IRInstrType::MUL:
                emitLine("    mul t0, t0, t0");
                break;
            case IRInstrType::DIV:
                // ToyC tests contain no undefined behaviour; x/x is therefore
                // reached only for x != 0.
                emitLine("    li t0, 1");
                break;
            case IRInstrType::REM:
            case IRInstrType::SLT:
                emitLine("    li t0, 0");
                break;
            default:
                return false;
        }
        i += 3;
        return true;
    }

    // At entry t0 still contains the value that would have been spilled.
    // If the RHS is itself a promoted local, operate on it directly instead of
    // shuffling the old t0 through t1 and reloading the RHS through t0.
    if (rhs.type == IRInstrType::LOAD) {
        const int rhsOff = std::stoi(rhs.src1);
        const std::string rhsReg = promotedRegForSlot(rhsOff);
        if (!rhsReg.empty()) {
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
            if (spillLeft)
                emitLine("    " + std::string(mnemonic) + " t0, t0, " + rhsReg);
            else
                emitLine("    " + std::string(mnemonic) + " t0, " + rhsReg + ", t0");
            i += 3;
            return true;
        }
    }

    if (rhs.type == IRInstrType::LI) {
        const auto imm = static_cast<std::int32_t>(std::stoll(rhs.src1));
        const auto cached = currentConstantRegs.find(imm);
        const std::string cachedReg = cached == currentConstantRegs.end() ? std::string{} : cached->second;

        switch (op.type) {
            case IRInstrType::ADD:
                if (imm == 0) {
                    // t0 already is the result.
                } else if (fitsImm12(imm)) {
                    emitLine("    addi t0, t0, " + std::to_string(imm));
                } else if (!cachedReg.empty()) {
                    emitLine("    add t0, t0, " + cachedReg);
                } else {
                    emitLine("    li t1, " + std::to_string(imm));
                    emitLine("    add t0, t0, t1");
                }
                i += 3;
                return true;

            case IRInstrType::SUB:
                if (spillLeft) {
                    const std::int64_t neg = -static_cast<std::int64_t>(imm);
                    if (imm == 0) {
                        // unchanged
                    } else if (neg >= -2048 && neg <= 2047) {
                        emitLine("    addi t0, t0, " + std::to_string(neg));
                    } else if (!cachedReg.empty()) {
                        emitLine("    sub t0, t0, " + cachedReg);
                    } else {
                        emitLine("    li t1, " + std::to_string(imm));
                        emitLine("    sub t0, t0, t1");
                    }
                } else {
                    if (imm == 0) {
                        emitLine("    neg t0, t0");
                    } else {
                        std::string r = cachedReg;
                        if (r.empty()) {
                            emitLine("    li t1, " + std::to_string(imm));
                            r = "t1";
                        }
                        emitLine("    sub t0, " + r + ", t0");
                    }
                }
                i += 3;
                return true;

            case IRInstrType::MUL: {
                if (imm == 0) emitLine("    li t0, 0");
                else if (imm == 1) {}
                else if (imm == -1) emitLine("    neg t0, t0");
                else {
                    const std::uint32_t u = static_cast<std::uint32_t>(imm);
                    const int sh = imm > 0 ? positivePow2Shift(u) : -1;
                    if (sh >= 0) emitLine("    slli t0, t0, " + std::to_string(sh));
                    else if (!cachedReg.empty()) emitLine("    mul t0, t0, " + cachedReg);
                    else {
                        emitLine("    li t1, " + std::to_string(imm));
                        emitLine("    mul t0, t0, t1");
                    }
                }
                i += 3;
                return true;
            }

            case IRInstrType::DIV:
                if (spillLeft) {
                    if (imm == 0 || !emitSignedDivConst("t0", "t0", imm)) return false;
                } else {
                    std::string r = cachedReg;
                    if (r.empty()) {
                        emitLine("    li t1, " + std::to_string(imm));
                        r = "t1";
                    }
                    emitLine("    div t0, " + r + ", t0");
                }
                i += 3;
                return true;

            case IRInstrType::REM:
                if (spillLeft) {
                    if (imm == 0 || !emitSignedRemConst("t0", "t0", imm)) return false;
                } else {
                    std::string r = cachedReg;
                    if (r.empty()) {
                        emitLine("    li t1, " + std::to_string(imm));
                        r = "t1";
                    }
                    emitLine("    rem t0, " + r + ", t0");
                }
                i += 3;
                return true;

            case IRInstrType::SLT:
                if (spillLeft && fitsImm12(imm)) {
                    emitLine("    slti t0, t0, " + std::to_string(imm));
                } else {
                    std::string r = cachedReg;
                    if (r.empty()) {
                        emitLine("    li t1, " + std::to_string(imm));
                        r = "t1";
                    }
                    if (spillLeft) emitLine("    slt t0, t0, " + r);
                    else emitLine("    slt t0, " + r + ", t0");
                }
                i += 3;
                return true;

            default:
                return false;
        }
    }

    // Conservative fallback for non-promoted loads / arguments / globals.
    emitLine("    mv t1, t0");
    emitInstr(rhs);
    emitInstr(op);
    i += 3;
    return true;
}

std::string RiscvGenerator::promotedRegForSlot(int logicalOffset) const {
    auto it = currentPromotedSlots.find(logicalOffset);
    return it == currentPromotedSlots.end() ? std::string{} : it->second;
}

int RiscvGenerator::physicalSlotOffset(int logicalOffset) const {
    if (!optimize) return currentLocalBase + logicalOffset;
    auto it = currentPhysicalSlots.find(logicalOffset);
    if (it == currentPhysicalSlots.end()) return currentLocalBase + logicalOffset;
    return currentLocalBase + it->second;
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
                emitLoadFromSp(dest, physicalSlotOffset(logicalOffset));
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
                if (dstReg.empty()) emitStoreToSp(srcReg, physicalSlotOffset(dstOff));
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

} // namespace toycc
