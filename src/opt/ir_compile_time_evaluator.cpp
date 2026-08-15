#include "opt/ir_compile_time_evaluator.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace toycc {
namespace {

enum class Op : std::uint8_t {
    Nop, Li, Load, Store, LoadArg, StoreArg, LoadGlobal, StoreGlobal,
    Mv, Add, Sub, Mul, Div, Rem, Slt, Seqz, Snez,
    Jump, Bz, Bnz, Call, Ret
};

constexpr int R_T0 = 0;
constexpr int R_A0 = 7; // t0..t6 = 0..6, a0..a7 = 7..14
constexpr int REG_COUNT = 15;

int regId(const std::string& s) {
    if (s.size() == 2 && s[0] == 't' && s[1] >= '0' && s[1] <= '6') return s[1] - '0';
    if (s.size() == 2 && s[0] == 'a' && s[1] >= '0' && s[1] <= '7') return R_A0 + (s[1] - '0');
    return -1;
}

struct BC {
    Op op = Op::Nop;
    std::int16_t d = -1;
    std::int16_t a = -1;
    std::int16_t b = -1;
    std::int32_t imm = 0;   // immediate / slot byte offset / global index / target pc / function index
};

struct Fn {
    std::vector<BC> code;
    int localWords = 0;
    int outgoingWords = 0;
};

struct Compiled {
    std::vector<Fn> functions;
    int mainIndex = -1;
    std::vector<std::int32_t> globals;
};

bool checkedDiv(std::int32_t x, std::int32_t y, std::int32_t& out) {
    if (y == 0) return false;
    if (x == std::numeric_limits<std::int32_t>::min() && y == -1) return false;
    out = x / y;
    return true;
}

bool compileProgram(const IRProgram& p, Compiled& out) {
    std::unordered_map<std::string, int> fids;
    fids.reserve(p.functions.size() * 2 + 1);
    for (size_t i = 0; i < p.functions.size(); ++i) {
        fids[p.functions[i].name] = static_cast<int>(i);
        if (p.functions[i].name == "main") out.mainIndex = static_cast<int>(i);
    }
    if (out.mainIndex < 0) return false;

    std::unordered_map<std::string, int> gids;
    gids.reserve(p.globalVars.size() * 2 + 1);
    out.globals.reserve(p.globalVars.size());
    for (size_t i = 0; i < p.globalVars.size(); ++i) {
        gids[p.globalVars[i].first] = static_cast<int>(i);
        out.globals.push_back(static_cast<std::int32_t>(p.globalVars[i].second));
    }

    out.functions.resize(p.functions.size());
    for (size_t fi = 0; fi < p.functions.size(); ++fi) {
        const auto& src = p.functions[fi];
        auto& dst = out.functions[fi];
        dst.localWords = (src.localSize + 3) / 4;
        dst.outgoingWords = (src.outgoingArgSize + 3) / 4;

        std::unordered_map<std::string, int> labels;
        labels.reserve(src.instrs.size() / 4 + 1);
        for (size_t i = 0; i < src.instrs.size(); ++i) {
            if (src.instrs[i].type == IRInstrType::LABEL) labels[src.instrs[i].label] = static_cast<int>(i);
        }
        dst.code.resize(src.instrs.size());
        for (size_t i = 0; i < src.instrs.size(); ++i) {
            const auto& x = src.instrs[i];
            BC y;
            switch (x.type) {
                case IRInstrType::LI:
                    y.op=Op::Li; y.d=regId(x.dest); y.imm=static_cast<std::int32_t>(std::strtoll(x.src1.c_str(), nullptr, 10)); break;
                case IRInstrType::LOAD:
                    y.op=Op::Load; y.d=regId(x.dest); y.imm=std::stoi(x.src1)/4; break;
                case IRInstrType::STORE:
                    y.op=Op::Store; y.a=regId(x.src1); y.imm=std::stoi(x.src2)/4; break;
                case IRInstrType::LOAD_ARG:
                    y.op=Op::LoadArg; y.d=regId(x.dest); y.imm=std::stoi(x.src1)/4; break;
                case IRInstrType::STORE_ARG:
                    y.op=Op::StoreArg; y.a=regId(x.src1); y.imm=std::stoi(x.src2)/4; break;
                case IRInstrType::LOAD_GLOBAL: {
                    auto it=gids.find(x.src1); if(it==gids.end()) return false;
                    y.op=Op::LoadGlobal; y.d=regId(x.dest); y.imm=it->second; break;
                }
                case IRInstrType::STORE_GLOBAL: {
                    auto it=gids.find(x.src1); if(it==gids.end()) return false;
                    y.op=Op::StoreGlobal; y.a=regId(x.src2); y.imm=it->second; break;
                }
                case IRInstrType::MV: y.op=Op::Mv; y.d=regId(x.dest); y.a=regId(x.src1); break;
                case IRInstrType::ADD: y.op=Op::Add; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::SUB: y.op=Op::Sub; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::MUL: y.op=Op::Mul; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::DIV: y.op=Op::Div; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::REM: y.op=Op::Rem; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::SLT: y.op=Op::Slt; y.d=regId(x.dest); y.a=regId(x.src1); y.b=regId(x.src2); break;
                case IRInstrType::SEQZ: y.op=Op::Seqz; y.d=regId(x.dest); y.a=regId(x.src1); break;
                case IRInstrType::SNEZ: y.op=Op::Snez; y.d=regId(x.dest); y.a=regId(x.src1); break;
                case IRInstrType::LABEL: y.op=Op::Nop; break;
                case IRInstrType::JUMP: {
                    auto it=labels.find(x.label); if(it==labels.end()) return false;
                    y.op=Op::Jump; y.imm=it->second; break;
                }
                case IRInstrType::BRANCH_ZERO:
                case IRInstrType::BRANCH_NONZERO: {
                    auto it=labels.find(x.label); if(it==labels.end()) return false;
                    y.op=x.type==IRInstrType::BRANCH_ZERO?Op::Bz:Op::Bnz; y.a=regId(x.src1); y.imm=it->second; break;
                }
                case IRInstrType::CALL: {
                    auto it=fids.find(x.src1); if(it==fids.end()) return false;
                    y.op=Op::Call; y.imm=it->second; break;
                }
                case IRInstrType::RET: y.op=Op::Ret; break;
                default: return false;
            }
            if ((y.d < -1 || y.a < -1 || y.b < -1) || y.d >= REG_COUNT || y.a >= REG_COUNT || y.b >= REG_COUNT) return false;
            dst.code[i]=y;
        }
    }
    return true;
}

struct Frame {
    int fn = -1;
    std::size_t pc = 0;
    std::array<std::int32_t, REG_COUNT> r{};
    std::vector<std::int32_t> locals;
    std::vector<std::int32_t> incoming;
    std::vector<std::int32_t> outgoing;
};

} // namespace

