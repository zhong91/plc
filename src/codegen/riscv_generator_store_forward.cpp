#include "codegen/riscv_generator.h"

#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace toycc {
namespace {

bool definesT0(const IRInstr& x) { return x.dest == "t0"; }
bool usesT0(const IRInstr& x) { return x.src1 == "t0" || x.src2 == "t0"; }

bool extraLoadReachable(const std::vector<IRInstr>& v,const std::string& slot,size_t immediateLoad) {
    std::unordered_map<std::string,size_t> labels;
    for(size_t k=0;k<v.size();++k) if(v[k].type==IRInstrType::LABEL) labels[v[k].label]=k;
    std::vector<unsigned char> seen(v.size(),0); std::vector<size_t> work;
    if(immediateLoad+1<v.size()) work.push_back(immediateLoad+1);
    while(!work.empty()) {
        size_t j=work.back();work.pop_back(); if(j>=v.size()||seen[j]) continue; seen[j]=1;
        const auto& x=v[j];
        if(x.type==IRInstrType::LOAD&&x.src1==slot) return true;
        if(x.type==IRInstrType::STORE&&x.src2==slot) continue;
        if(x.type==IRInstrType::RET) continue;
        if(x.type==IRInstrType::JUMP){auto it=labels.find(x.label);if(it!=labels.end())work.push_back(it->second);continue;}
        if(x.type==IRInstrType::BRANCH_ZERO||x.type==IRInstrType::BRANCH_NONZERO){auto it=labels.find(x.label);if(it!=labels.end())work.push_back(it->second);if(j+1<v.size())work.push_back(j+1);continue;}
        if(j+1<v.size())work.push_back(j+1);
    }
    return false;
}

} // namespace

