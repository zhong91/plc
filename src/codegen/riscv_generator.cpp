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
            // 由外部统一生成
            break;
        default:
            break;
    }
}

void RiscvGenerator::generate(const IRProgram& program) {
    std::cerr << "[DEBUG] RiscvGenerator::generate called" << std::endl;

    if (!program.globalVars.empty()) {
        emitLine(".data");
        for (const auto& [name, value] : program.globalVars) {
            emitLine(".globl " + name);
            emitLine(name + ":");
            emitLine("    .word " + std::to_string(value));
        }
    }

    emitLine(".section .text");
    // 声明所有函数为全局，方便互相调用
    for (const auto& func : program.functions) {
        emitLine(".globl " + func.name);
    }

    for (const auto& func : program.functions) {
        emitLine(func.name + ":");
        // 栈帧大小：16 字节（保存 ra 和临时空间）
        int frameSize = 16;
        emitLine("    addi sp, sp, -" + std::to_string(frameSize));
        emitLine("    sw ra, 12(sp)");

        for (const auto& instr : func.instrs) {
            emitInstr(instr);
        }

        emitLine("    lw ra, 12(sp)");
        emitLine("    addi sp, sp, " + std::to_string(frameSize));
        // 如果最后一条已经 RET，就不再重复，但我们的 IRBuilder 已经保证最后有 RET，因此这里不用重复加 ret
        // 但是为了防止 IR 中 RET 被忽略，我们在 generate 中不自动加 ret，而是依赖于 IR 中的 RET 指令
        // 但 RET 指令在 emitInstr 中未做处理，所以我们需要在函数末尾显式加 ret
        // 但我们在 IRBuilder 中最后一条是 RET，不过 emitInstr 中 RET 不做输出，所以这里需要手动加 ret
        // 但为了不影响，我们在 emitInstr 中不处理 RET，在 generate 的最后统一加 ret
        // 但考虑到有的函数可能以 RET 结尾，我们判断最后一条是否为 RET，若是则不重复加
        // 简单起见，我们在 emitInstr 中让 RET 输出 "ret"，但为了栈帧恢复，我们需要在恢复栈帧后 ret。
        // 最稳妥：在 emitInstr 中 RET 输出 "ret"，但这样顺序是先 ret 再恢复栈帧。
        // 所以我们需要在生成完所有指令后，再统一加 ret，且将 RET 指令视为无操作。
        // 目前我们在 emitInstr 中 RET 不做任何事，然后在下面手动加 ret。
        // 因此我们直接在这里加 ret。
        emitLine("    ret");
    }
}

} // namespace toycc