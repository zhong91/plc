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
        if (optimize && !hasCall) {
            // Cache hot loop immediates in otherwise-unused caller-saved registers.
            // This removes a `li` from every loop-condition evaluation.
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
                if(v>=std::numeric_limits<int>::min()&&v<=std::numeric_limits<int>::max()&&v!=0)
                    cscore[static_cast<int>(v)] += weight[ii];
            }
            std::vector<std::pair<int,long long>> cs(cscore.begin(),cscore.end());
            std::sort(cs.begin(),cs.end(),[](auto&a,auto&b){return a.second>b.second;});
            std::unordered_map<std::string,bool> used;
            for(const auto& pr:promoted) used[pr.second]=true;
            static const char* pool[]={"t2","t3","t4","t5","a1","a2","a3","a4","a5","a6","a7"};
            size_t pi=0;
            for(const auto& [val,sc]:cs){
                if(sc<33) continue;
                while(pi<std::size(pool)&&used[pool[pi]]) ++pi;
                if(pi>=std::size(pool)) break;
                std::string reg=pool[pi++];
                bool mentioned=false;
                for(const auto& ins:func.instrs) if(ins.dest==reg||ins.src1==reg||ins.src2==reg){mentioned=true;break;}
                if(mentioned) continue;
                currentConstantRegs[val]=reg; used[reg]=true;
            }
        }

        std::vector<std::pair<int, std::string>> savedPromoted;
        savedPromoted.reserve(promoted.size());
        for (const auto& pr : promoted) {
            if (!pr.second.empty() && pr.second[0] == 's') savedPromoted.push_back(pr);
        }

        int savedBase = func.outgoingArgSize + compactLocalBytes;
        int savedBytes = static_cast<int>(savedPromoted.size()) * 4;
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
        for (size_t r = 0; r < savedPromoted.size(); ++r) {
            emitStoreToSp(savedPromoted[r].second, savedBase + static_cast<int>(r) * 4);
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
        for (size_t r = savedPromoted.size(); r > 0; --r) {
            emitLoadFromSp(savedPromoted[r - 1].second, savedBase + static_cast<int>(r - 1) * 4);
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
