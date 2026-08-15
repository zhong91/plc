#include "codegen/riscv_generator.h"
#include <iostream>
#include <string>

namespace toycc {

bool RiscvGenerator::fitsImm12(int value) const {
    return value >= -2048 && value <= 2047;
}

void RiscvGenerator::emitAdjustSp(int delta) {
    if (fitsImm12(delta)) {
        emitLine("    addi sp, sp, " + std::to_string(delta));
        return;
    }

    // addi 只有 12 位有符号立即数；大栈帧使用 li + add。
    emitLine("    li t6, " + std::to_string(delta));
    emitLine("    add sp, sp, t6");
}

void RiscvGenerator::emitLoadFromSp(const std::string& dest, int offset) {
    if (fitsImm12(offset)) {
        emitLine("    lw " + dest + ", " + std::to_string(offset) + "(sp)");
        return;
    }

    // lw 的偏移同样只有 12 位。先计算完整地址再从 0 偏移读取。
    emitLine("    li t6, " + std::to_string(offset));
    emitLine("    add t6, sp, t6");
    emitLine("    lw " + dest + ", 0(t6)");
}

void RiscvGenerator::emitStoreToSp(const std::string& src, int offset) {
    if (fitsImm12(offset)) {
        emitLine("    sw " + src + ", " + std::to_string(offset) + "(sp)");
        return;
    }

    // t6 作为后端保留的地址临时寄存器。当前 IR 不把值长期保存在 t6 中。
    emitLine("    li t6, " + std::to_string(offset));
    emitLine("    add t6, sp, t6");
    emitLine("    sw " + src + ", 0(t6)");
}

void RiscvGenerator::emitInstr(const IRInstr& instr) {
    switch (instr.type) {
        case IRInstrType::LI:
            emitLine("    li " + instr.dest + ", " + instr.src1);
            break;

        case IRInstrType::LOAD: {
            int logicalOffset = std::stoi(instr.src1);
            emitLoadFromSp(instr.dest, currentLocalBase + logicalOffset);
            break;
        }

        case IRInstrType::STORE: {
            int logicalOffset = std::stoi(instr.src2);
            emitStoreToSp(instr.src1, currentLocalBase + logicalOffset);
            break;
        }

        case IRInstrType::LOAD_ARG: {
            // 第 9+ 个参数位于“调用者调用时的 sp + argOffset”。
            // 进入当前函数后 sp 已减去 currentFrameSize，因此需加回整个栈帧。
            int argOffset = std::stoi(instr.src1);
            emitLoadFromSp(instr.dest, currentFrameSize + argOffset);
            break;
        }

        case IRInstrType::STORE_ARG: {
            // outgoing 参数区从当前 sp + 0 开始，不叠加 currentLocalBase。
            int argOffset = std::stoi(instr.src2);
            emitStoreToSp(instr.src1, argOffset);
            break;
        }

        case IRInstrType::MV:
            emitLine("    mv " + instr.dest + ", " + instr.src1);
            break;
        case IRInstrType::ADD:
            emitLine("    add " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::SUB:
            emitLine("    sub " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::MUL:
            emitLine("    mul " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::DIV:
            emitLine("    div " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::REM:
            emitLine("    rem " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::SLT:
            emitLine("    slt " + instr.dest + ", " + instr.src1 + ", " + instr.src2);
            break;
        case IRInstrType::SEQZ:
            emitLine("    seqz " + instr.dest + ", " + instr.src1);
            break;
        case IRInstrType::SNEZ:
            emitLine("    snez " + instr.dest + ", " + instr.src1);
            break;
        case IRInstrType::LABEL:
            emitLine(instr.label + ":");
            break;
        case IRInstrType::JUMP:
            emitLine("    j " + instr.label);
            break;
        case IRInstrType::BRANCH_ZERO:
            emitLine("    beqz " + instr.src1 + ", " + instr.label);
            break;
        case IRInstrType::BRANCH_NONZERO:
            emitLine("    bnez " + instr.src1 + ", " + instr.label);
            break;

        case IRInstrType::LOAD_GLOBAL:
            emitLine("    la t1, " + instr.src1);
            emitLine("    lw " + instr.dest + ", 0(t1)");
            break;
        case IRInstrType::STORE_GLOBAL:
            emitLine("    la t1, " + instr.src1);
            emitLine("    sw " + instr.src2 + ", 0(t1)");
            break;
        case IRInstrType::CALL:
            emitLine("    call " + instr.src1);
            break;
        case IRInstrType::RET:
            // RET 标记本身不输出；统一尾声由 generate() 负责。
            break;
        default:
            break;
    }
}

void RiscvGenerator::generate(const IRProgram& program) {
    std::cerr << "[DEBUG] RiscvGenerator::generate called" << std::endl;

    // -------------------- 数据段：全局变量 --------------------
    if (!program.globalVars.empty()) {
        emitLine(".data");
        for (const auto& [name, value] : program.globalVars) {
            emitLine(".globl " + name);
            emitLine(name + ":");
            emitLine("    .word " + std::to_string(value));
        }
    }

    // -------------------- 代码段 --------------------
    emitLine(".section .text");
    for (const auto& func : program.functions) {
        emitLine(".globl " + func.name);
    }

    for (const auto& func : program.functions) {
        emitLine(func.name + ":");

        // 帧布局（低地址 -> 高地址）：
        // [outgoing args][locals/params/spills][padding][saved ra]
        currentLocalBase = func.outgoingArgSize;

        int frameSize = func.outgoingArgSize + func.localSize + 4;
        if (frameSize % 16 != 0) {
            frameSize += 16 - (frameSize % 16);
        }
        currentFrameSize = frameSize;

        int raOffset = frameSize - 4;

        // 序言：分配栈帧，保存 ra。辅助函数会自动处理 >12-bit 的立即数。
        emitAdjustSp(-frameSize);
        emitStoreToSp("ra", raOffset);

        for (const auto& instr : func.instrs) {
            emitInstr(instr);
        }

        // 尾声：恢复 ra，回收栈帧，返回。
        emitLoadFromSp("ra", raOffset);
        emitAdjustSp(frameSize);
        emitLine("    ret");

        currentFrameSize = 0;
        currentLocalBase = 0;
    }
}

} // namespace toycc
