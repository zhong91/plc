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
            // RET 指令本身不输出，在 generate() 中统一添加 ret 以配合栈帧恢复
            break;
        default:
            break;
    }
}

void RiscvGenerator::generate(const IRProgram& program) {
    std::cerr << "[DEBUG] RiscvGenerator::generate called" << std::endl;

    // 数据段：全局变量
    if (!program.globalVars.empty()) {
        emitLine(".data");
        for (const auto& [name, value] : program.globalVars) {
            emitLine(".globl " + name);
            emitLine(name + ":");
            emitLine("    .word " + std::to_string(value));
        }
    }

    // 代码段
    emitLine(".section .text");
    // 所有函数对外可见，支持互相调用
    for (const auto& func : program.functions) {
        emitLine(".globl " + func.name);
    }

    for (const auto& func : program.functions) {
        emitLine(func.name + ":");
        // 分配栈帧（16 字节：保存 ra + 对齐）
        int frameSize = 16;
        emitLine("    addi sp, sp, -" + std::to_string(frameSize));
        emitLine("    sw ra, 12(sp)");

        for (const auto& instr : func.instrs) {
            emitInstr(instr);
        }

        // 恢复栈帧并返回
        emitLine("    lw ra, 12(sp)");
        emitLine("    addi sp, sp, " + std::to_string(frameSize));
        // IR 中的 RET 不生成指令，此处统一添加 ret，确保在栈恢复之后返回
        emitLine("    ret");
    }
}

} // namespace toycc