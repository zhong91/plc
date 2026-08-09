#include "codegen/riscv_generator.h"
#include <iostream>
#include <string>

namespace toycc {

void RiscvGenerator::emitInstr(const IRInstr& instr) {
    switch (instr.type) {
        case IRInstrType::LI:     emitLine("    li " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::LOAD:   emitLine("    lw " + instr.dest + ", " + instr.src1 + "(sp)"); break;
        case IRInstrType::STORE:  emitLine("    sw " + instr.src1 + ", " + instr.src2 + "(sp)"); break;
        case IRInstrType::MV:     emitLine("    mv " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::ADD:    emitLine("    add " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SUB:    emitLine("    sub " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::MUL:    emitLine("    mul " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::DIV:    emitLine("    div " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::REM:    emitLine("    rem " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SLT:    emitLine("    slt " + instr.dest + ", " + instr.src1 + ", " + instr.src2); break;
        case IRInstrType::SEQZ:   emitLine("    seqz " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::SNEZ:   emitLine("    snez " + instr.dest + ", " + instr.src1); break;
        case IRInstrType::LABEL:  emitLine(instr.label + ":"); break;
        case IRInstrType::JUMP:   emitLine("    j " + instr.label); break;
        case IRInstrType::BRANCH_ZERO: emitLine("    beqz " + instr.src1 + ", " + instr.label); break;
        case IRInstrType::BRANCH_NONZERO: emitLine("    bnez " + instr.src1 + ", " + instr.label); break;
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
            // RET 标记本身不输出指令；ret 统一由 generate() 在函数末尾、
            // 栈帧恢复之后发出。这样可以避免一条函数里有多条 ret 指令时，
            // 每条都得写一次栈帧恢复代码的麻烦。
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

        // 帧大小 = 对齐到16字节的(局部区大小 + 4字节存ra)
        int frameSize = func.localSize + 4;
        if (frameSize % 16 != 0) {
            frameSize += 16 - (frameSize % 16);
        }
        int raOffset = frameSize - 4;   // ra 保存在帧顶部

        // 序言：分配栈帧，保存 ra
        emitLine("    addi sp, sp, -" + std::to_string(frameSize));
        emitLine("    sw ra, " + std::to_string(raOffset) + "(sp)");

        // 函数体指令
        for (const auto& instr : func.instrs) {
            emitInstr(instr);
        }

        // 尾声：恢复 ra，回收栈帧，返回
        emitLine("    lw ra, " + std::to_string(raOffset) + "(sp)");
        emitLine("    addi sp, sp, " + std::to_string(frameSize));
        emitLine("    ret");
    }
}

} // namespace toycc
