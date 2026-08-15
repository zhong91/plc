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
    for (size_t j = immediateLoadIndex + 1; j < instrs.size(); ++j) {
        if (instrs[j].type == IRInstrType::LOAD && instrs[j].src1 == slot) {
            return true;
        }
    }
    return false;
}
} // namespace

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
    if (hasAdditionalLoadOfSlotSplit(v, spill.src2, i + 3)) return false;
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
                emitSignedDivConst(dstReg,lhsReg,static_cast<std::int32_t>(imm));
                break;
            case IRInstrType::REM:
                emitSignedRemConst(dstReg,lhsReg,static_cast<std::int32_t>(imm));
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
            emitLoadFromSp("t1", physicalSlotOffset(rhsOff));
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
            emitLoadFromSp("t6", physicalSlotOffset(std::stoi(rhs.src1)));
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

} // namespace toycc