// Forward the final value of a spill-chain directly into a promoted destination:
//   STORE t0,tmp ; RHS ; LOAD t1,tmp ; OP t0,t1,t0 ; STORE t0,dst
// The v11 backend already elides the tmp store/reload; this variant also avoids
// the final `mv dstReg,t0` when t0 is immediately redefined afterwards.
bool RiscvGenerator::tryEmitStoreForwarding(const std::vector<IRInstr>& v, size_t& i) {
    if(i+5<v.size()) {
        const auto& st=v[i];const auto& rhs=v[i+1];const auto& ld=v[i+2];const auto& op=v[i+3];const auto& fin=v[i+4];const auto& next=v[i+5];
        if(st.type==IRInstrType::STORE&&st.src1=="t0"&&ld.type==IRInstrType::LOAD&&ld.dest=="t1"&&ld.src1==st.src2&&
           isCoreBinaryOp(op.type)&&op.dest=="t0"&&fin.type==IRInstrType::STORE&&fin.src1=="t0"&&
           definesT0(next)&&!usesT0(next)&&!extraLoadReachable(v,st.src2,i+2)) {
            const bool oldLeft=op.src1=="t1"&&op.src2=="t0";
            const bool rhsLeft=op.src1=="t0"&&op.src2=="t1";
            if(oldLeft||rhsLeft) {
                const std::string dst=promotedRegForSlot(std::stoi(fin.src2));
                if(!dst.empty()) {
                    if(rhs.type==IRInstrType::LI&&rhs.dest=="t0") {
                        const long long imm=std::stoll(rhs.src1);
                        std::string cr;
                        if(imm>=std::numeric_limits<int>::min()&&imm<=std::numeric_limits<int>::max()) {
                            auto it=currentConstantRegs.find(static_cast<int>(imm));if(it!=currentConstantRegs.end())cr=it->second;
                        }
                        switch(op.type) {
                            case IRInstrType::ADD:
                                if(imm>=-2048&&imm<=2047) emitLine("    addi "+dst+", t0, "+std::to_string(imm));
                                else if(!cr.empty()) emitLine("    add "+dst+", t0, "+cr);
                                else {emitLine("    li t1, "+std::to_string(imm));emitLine("    add "+dst+", t0, t1");}
                                i+=4;return true;
                            case IRInstrType::SUB:
                                if(oldLeft){long long neg=-imm;if(neg>=-2048&&neg<=2047)emitLine("    addi "+dst+", t0, "+std::to_string(neg));else {std::string r=cr;if(r.empty()){emitLine("    li t1, "+std::to_string(imm));r="t1";}emitLine("    sub "+dst+", t0, "+r);}}
                                else {std::string r=cr;if(r.empty()){emitLine("    li t1, "+std::to_string(imm));r="t1";}emitLine("    sub "+dst+", "+r+", t0");}
                                i+=4;return true;
                            case IRInstrType::MUL:
                                if(imm==0)emitLine("    li "+dst+", 0");
                                else if(imm==1){if(dst!="t0")emitLine("    mv "+dst+", t0");}
                                else if(imm==-1)emitLine("    neg "+dst+", t0");
                                else if(imm>0&&(imm&(imm-1))==0){int sh=0;for(long long z=imm;z>1;z>>=1)++sh;emitLine("    slli "+dst+", t0, "+std::to_string(sh));}
                                else {std::string r=cr;if(r.empty()){emitLine("    li t1, "+std::to_string(imm));r="t1";}emitLine("    mul "+dst+", t0, "+r);}
                                i+=4;return true;
                            case IRInstrType::DIV:
                                if(oldLeft&&imm!=0&&emitSignedDivConst(dst,"t0",static_cast<std::int32_t>(imm))){i+=4;return true;}break;
                            case IRInstrType::REM:
                                if(oldLeft&&imm!=0&&emitSignedRemConst(dst,"t0",static_cast<std::int32_t>(imm))){i+=4;return true;}break;
                            case IRInstrType::SLT: {
                                if(oldLeft&&imm>=-2048&&imm<=2047)emitLine("    slti "+dst+", t0, "+std::to_string(imm));
                                else {std::string r=cr;if(r.empty()){emitLine("    li t1, "+std::to_string(imm));r="t1";}if(oldLeft)emitLine("    slt "+dst+", t0, "+r);else emitLine("    slt "+dst+", "+r+", t0");}
                                i+=4;return true;
                            }
                            default: break;
                        }
                    }
                    if(rhs.type==IRInstrType::LOAD&&rhs.dest=="t0") {
                        std::string rr=promotedRegForSlot(std::stoi(rhs.src1));
                        if(rr.empty()) {emitLoadFromSp("t1",physicalSlotOffset(std::stoi(rhs.src1)));rr="t1";}
                        const char* m=nullptr;switch(op.type){case IRInstrType::ADD:m="add";break;case IRInstrType::SUB:m="sub";break;case IRInstrType::MUL:m="mul";break;case IRInstrType::DIV:m="div";break;case IRInstrType::REM:m="rem";break;case IRInstrType::SLT:m="slt";break;default:break;}
                        if(m){if(oldLeft)emitLine("    "+std::string(m)+" "+dst+", t0, "+rr);else emitLine("    "+std::string(m)+" "+dst+", "+rr+", t0");i+=4;return true;}
                    }
                }
            }
        }
    }

    // Simple OP t0,... ; STORE t0,dst form.
    if(i+2>=v.size()) return false;
    const auto& op=v[i];const auto& st=v[i+1];const auto& next=v[i+2];
    if(st.type!=IRInstrType::STORE||st.src1!="t0"||op.dest!="t0"||!definesT0(next)||usesT0(next)) return false;
    const std::string dst=promotedRegForSlot(std::stoi(st.src2));if(dst.empty())return false;
    const char* m=nullptr;switch(op.type){case IRInstrType::ADD:m="add";break;case IRInstrType::SUB:m="sub";break;case IRInstrType::MUL:m="mul";break;case IRInstrType::DIV:m="div";break;case IRInstrType::REM:m="rem";break;case IRInstrType::SLT:m="slt";break;default:break;}
    if(m){emitLine("    "+std::string(m)+" "+dst+", "+op.src1+", "+op.src2);i+=1;return true;}
    if(op.type==IRInstrType::SEQZ){emitLine("    seqz "+dst+", "+op.src1);i+=1;return true;}
    if(op.type==IRInstrType::SNEZ){emitLine("    snez "+dst+", "+op.src1);i+=1;return true;}
    return false;
}

} // namespace toycc
