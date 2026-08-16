#include "codegen/riscv_generator.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace toycc {
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

        // Compact all stack-resident logical slots. IRBuilder intentionally never
        // reuses logical offsets, which is convenient for correctness but can make
        // frames huge after optimization removes/promotes most of them. Physical
        // codegen only needs slots that still occur in LOAD/STORE instructions.
        currentPhysicalSlots.clear();
        int compactLocalBytes = 0;
        if (optimize) {
            std::vector<int> usedSlots;
            for (const auto& ins : func.instrs) {
                int off = -1;
                if (ins.type == IRInstrType::LOAD) off = std::stoi(ins.src1);
                else if (ins.type == IRInstrType::STORE) off = std::stoi(ins.src2);
                if (off >= 0 && !currentPromotedSlots.count(off)) usedSlots.push_back(off);
            }
            std::sort(usedSlots.begin(), usedSlots.end());
            usedSlots.erase(std::unique(usedSlots.begin(), usedSlots.end()), usedSlots.end());
            for (int off : usedSlots) {
                currentPhysicalSlots[off] = compactLocalBytes;
                compactLocalBytes += 4;
            }
        } else {
            compactLocalBytes = func.localSize;
        }

        bool hasCall = false;
        for (const auto& ins : func.instrs) {
            if (ins.type == IRInstrType::CALL) { hasCall = true; break; }
        }

        currentConstantRegs.clear();
        if (optimize) {
            // Cache hot loop immediates.  Leaf functions can use otherwise-free
            // caller-saved registers at zero save/restore cost.  When those are
            // exhausted (or the function still contains calls), use any unused
            // s-register: two prologue/epilogue memory operations are easily
            // amortized by a constant loaded millions of times in a hot loop.
            std::unordered_map<std::string, size_t> labels;
            for (size_t ii=0; ii<func.instrs.size(); ++ii)
                if (func.instrs[ii].type==IRInstrType::LABEL) labels[func.instrs[ii].label]=ii;
            std::vector<int> weight(func.instrs.size(),1);
            for (size_t ii=0; ii<func.instrs.size(); ++ii) {
                const auto& ins=func.instrs[ii]; if(ins.type!=IRInstrType::JUMP) continue;
                auto it=labels.find(ins.label); if(it==labels.end()||it->second>=ii) continue;
                for(size_t j=it->second;j<=ii;++j) weight[j]+=32;
            }
            std::unordered_map<int,long long> cscore;
            for(size_t ii=0;ii<func.instrs.size();++ii) if(func.instrs[ii].type==IRInstrType::LI) {
                long long v=std::stoll(func.instrs[ii].src1);
                if(v>=std::numeric_limits<int>::min()&&v<=std::numeric_limits<int>::max()&&v!=0&&v!=1&&v!=-1) {
                    long long bonus = (v < -2048 || v > 2047) ? 4 : 1;
                    cscore[static_cast<int>(v)] += static_cast<long long>(weight[ii]) * bonus;
                }
            }
            std::vector<std::pair<int,long long>> cs(cscore.begin(),cscore.end());
            std::sort(cs.begin(),cs.end(),[](auto&a,auto&b){
                if(a.second!=b.second)return a.second>b.second;
                return a.first<b.first;
            });

            std::unordered_map<std::string,bool> used;
            for(const auto& pr:promoted) used[pr.second]=true;
            auto mentionedInIR = [&](const std::string& reg) {
                for(const auto& ins:func.instrs)
                    if(ins.dest==reg||ins.src1==reg||ins.src2==reg) return true;
                return false;
            };

            std::vector<std::string> freeCaller;
            if(!hasCall) {
                static const char* pool[]={"t2","t3","t4","t5","a1","a2","a3","a4","a5","a6","a7"};
                for(const char* p:pool) {
                    std::string reg=p;
                    if(!used[reg]&&!mentionedInIR(reg)) freeCaller.push_back(reg);
                }
            }
            std::vector<std::string> freeSaved;
            static const char* savedPool[]={"s0","s1","s2","s3","s4","s5","s6","s7","s8","s9","s10","s11"};
            for(const char* p:savedPool) {
                std::string reg=p;
                if(!used[reg]) freeSaved.push_back(reg);
            }

            size_t ci=0, si=0;
            for(const auto& [val,sc]:cs) {
                if(sc<33) continue; // at least one use in a loop
                std::string reg;
                if(ci<freeCaller.size()) reg=freeCaller[ci++];
                else if(si<freeSaved.size()) reg=freeSaved[si++];
                else break;
                currentConstantRegs[val]=reg;
                used[reg]=true;
            }
        }

        // Save every s-register owned either by a promoted slot or by a cached
        // loop constant.  Caller-saved cached constants need no frame traffic.
        std::vector<std::string> savedRegs;
        auto addSaved = [&](const std::string& reg) {
            if (reg.empty() || reg[0] != 's') return;
            if (std::find(savedRegs.begin(), savedRegs.end(), reg) == savedRegs.end())
                savedRegs.push_back(reg);
        };
        for (const auto& pr : promoted) addSaved(pr.second);
        for (const auto& [value, reg] : currentConstantRegs) addSaved(reg);

        int savedBase = func.outgoingArgSize + compactLocalBytes;
        int savedBytes = static_cast<int>(savedRegs.size()) * 4;
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
        for (size_t r = 0; r < savedRegs.size(); ++r) {
            emitStoreToSp(savedRegs[r], savedBase + static_cast<int>(r) * 4);
        }
        if (hasCall) emitStoreToSp("ra", raOffset);
        for (const auto& [value, reg] : currentConstantRegs) {
            emitLine("    li " + reg + ", " + std::to_string(value));
        }

        for (size_t i = 0; i < func.instrs.size(); ++i) {
            if (optimize && tryEmitOptimizedCompareBranch(func.instrs, i)) continue;
            if (optimize && tryEmitCompareBranch(func.instrs, i)) continue;
            if (optimize && tryEmitOptimizedBinaryUpdate(func.instrs, i)) continue;
            if (optimize && tryEmitDirectBinaryUpdate(func.instrs, i)) continue;
            if (optimize && tryEmitMulAccumulateUpdate(func.instrs, i)) continue;
            if (optimize && tryEmitDirectValueBranch(func.instrs, i)) continue;
            if (optimize && tryEmitImmediateBinaryValue(func.instrs, i)) continue;
            if (optimize && tryEmitRegisterBinaryValue(func.instrs, i)) continue;
            if (optimize && tryEmitPromotedOperandOp(func.instrs, i)) continue;
            if (optimize && tryEmitSpillPeephole(func.instrs, i)) continue;
            if (optimize && tryEmitSimplePair(func.instrs, i)) continue;
            if (optimize && func.instrs[i].type == IRInstrType::JUMP &&
                i + 1 < func.instrs.size() &&
                func.instrs[i + 1].type == IRInstrType::LABEL &&
                func.instrs[i].label == func.instrs[i + 1].label) {
                continue;
            }
            emitInstr(func.instrs[i]);
        }

        if (hasCall) emitLoadFromSp("ra", raOffset);
        for (size_t r = savedRegs.size(); r > 0; --r) {
            emitLoadFromSp(savedRegs[r - 1], savedBase + static_cast<int>(r - 1) * 4);
        }
        emitAdjustSp(frameSize);
        emitLine("    ret");

        currentPromotedSlots.clear();
        currentPhysicalSlots.clear();
        currentConstantRegs.clear();
        currentFrameSize = 0;
        currentLocalBase = 0;
    }
}

} // namespace toycc