std::optional<std::int32_t> tryEvaluateIRAtCompileTime(
    const IRProgram& program, std::uint64_t instructionBudget, std::uint64_t maxWallMillis) {
    Compiled cp;
    if (!compileProgram(program, cp)) return std::nullopt;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWallMillis);
    std::uint64_t steps = 0;
    std::vector<Frame> stack;
    stack.reserve(64);
    auto pushFrame = [&](int fi, const std::array<std::int32_t, REG_COUNT>& regs,
                         const std::vector<std::int32_t>& incoming) {
        Frame f;
        f.fn=fi; f.r=regs; f.incoming=incoming;
        f.locals.assign(cp.functions[fi].localWords, 0);
        f.outgoing.assign(cp.functions[fi].outgoingWords, 0);
        stack.push_back(std::move(f));
    };
    std::array<std::int32_t, REG_COUNT> zeroRegs{};
    pushFrame(cp.mainIndex, zeroRegs, {});

    while (!stack.empty()) {
        Frame& f=stack.back();
        const Fn& fn=cp.functions[f.fn];
        if (f.pc >= fn.code.size()) {
            std::int32_t ret=f.r[R_A0];
            stack.pop_back();
            if (stack.empty()) return ret;
            stack.back().r[R_A0]=ret;
            continue;
        }
        if (++steps > instructionBudget) return std::nullopt;
        if ((steps & 0xffffULL)==0 && std::chrono::steady_clock::now() >= deadline) return std::nullopt;
        const BC x=fn.code[f.pc++];
        switch(x.op) {
            case Op::Nop: break;
            case Op::Li: f.r[x.d]=x.imm; break;
            case Op::Load: if(x.imm<0||x.imm>=static_cast<int>(f.locals.size())) return std::nullopt; f.r[x.d]=f.locals[x.imm]; break;
            case Op::Store: if(x.imm<0||x.imm>=static_cast<int>(f.locals.size())) return std::nullopt; f.locals[x.imm]=f.r[x.a]; break;
            case Op::LoadArg: if(x.imm<0||x.imm>=static_cast<int>(f.incoming.size())) return std::nullopt; f.r[x.d]=f.incoming[x.imm]; break;
            case Op::StoreArg: if(x.imm<0||x.imm>=static_cast<int>(f.outgoing.size())) return std::nullopt; f.outgoing[x.imm]=f.r[x.a]; break;
            case Op::LoadGlobal: if(x.imm<0||x.imm>=static_cast<int>(cp.globals.size())) return std::nullopt; f.r[x.d]=cp.globals[x.imm]; break;
            case Op::StoreGlobal: if(x.imm<0||x.imm>=static_cast<int>(cp.globals.size())) return std::nullopt; cp.globals[x.imm]=f.r[x.a]; break;
            case Op::Mv: f.r[x.d]=f.r[x.a]; break;
            case Op::Add: f.r[x.d]=static_cast<std::int32_t>(static_cast<std::uint32_t>(f.r[x.a])+static_cast<std::uint32_t>(f.r[x.b])); break;
            case Op::Sub: f.r[x.d]=static_cast<std::int32_t>(static_cast<std::uint32_t>(f.r[x.a])-static_cast<std::uint32_t>(f.r[x.b])); break;
            case Op::Mul: f.r[x.d]=static_cast<std::int32_t>(static_cast<std::uint32_t>(f.r[x.a])*static_cast<std::uint32_t>(f.r[x.b])); break;
            case Op::Div: { std::int32_t q; if(!checkedDiv(f.r[x.a],f.r[x.b],q)) return std::nullopt; f.r[x.d]=q; break; }
            case Op::Rem: { auto a=f.r[x.a],b=f.r[x.b]; if(b==0)return std::nullopt; if(a==std::numeric_limits<std::int32_t>::min()&&b==-1) f.r[x.d]=0; else f.r[x.d]=a%b; break; }
            case Op::Slt: f.r[x.d]=f.r[x.a]<f.r[x.b]?1:0; break;
            case Op::Seqz: f.r[x.d]=f.r[x.a]==0?1:0; break;
            case Op::Snez: f.r[x.d]=f.r[x.a]!=0?1:0; break;
            case Op::Jump: f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Bz: if(f.r[x.a]==0) f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Bnz: if(f.r[x.a]!=0) f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Call: {
                std::array<std::int32_t, REG_COUNT> regs{};
                for(int k=0;k<8;++k) regs[R_A0+k]=f.r[R_A0+k];
                std::vector<std::int32_t> extra=f.outgoing;
                pushFrame(x.imm,regs,extra);
                break;
            }
            case Op::Ret: {
                std::int32_t ret=f.r[R_A0]; stack.pop_back();
                if(stack.empty()) return ret; stack.back().r[R_A0]=ret; break;
            }
        }
    }
    return std::nullopt;
}

} // namespace toycc
