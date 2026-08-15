#include "opt/ir_optimizer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toycc {
namespace {

enum class ValueKind { Unknown, Constant, SlotValue, Expression };

struct Value {
    ValueKind kind = ValueKind::Unknown;
    int32_t constant = 0;
    int slot = -1;
    unsigned version = 0;
    std::string exprKey;

    static Value unknown() { return {}; }
    static Value constantValue(int32_t v) {
        Value x;
        x.kind = ValueKind::Constant;
        x.constant = v;
        return x;
    }
    static Value slotValue(int off, unsigned ver) {
        Value x;
        x.kind = ValueKind::SlotValue;
        x.slot = off;
        x.version = ver;
        return x;
    }
    static Value expression(std::string key) {
        Value x;
        x.kind = ValueKind::Expression;
        x.exprKey = std::move(key);
        return x;
    }
};

bool isKnown(const Value& v) {
    return v.kind != ValueKind::Unknown;
}

std::string valueKey(const Value& v) {
    switch (v.kind) {
        case ValueKind::Constant:
            return "C:" + std::to_string(v.constant);
        case ValueKind::SlotValue:
            return "S:" + std::to_string(v.slot) + "#" + std::to_string(v.version);
        case ValueKind::Expression:
            return "E:" + v.exprKey;
        default:
            return {};
    }
}

bool isCommutative(IRInstrType type) {
    return type == IRInstrType::ADD || type == IRInstrType::MUL;
}

bool parseSlot(const std::string& s, int& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0') return false;
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) return false;
    out = static_cast<int>(v);
    return true;
}

std::optional<int32_t> foldBinary(IRInstrType type, int32_t a, int32_t b) {
    const int64_t aa = a;
    const int64_t bb = b;
    switch (type) {
        case IRInstrType::ADD:
            return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
        case IRInstrType::SUB:
            return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
        case IRInstrType::MUL:
            return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
        case IRInstrType::DIV:
            if (b == 0) return std::nullopt;
            if (a == std::numeric_limits<int32_t>::min() && b == -1) return std::nullopt;
            return static_cast<int32_t>(aa / bb);
        case IRInstrType::REM:
            if (b == 0) return std::nullopt;
            if (a == std::numeric_limits<int32_t>::min() && b == -1) return int32_t{0};
            return static_cast<int32_t>(aa % bb);
        case IRInstrType::SLT:
            return static_cast<int32_t>(a < b ? 1 : 0);
        default:
            return std::nullopt;
    }
}

struct ExprLocation {
    int slot = -1;
    unsigned version = 0;
};

struct LocalState {
    std::unordered_map<std::string, Value> regs;
    std::unordered_map<int, Value> slots;
    std::unordered_map<int, unsigned> slotVersions;
    std::unordered_map<std::string, ExprLocation> exprLocations;

    Value reg(const std::string& name) const {
        auto it = regs.find(name);
        return it == regs.end() ? Value::unknown() : it->second;
    }

    unsigned versionOf(int slot) const {
        auto it = slotVersions.find(slot);
        return it == slotVersions.end() ? 0u : it->second;
    }

    void setReg(const std::string& name, const Value& v) {
        if (!name.empty()) regs[name] = v;
    }

    void killReg(const std::string& name) {
        if (!name.empty()) regs.erase(name);
    }

    void clearAll() {
        regs.clear();
        slots.clear();
        slotVersions.clear();
        exprLocations.clear();
    }
};

bool aliasStillValid(const Value& v, const LocalState& st) {
    return v.kind == ValueKind::SlotValue && st.versionOf(v.slot) == v.version;
}

std::string makeExprKey(IRInstrType type, const Value& lhs, const Value& rhs) {
    std::string a = valueKey(lhs);
    std::string b = valueKey(rhs);
    if (a.empty() || b.empty()) return {};
    if (isCommutative(type) && b < a) std::swap(a, b);
    return std::to_string(static_cast<int>(type)) + "(" + a + "," + b + ")";
}

void invalidateCallerSaved(LocalState& st) {
    static const char* regs[] = {
        "t0", "t1", "t2", "t3", "t4", "t5", "t6",
        "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"
    };
    for (const char* r : regs) st.killReg(r);
}

// Basic-block-local constant/copy propagation + local value numbering.
bool simplifyLocally(IRFunction& func) {
    bool changed = false;
    LocalState st;
    std::vector<IRInstr> out;
    out.reserve(func.instrs.size());

    auto emit = [&](IRInstr ins) { out.push_back(std::move(ins)); };

    for (const auto& original : func.instrs) {
        IRInstr ins = original;

        // Labels are potential join points, so do not propagate values through them.
        if (ins.type == IRInstrType::LABEL) {
            st.clearAll();
            emit(ins);
            continue;
        }

        switch (ins.type) {
            case IRInstrType::LI: {
                int64_t raw = std::strtoll(ins.src1.c_str(), nullptr, 10);
                int32_t v = static_cast<int32_t>(raw);
                st.setReg(ins.dest, Value::constantValue(v));
                emit(ins);
                break;
            }

            case IRInstrType::MV: {
                if (ins.dest == ins.src1) {
                    changed = true;
                    break;
                }
                Value v = st.reg(ins.src1);
                st.setReg(ins.dest, v);
                if (v.kind == ValueKind::Constant) {
                    ins.type = IRInstrType::LI;
                    ins.src1 = std::to_string(v.constant);
                    ins.src2.clear();
                    changed = true;
                } else if (v.kind == ValueKind::SlotValue && aliasStillValid(v, st)) {
                    // Canonicalize copies of a freshly loaded slot back to LOAD.
                    // This exposes LOAD/LOAD/OP/STORE patterns to the optimized
                    // backend instead of leaving an opaque `mv t0,t1` barrier.
                    ins.type = IRInstrType::LOAD;
                    ins.src1 = std::to_string(v.slot);
                    ins.src2.clear();
                    changed = true;
                }
                emit(ins);
                break;
            }

            case IRInstrType::LOAD: {
                int slot = 0;
                if (!parseSlot(ins.src1, slot)) {
                    st.killReg(ins.dest);
                    emit(ins);
                    break;
                }
                Value v;
                auto it = st.slots.find(slot);
                if (it != st.slots.end()) v = it->second;
                else v = Value::slotValue(slot, st.versionOf(slot));

                if (v.kind == ValueKind::Constant) {
                    ins.type = IRInstrType::LI;
                    ins.src1 = std::to_string(v.constant);
                    ins.src2.clear();
                    st.setReg(ins.dest, v);
                    changed = true;
                } else if (aliasStillValid(v, st) && v.slot != slot) {
                    ins.src1 = std::to_string(v.slot);
                    st.setReg(ins.dest, v);
                    changed = true;
                } else {
                    // If an alias has become stale, the value is still safely available in this slot.
                    if (v.kind == ValueKind::SlotValue && !aliasStillValid(v, st)) {
                        v = Value::slotValue(slot, st.versionOf(slot));
                        st.slots[slot] = v;
                    }
                    st.setReg(ins.dest, v);
                }
                emit(ins);
                break;
            }

            case IRInstrType::STORE: {
                int slot = 0;
                if (!parseSlot(ins.src2, slot)) {
                    emit(ins);
                    break;
                }
                Value v = st.reg(ins.src1);
                unsigned ver = ++st.slotVersions[slot];
                st.slots[slot] = isKnown(v) ? v : Value::unknown();
                if (v.kind == ValueKind::Expression) {
                    st.exprLocations[v.exprKey] = {slot, ver};
                }
                emit(ins);
                break;
            }

            case IRInstrType::LOAD_ARG:
            case IRInstrType::LOAD_GLOBAL:
                st.killReg(ins.dest);
                emit(ins);
                break;

            case IRInstrType::STORE_ARG:
            case IRInstrType::STORE_GLOBAL:
                emit(ins);
                break;

            case IRInstrType::SEQZ:
            case IRInstrType::SNEZ: {
                Value a = st.reg(ins.src1);
                if (a.kind == ValueKind::Constant) {
                    int32_t result = ins.type == IRInstrType::SEQZ
                        ? (a.constant == 0 ? 1 : 0)
                        : (a.constant != 0 ? 1 : 0);
                    ins.type = IRInstrType::LI;
                    ins.src1 = std::to_string(result);
                    ins.src2.clear();
                    st.setReg(ins.dest, Value::constantValue(result));
                    changed = true;
                } else {
                    std::string aKey = valueKey(a);
                    if (aKey.empty()) st.killReg(ins.dest);
                    else st.setReg(ins.dest, Value::expression(
                        std::to_string(static_cast<int>(ins.type)) + "(" + aKey + ")"));
                }
                emit(ins);
                break;
            }

            case IRInstrType::ADD:
            case IRInstrType::SUB:
            case IRInstrType::MUL:
            case IRInstrType::DIV:
            case IRInstrType::REM:
            case IRInstrType::SLT: {
                Value a = st.reg(ins.src1);
                Value b = st.reg(ins.src2);

                if (a.kind == ValueKind::Constant && b.kind == ValueKind::Constant) {
                    auto folded = foldBinary(ins.type, a.constant, b.constant);
                    if (folded.has_value()) {
                        ins.type = IRInstrType::LI;
                        ins.src1 = std::to_string(*folded);
                        ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(*folded));
                        changed = true;
                        emit(ins);
                        break;
                    }
                }

                // Algebraic identities. Operands have already been evaluated in the IR,
                // so replacing only the arithmetic instruction cannot remove side effects.
                bool replaced = false;
                if (ins.type == IRInstrType::ADD) {
                    if (b.kind == ValueKind::Constant && b.constant == 0) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src1; ins.src2.clear();
                        st.setReg(ins.dest, a); replaced = true;
                    } else if (a.kind == ValueKind::Constant && a.constant == 0) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src2; ins.src2.clear();
                        st.setReg(ins.dest, b); replaced = true;
                    }
                } else if (ins.type == IRInstrType::SUB) {
                    if (b.kind == ValueKind::Constant && b.constant == 0) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src1; ins.src2.clear();
                        st.setReg(ins.dest, a); replaced = true;
                    } else if (original.src1 == original.src2) {
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    }
                } else if (ins.type == IRInstrType::MUL) {
                    if ((a.kind == ValueKind::Constant && a.constant == 0) ||
                        (b.kind == ValueKind::Constant && b.constant == 0)) {
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    } else if (b.kind == ValueKind::Constant && b.constant == 1) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src1; ins.src2.clear();
                        st.setReg(ins.dest, a); replaced = true;
                    } else if (a.kind == ValueKind::Constant && a.constant == 1) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src2; ins.src2.clear();
                        st.setReg(ins.dest, b); replaced = true;
                    }
                } else if (ins.type == IRInstrType::DIV) {
                    if (b.kind == ValueKind::Constant && b.constant == 1) {
                        ins.type = IRInstrType::MV; ins.src1 = original.src1; ins.src2.clear();
                        st.setReg(ins.dest, a); replaced = true;
                    }
                } else if (ins.type == IRInstrType::REM) {
                    if (b.kind == ValueKind::Constant && (b.constant == 1 || b.constant == -1)) {
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    }
                }

                if (replaced) {
                    changed = true;
                    emit(ins);
                    break;
                }

                // Conservative local CSE.  Re-enable only the high-value, easy-to-prove
                // cases: both operands must be constants or direct values of unchanged
                // local stack slots, and the operation must be a simple add/sub/mul/slt.
                // In particular, do not CSE nested Expression values, DIV/REM, or values
                // whose provenance cannot be tied to a current slot version.
                // v6: CSE only arithmetic values.  Keep comparison results out of
                // the value-numbering cache; normalized relational expressions are
                // cheap, branch-heavy, and much more sensitive to control-flow shape.
                const bool cseOp = ins.type == IRInstrType::ADD ||
                                   ins.type == IRInstrType::SUB ||
                                   ins.type == IRInstrType::MUL ||
                                   ins.type == IRInstrType::DIV ||
                                   ins.type == IRInstrType::REM;
                const auto atomicForCse = [&](const Value& v) {
                    if (v.kind == ValueKind::Constant) return true;
                    return v.kind == ValueKind::SlotValue && aliasStillValid(v, st);
                };
                const auto stableForCse = [&](const Value& v) {
                    return atomicForCse(v) || v.kind == ValueKind::Expression;
                };

                std::string key;
                const bool expensive = ins.type==IRInstrType::MUL || ins.type==IRInstrType::DIV || ins.type==IRInstrType::REM;
                if (cseOp && (expensive ? (stableForCse(a)&&stableForCse(b))
                                       : (atomicForCse(a)&&atomicForCse(b)))) {
                    key = makeExprKey(ins.type, a, b);
                }
                if (!key.empty()) {
                    auto loc = st.exprLocations.find(key);
                    if (loc != st.exprLocations.end() &&
                        st.versionOf(loc->second.slot) == loc->second.version) {
                        ins.type = IRInstrType::LOAD;
                        ins.src1 = std::to_string(loc->second.slot);
                        ins.src2.clear();
                        st.setReg(ins.dest, Value::expression(key));
                        changed = true;
                        emit(ins);
                        break;
                    }
                    st.setReg(ins.dest, Value::expression(key));
                } else {
                    st.killReg(ins.dest);
                }
                emit(ins);
                break;
            }

            case IRInstrType::BRANCH_ZERO:
            case IRInstrType::BRANCH_NONZERO: {
                Value cond = st.reg(ins.src1);
                if (cond.kind == ValueKind::Constant) {
                    bool take = ins.type == IRInstrType::BRANCH_ZERO ? cond.constant == 0 : cond.constant != 0;
                    changed = true;
                    if (take) {
                        ins.type = IRInstrType::JUMP;
                        ins.dest.clear(); ins.src1.clear(); ins.src2.clear();
                        emit(ins);
                    }
                    // If not taken, drop the branch completely.
                } else {
                    emit(ins);
                }
                st.clearAll();
                break;
            }

            case IRInstrType::JUMP:
                emit(ins);
                st.clearAll();
                break;

            case IRInstrType::CALL:
                emit(ins);
                invalidateCallerSaved(st);
                // Calls cannot modify this function's local stack slots, so slot facts survive.
                break;

            case IRInstrType::RET:
                emit(ins);
                st.clearAll();
                break;

            default:
                emit(ins);
                break;
        }
    }

    if (changed) func.instrs.swap(out);
    return changed;
}


struct ConstFlowState {
    std::unordered_map<int, int32_t> slots;
    std::unordered_map<std::string, int32_t> regs;
};

bool sameConstState(const ConstFlowState& a, const ConstFlowState& b) {
    return a.slots == b.slots && a.regs == b.regs;
}

ConstFlowState meetConstStates(const std::vector<const ConstFlowState*>& states) {
    ConstFlowState out;
    if (states.empty()) return out;
    out = *states.front();
    for (size_t i = 1; i < states.size(); ++i) {
        for (auto it = out.slots.begin(); it != out.slots.end();) {
            auto j = states[i]->slots.find(it->first);
            if (j == states[i]->slots.end() || j->second != it->second) it = out.slots.erase(it);
            else ++it;
        }
        for (auto it = out.regs.begin(); it != out.regs.end();) {
            auto j = states[i]->regs.find(it->first);
            if (j == states[i]->regs.end() || j->second != it->second) it = out.regs.erase(it);
            else ++it;
        }
    }
    return out;
}

void transferConstInstr(const IRInstr& ins, ConstFlowState& st) {
    auto regConst = [&](const std::string& r) -> std::optional<int32_t> {
        auto it = st.regs.find(r); return it == st.regs.end() ? std::nullopt : std::optional<int32_t>(it->second);
    };
    auto setReg = [&](const std::string& r, std::optional<int32_t> v) {
        if (r.empty()) return;
        if (v) st.regs[r] = *v; else st.regs.erase(r);
    };
    switch (ins.type) {
        case IRInstrType::LI:
            setReg(ins.dest, static_cast<int32_t>(std::strtoll(ins.src1.c_str(), nullptr, 10))); break;
        case IRInstrType::LOAD: {
            int slot=0; if (!parseSlot(ins.src1,slot)) { setReg(ins.dest,std::nullopt); break; }
            auto it=st.slots.find(slot); setReg(ins.dest,it==st.slots.end()?std::nullopt:std::optional<int32_t>(it->second)); break;
        }
        case IRInstrType::STORE: {
            int slot=0; if (!parseSlot(ins.src2,slot)) break;
            auto v=regConst(ins.src1); if(v) st.slots[slot]=*v; else st.slots.erase(slot); break;
        }
        case IRInstrType::MV: setReg(ins.dest,regConst(ins.src1)); break;
        case IRInstrType::SEQZ:
        case IRInstrType::SNEZ: {
            auto a=regConst(ins.src1);
            setReg(ins.dest,a?std::optional<int32_t>(ins.type==IRInstrType::SEQZ?(*a==0):(*a!=0)):std::nullopt); break;
        }
        case IRInstrType::ADD: case IRInstrType::SUB: case IRInstrType::MUL:
        case IRInstrType::DIV: case IRInstrType::REM: case IRInstrType::SLT: {
            auto a=regConst(ins.src1), b=regConst(ins.src2);
            if(a&&b) setReg(ins.dest,foldBinary(ins.type,*a,*b)); else setReg(ins.dest,std::nullopt); break;
        }
        case IRInstrType::LOAD_ARG:
        case IRInstrType::LOAD_GLOBAL: setReg(ins.dest,std::nullopt); break;
        case IRInstrType::CALL:
            for(int i=0;i<=6;++i) st.regs.erase("t"+std::to_string(i));
            for(int i=0;i<8;++i) st.regs.erase("a"+std::to_string(i));
            break;
        default: break;
    }
}

// Propagate constants through CFG joins/back-edges. The local simplifier is
// deliberately block-local; this pass supplies the missing loop-invariant and
// cross-if facts by rewriting proven LOADs to LI before local simplification.
bool propagateConstantsAcrossCFG(IRFunction& func) {
    const size_t n=func.instrs.size(); if(n==0) return false;
    std::vector<size_t> starts{0};
    for(size_t i=0;i<n;++i){
        if(i>0 && func.instrs[i].type==IRInstrType::LABEL) starts.push_back(i);
        auto t=func.instrs[i].type;
        if((t==IRInstrType::JUMP||t==IRInstrType::BRANCH_ZERO||t==IRInstrType::BRANCH_NONZERO||t==IRInstrType::RET) && i+1<n)
            starts.push_back(i+1);
    }
    std::sort(starts.begin(),starts.end()); starts.erase(std::unique(starts.begin(),starts.end()),starts.end());
    const size_t B=starts.size();
    std::vector<size_t> end(B); std::vector<int> blockOf(n,-1);
    for(size_t b=0;b<B;++b){ end[b]=(b+1<B?starts[b+1]:n); for(size_t i=starts[b];i<end[b];++i) blockOf[i]=static_cast<int>(b); }
    std::unordered_map<std::string,int> labelBlock;
    for(size_t i=0;i<n;++i) if(func.instrs[i].type==IRInstrType::LABEL) labelBlock[func.instrs[i].label]=blockOf[i];
    std::vector<std::vector<int>> succ(B),pred(B);
    for(size_t b=0;b<B;++b){
        if(starts[b]>=end[b]) continue; const auto& last=func.instrs[end[b]-1];
        auto add=[&](int x){if(x>=0 && std::find(succ[b].begin(),succ[b].end(),x)==succ[b].end()) succ[b].push_back(x);};
        if(last.type==IRInstrType::JUMP){auto it=labelBlock.find(last.label);if(it!=labelBlock.end())add(it->second);}
        else if(last.type==IRInstrType::BRANCH_ZERO||last.type==IRInstrType::BRANCH_NONZERO){auto it=labelBlock.find(last.label);if(it!=labelBlock.end())add(it->second);if(b+1<B)add(static_cast<int>(b+1));}
        else if(last.type!=IRInstrType::RET && b+1<B) add(static_cast<int>(b+1));
    }
    for(size_t b=0;b<B;++b) for(int x:succ[b]) pred[x].push_back(static_cast<int>(b));
    std::vector<ConstFlowState> in(B),out(B); std::vector<bool> reachable(B,false), haveOut(B,false); reachable[0]=true;
    for(int iter=0;iter<100;++iter){ bool any=false;
        for(size_t b=0;b<B;++b){
            if(b!=0){ std::vector<const ConstFlowState*> ps; for(int p:pred[b]) if(haveOut[p]) ps.push_back(&out[p]); if(ps.empty()) continue; ConstFlowState ni=meetConstStates(ps); if(!reachable[b]||!sameConstState(ni,in[b])){in[b]=std::move(ni);reachable[b]=true;any=true;} }
            if(!reachable[b]) continue; ConstFlowState st=in[b]; for(size_t i=starts[b];i<end[b];++i) transferConstInstr(func.instrs[i],st);
            if(!haveOut[b]||!sameConstState(st,out[b])){out[b]=std::move(st);haveOut[b]=true;any=true;}
        }
        if(!any) break;
    }
    bool changed=false;
    for(size_t b=0;b<B;++b){ if(!reachable[b]) continue; ConstFlowState st=in[b];
        for(size_t i=starts[b];i<end[b];++i){ auto& ins=func.instrs[i];
            if(ins.type==IRInstrType::LOAD){int slot=0;if(parseSlot(ins.src1,slot)){auto it=st.slots.find(slot);if(it!=st.slots.end()){ins.type=IRInstrType::LI;ins.src1=std::to_string(it->second);ins.src2.clear();changed=true;}}}
            transferConstInstr(ins,st);
        }
    }
    return changed;
}

bool removeUnreachableAfterJump(IRFunction& func) {
    bool changed = false;
    std::vector<IRInstr> out;
    out.reserve(func.instrs.size());
    bool unreachable = false;
    for (const auto& ins : func.instrs) {
        if (unreachable) {
            if (ins.type == IRInstrType::LABEL) {
                unreachable = false;
                out.push_back(ins);
            } else {
                changed = true;
            }
            continue;
        }
        out.push_back(ins);
        if (ins.type == IRInstrType::JUMP || ins.type == IRInstrType::RET) unreachable = true;
    }
    if (changed) func.instrs.swap(out);
    return changed;
}

struct CFG {
    std::vector<std::vector<size_t>> succ;
};

CFG buildCFG(const IRFunction& func) {
    const size_t n = func.instrs.size();
    CFG cfg;
    cfg.succ.resize(n);
    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i) {
        if (func.instrs[i].type == IRInstrType::LABEL) labels[func.instrs[i].label] = i;
    }
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = func.instrs[i];
        auto addLabel = [&](const std::string& lbl) {
            auto it = labels.find(lbl);
            if (it != labels.end()) cfg.succ[i].push_back(it->second);
        };
        if (ins.type == IRInstrType::JUMP) {
            addLabel(ins.label);
        } else if (ins.type == IRInstrType::BRANCH_ZERO || ins.type == IRInstrType::BRANCH_NONZERO) {
            addLabel(ins.label);
            if (i + 1 < n) cfg.succ[i].push_back(i + 1);
        } else if (ins.type != IRInstrType::RET) {
            if (i + 1 < n) cfg.succ[i].push_back(i + 1);
        }
    }
    return cfg;
}

bool eliminateUnreachableCFG(IRFunction& func) {
    if (func.instrs.empty()) return false;
    CFG cfg=buildCFG(func);
    std::vector<char> seen(func.instrs.size(),0);
    std::vector<size_t> work{0}; seen[0]=1;
    while(!work.empty()){
        size_t i=work.back();work.pop_back();
        for(size_t s:cfg.succ[i]) if(s<seen.size()&&!seen[s]){seen[s]=1;work.push_back(s);}
    }
    bool changed=false;std::vector<IRInstr> out;out.reserve(func.instrs.size());
    for(size_t i=0;i<func.instrs.size();++i){if(seen[i])out.push_back(func.instrs[i]);else changed=true;}
    if(changed)func.instrs.swap(out);return changed;
}

std::optional<int> slotUse(const IRInstr& ins) {
    int slot = 0;
    if (ins.type == IRInstrType::LOAD && parseSlot(ins.src1, slot)) return slot;
    return std::nullopt;
}

std::optional<int> slotDef(const IRInstr& ins) {
    int slot = 0;
    if (ins.type == IRInstrType::STORE && parseSlot(ins.src2, slot)) return slot;
    return std::nullopt;
}

bool eliminateDeadPureLoops(IRFunction& func) {
    const size_t n=func.instrs.size(); if(n<3) return false;
    CFG cfg=buildCFG(func);
    std::vector<std::unordered_set<int>> liveIn(n),liveOut(n);
    bool progress=true;
    while(progress){progress=false;for(size_t ri=n;ri-->0;){
        std::unordered_set<int> no;for(size_t q:cfg.succ[ri])no.insert(liveIn[q].begin(),liveIn[q].end());
        auto ni=no;if(auto d=slotDef(func.instrs[ri]))ni.erase(*d);if(auto u=slotUse(func.instrs[ri]))ni.insert(*u);
        if(no!=liveOut[ri]||ni!=liveIn[ri]){liveOut[ri]=std::move(no);liveIn[ri]=std::move(ni);progress=true;}
    }}
    std::unordered_map<std::string,size_t> labels;for(size_t i=0;i<n;++i)if(func.instrs[i].type==IRInstrType::LABEL)labels[func.instrs[i].label]=i;
    for(size_t j=n;j-->0;){
        if(func.instrs[j].type!=IRInstrType::JUMP)continue;auto hit=labels.find(func.instrs[j].label);if(hit==labels.end())continue;
        size_t h=hit->second;if(h>=j||j+1>=n||func.instrs[j+1].type!=IRInstrType::LABEL)continue;
        const std::string exitLabel=func.instrs[j+1].label;
        int exitBranches=0;bool safe=true;std::unordered_set<int> modified;
        for(size_t k=h+1;k<j;++k){auto t=func.instrs[k].type;
            if(t==IRInstrType::LABEL||t==IRInstrType::JUMP||t==IRInstrType::CALL||t==IRInstrType::STORE_GLOBAL||t==IRInstrType::STORE_ARG||t==IRInstrType::RET){safe=false;break;}
            if(t==IRInstrType::BRANCH_ZERO||t==IRInstrType::BRANCH_NONZERO){if(func.instrs[k].label!=exitLabel){safe=false;break;}++exitBranches;}
            if(auto d=slotDef(func.instrs[k]))modified.insert(*d);
        }
        if(!safe||exitBranches!=1||modified.empty())continue;
        bool dead=true;for(int d:modified)if(liveIn[j+1].count(d)){dead=false;break;}if(!dead)continue;
        std::vector<IRInstr> out;out.reserve(n-(j-h+1));
        out.insert(out.end(),func.instrs.begin(),func.instrs.begin()+static_cast<long>(h));
        out.insert(out.end(),func.instrs.begin()+static_cast<long>(j+1),func.instrs.end());
        func.instrs.swap(out);return true; // one at a time; next optimizer round recomputes CFG/liveness
    }
    return false;
}

bool eliminateDeadStores(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n == 0) return false;
    CFG cfg = buildCFG(func);
    std::vector<std::unordered_set<int>> in(n), out(n);

    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t ri = n; ri-- > 0;) {
            std::unordered_set<int> newOut;
            for (size_t s : cfg.succ[ri]) newOut.insert(in[s].begin(), in[s].end());
            std::unordered_set<int> newIn = newOut;
            if (auto d = slotDef(func.instrs[ri])) newIn.erase(*d);
            if (auto u = slotUse(func.instrs[ri])) newIn.insert(*u);
            if (newOut != out[ri] || newIn != in[ri]) {
                out[ri] = std::move(newOut);
                in[ri] = std::move(newIn);
                progress = true;
            }
        }
    }

    bool changed = false;
    std::vector<IRInstr> kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto d = slotDef(func.instrs[i]);
        if (d.has_value() && !out[i].count(*d)) {
            changed = true;
            continue;
        }
        kept.push_back(func.instrs[i]);
    }
    if (changed) func.instrs.swap(kept);
    return changed;
}

void addReg(std::unordered_set<std::string>& s, const std::string& r) {
    if (!r.empty()) s.insert(r);
}

std::unordered_set<std::string> regUses(const IRInstr& ins) {
    std::unordered_set<std::string> u;
    switch (ins.type) {
        case IRInstrType::STORE:
        case IRInstrType::STORE_ARG:
            addReg(u, ins.src1); break;
        case IRInstrType::STORE_GLOBAL:
            addReg(u, ins.src2); break;
        case IRInstrType::MV:
        case IRInstrType::SEQZ:
        case IRInstrType::SNEZ:
        case IRInstrType::BRANCH_ZERO:
        case IRInstrType::BRANCH_NONZERO:
            addReg(u, ins.src1); break;
        case IRInstrType::ADD:
        case IRInstrType::SUB:
        case IRInstrType::MUL:
        case IRInstrType::DIV:
        case IRInstrType::REM:
        case IRInstrType::SLT:
            addReg(u, ins.src1); addReg(u, ins.src2); break;
        case IRInstrType::CALL:
            for (int i = 0; i < 8; ++i) u.insert("a" + std::to_string(i));
            break;
        default:
            break;
    }
    return u;
}

std::unordered_set<std::string> regDefs(const IRInstr& ins) {
    std::unordered_set<std::string> d;
    switch (ins.type) {
        case IRInstrType::LI:
        case IRInstrType::LOAD:
        case IRInstrType::LOAD_ARG:
        case IRInstrType::LOAD_GLOBAL:
        case IRInstrType::MV:
        case IRInstrType::ADD:
        case IRInstrType::SUB:
        case IRInstrType::MUL:
        case IRInstrType::DIV:
        case IRInstrType::REM:
        case IRInstrType::SLT:
        case IRInstrType::SEQZ:
        case IRInstrType::SNEZ:
            addReg(d, ins.dest); break;
        case IRInstrType::CALL:
            for (int i = 0; i <= 6; ++i) d.insert("t" + std::to_string(i));
            for (int i = 0; i < 8; ++i) d.insert("a" + std::to_string(i));
            break;
        default:
            break;
    }
    return d;
}

bool isRemovablePureRegInstr(const IRInstr& ins) {
    switch (ins.type) {
        case IRInstrType::LI:
        case IRInstrType::LOAD:
        case IRInstrType::LOAD_ARG:
        case IRInstrType::LOAD_GLOBAL:
        case IRInstrType::MV:
        case IRInstrType::ADD:
        case IRInstrType::SUB:
        case IRInstrType::MUL:
        case IRInstrType::DIV:
        case IRInstrType::REM:
        case IRInstrType::SLT:
        case IRInstrType::SEQZ:
        case IRInstrType::SNEZ:
            return true;
        default:
            return false;
    }
}

bool eliminateDeadRegisterComputations(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n == 0) return false;
    CFG cfg = buildCFG(func);
    std::vector<std::unordered_set<std::string>> in(n), out(n);

    bool progress = true;
    while (progress) {
        progress = false;
        for (size_t ri = n; ri-- > 0;) {
            std::unordered_set<std::string> newOut;
            if (cfg.succ[ri].empty()) {
                if (!func.isVoid) newOut.insert("a0");
            } else {
                for (size_t s : cfg.succ[ri]) newOut.insert(in[s].begin(), in[s].end());
            }
            auto defs = regDefs(func.instrs[ri]);
            auto uses = regUses(func.instrs[ri]);
            std::unordered_set<std::string> newIn = newOut;
            for (const auto& d : defs) newIn.erase(d);
            newIn.insert(uses.begin(), uses.end());
            if (newOut != out[ri] || newIn != in[ri]) {
                out[ri] = std::move(newOut);
                in[ri] = std::move(newIn);
                progress = true;
            }
        }
    }

    bool changed = false;
    std::vector<IRInstr> kept;
    kept.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = func.instrs[i];
        if (isRemovablePureRegInstr(ins)) {
            auto defs = regDefs(ins);
            bool allDead = !defs.empty();
            for (const auto& d : defs) {
                if (out[i].count(d)) { allDead = false; break; }
            }
            if (allDead) {
                changed = true;
                continue;
            }
        }
        kept.push_back(ins);
    }
    if (changed) func.instrs.swap(kept);
    return changed;
}


// Convert a direct self tail call into a loop inside the current frame.
// IRBuilder emits return f(args) as:
//   ... materialize args into a0..a7 ...
//   CALL f
//   MV t0, a0
//   MV a0, t0
//   JUMP .ret_f
// For <= 8 parameters all new arguments are already in a-registers, so jumping
// back to the function's parameter-copy prologue is ABI-equivalent to a tail call
// and avoids recursive stack growth.  9+ parameter functions are left unchanged
// because their incoming stack-argument addresses differ from the current frame's
// outgoing argument area.
bool eliminateDirectTailRecursion(IRFunction& func) {
    if (func.paramCount > 8) return false;
    const std::string returnLabel = ".ret_" + func.name;
    const std::string tailLabel = ".tail_" + func.name;

    // Capture the parameter-copy prologue generated by IRBuilder:
    //   MV t0,aN ; STORE t0,paramSlot
    std::vector<std::string> paramSlots;
    size_t prologueEnd = 0;
    for (int p = 0; p < func.paramCount; ++p) {
        if (prologueEnd + 1 >= func.instrs.size()) return false;
        const auto& mv = func.instrs[prologueEnd];
        const auto& st = func.instrs[prologueEnd + 1];
        if (mv.type != IRInstrType::MV || mv.dest != "t0" || mv.src1 != "a" + std::to_string(p) ||
            st.type != IRInstrType::STORE || st.src1 != "t0") return false;
        paramSlots.push_back(st.src2);
        prologueEnd += 2;
    }

    bool hasTail = false;
    std::vector<bool> tailCall(func.instrs.size(), false);
    for (size_t i = 0; i + 3 < func.instrs.size(); ++i) {
        const auto& call = func.instrs[i];
        const auto& r0 = func.instrs[i + 1];
        const auto& r1 = func.instrs[i + 2];
        const auto& jump = func.instrs[i + 3];
        if (call.type == IRInstrType::CALL && call.src1 == func.name &&
            r0.type == IRInstrType::MV && r0.dest == "t0" && r0.src1 == "a0" &&
            r1.type == IRInstrType::MV && r1.dest == "a0" && r1.src1 == "t0" &&
            jump.type == IRInstrType::JUMP && jump.label == returnLabel) {
            // The final 2*N instructions immediately before CALL are the
            // argument-slot loads and a-register moves. All argument expressions
            // were already materialized to distinct temp slots, so replacing
            // those moves with stores to parameter slots preserves simultaneous
            // assignment semantics.
            if (i < static_cast<size_t>(2 * func.paramCount)) continue;
            bool setup = true;
            for (int p = 0; p < func.paramCount; ++p) {
                size_t k = i - static_cast<size_t>(2 * func.paramCount) + static_cast<size_t>(2 * p);
                const auto& ld = func.instrs[k];
                const auto& mv = func.instrs[k + 1];
                if (ld.type != IRInstrType::LOAD || ld.dest != "t0" ||
                    mv.type != IRInstrType::MV || mv.dest != "a" + std::to_string(p) || mv.src1 != "t0") {
                    setup = false; break;
                }
            }
            if (setup) { tailCall[i] = true; hasTail = true; }
        }
    }
    if (!hasTail) return false;

    std::vector<IRInstr> out;
    out.reserve(func.instrs.size() + 1);
    for (size_t i = 0; i < func.instrs.size();) {
        if (i == prologueEnd) out.emplace_back(IRInstrType::LABEL, "", "", "", tailLabel);

        // Detect the argument-setup prefix of a marked tail call.
        bool rewrittenSetup = false;
        for (size_t callIdx = i; callIdx < func.instrs.size(); ++callIdx) {
            if (!tailCall[callIdx]) continue;
            size_t setupStart = callIdx - static_cast<size_t>(2 * func.paramCount);
            if (i != setupStart) continue;
            for (int p = 0; p < func.paramCount; ++p) {
                const auto& ld = func.instrs[i + static_cast<size_t>(2 * p)];
                out.push_back(ld);
                out.emplace_back(IRInstrType::STORE, "", "t0", paramSlots[p]);
            }
            i = callIdx;
            rewrittenSetup = true;
            break;
        }
        if (rewrittenSetup) continue;

        if (i < tailCall.size() && tailCall[i]) {
            out.emplace_back(IRInstrType::JUMP, "", "", "", tailLabel);
            i += 4;
            continue;
        }
        out.push_back(func.instrs[i++]);
    }
    if (prologueEnd == func.instrs.size()) out.emplace_back(IRInstrType::LABEL, "", "", "", tailLabel);
    func.instrs.swap(out);
    return true;
}

bool removeRedundantDirectReturnCopies(IRFunction& func) {
    const std::string returnLabel = ".ret_" + func.name;
    bool changed=false;
    std::vector<IRInstr> out; out.reserve(func.instrs.size());
    for(size_t i=0;i<func.instrs.size();++i){
        if(i+3<func.instrs.size() &&
           func.instrs[i].type==IRInstrType::CALL &&
           func.instrs[i+1].type==IRInstrType::MV && func.instrs[i+1].dest=="t0" && func.instrs[i+1].src1=="a0" &&
           func.instrs[i+2].type==IRInstrType::MV && func.instrs[i+2].dest=="a0" && func.instrs[i+2].src1=="t0" &&
           func.instrs[i+3].type==IRInstrType::JUMP && func.instrs[i+3].label==returnLabel){
            out.push_back(func.instrs[i]);
            out.push_back(func.instrs[i+3]);
            i+=3; changed=true; continue;
        }
        out.push_back(func.instrs[i]);
    }
    if(changed) func.instrs.swap(out);
    return changed;
}

bool removeJumpToNextLabel(IRFunction& func) {
    bool changed = false;
    std::vector<IRInstr> out;
    out.reserve(func.instrs.size());
    for (size_t i = 0; i < func.instrs.size(); ++i) {
        const auto& ins = func.instrs[i];
        if (ins.type == IRInstrType::JUMP && i + 1 < func.instrs.size() &&
            func.instrs[i + 1].type == IRInstrType::LABEL &&
            func.instrs[i + 1].label == ins.label) {
            changed = true;
            continue;
        }
        out.push_back(ins);
    }
    if (changed) func.instrs.swap(out);
    return changed;
}



// Hoist the common invariant scalar form
//   LI imm; LOAD invariantSlot; ADD/SUB/MUL; STORE tempSlot
// out of a natural loop.  Only non-trapping integer operations are moved and
// the source slot must have no definition in the loop. This captures repeated
// outer-index expressions in nested matrix/graph loops without requiring a
// full SSA LICM implementation.
bool hoistSimpleLoopInvariants(IRFunction& func) {
    bool any=false;
    for(size_t back=func.instrs.size(); back-- > 0;){
        if(func.instrs[back].type!=IRInstrType::JUMP) continue;
        const std::string hdr=func.instrs[back].label;
        size_t h=back;
        while(h>0){--h;if(func.instrs[h].type==IRInstrType::LABEL&&func.instrs[h].label==hdr)break;}
        if(func.instrs[h].type!=IRInstrType::LABEL||func.instrs[h].label!=hdr||h>=back) continue;
        std::unordered_map<int,int> defs;
        for(size_t k=h+1;k<back;++k) if(auto d=slotDef(func.instrs[k])) defs[*d]++;
        std::vector<std::pair<size_t,size_t>> groups;
        for(size_t k=h+1;k+3<back;++k){
            const auto&a=func.instrs[k];const auto&b=func.instrs[k+1];const auto&op=func.instrs[k+2];const auto&st=func.instrs[k+3];
            if(!(op.type==IRInstrType::ADD||op.type==IRInstrType::SUB||op.type==IRInstrType::MUL)||
               op.dest!="t0"||op.src1!="t1"||op.src2!="t0"||st.type!=IRInstrType::STORE||st.src1!="t0") continue;
            int dst=0;if(!parseSlot(st.src2,dst)||defs[dst]!=1) continue;
            bool invariant=false;
            // Immediate + invariant local.
            if(a.type==IRInstrType::LI&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1") {
                int src=0; invariant=parseSlot(b.src1,src)&&!defs.count(src);
            }
            // Two invariant locals. This is common in nested graph/matrix loops
            // where outer-loop indices or precomputed scalars are recombined on
            // every inner iteration.
            if(a.type==IRInstrType::LOAD&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1") {
                int s0=0,s1=0; invariant=parseSlot(a.src1,s0)&&parseSlot(b.src1,s1)&&!defs.count(s0)&&!defs.count(s1);
            }
            if(!invariant) continue;
            groups.push_back({k,k+4}); k+=3;
        }
        if(!groups.empty()) {
            std::vector<bool> remove(func.instrs.size(),false);
            std::vector<IRInstr> hoisted;
            for(auto [a,b]:groups) for(size_t k=a;k<b;++k){hoisted.push_back(func.instrs[k]);remove[k]=true;}
            std::vector<IRInstr> out;out.reserve(func.instrs.size());
            for(size_t k=0;k<func.instrs.size();++k){
                if(k==h) out.insert(out.end(),hoisted.begin(),hoisted.end());
                if(!remove[k]) out.push_back(func.instrs[k]);
            }
            func.instrs.swap(out); any=true; back=func.instrs.size(); continue;
        }

    }
    return any;
}

// Safe 4x unrolling for canonical straight-line while loops.  We duplicate the
// condition+body three extra times and keep only one backward jump.  Every copy
// still performs the original condition check, so trip counts, overflow and
// side effects are unchanged; only continue/break/nested-control-flow loops are
// excluded because they contain extra labels/jumps.
bool unrollSimpleLoops(IRFunction& func) {
    bool changed=false;
    for (size_t j=func.instrs.size(); j-- > 0;) {
        if (func.instrs[j].type!=IRInstrType::JUMP) continue;
        const std::string header=func.instrs[j].label;
        if (j+1>=func.instrs.size() || func.instrs[j+1].type!=IRInstrType::LABEL) continue;
        const std::string endLabel=func.instrs[j+1].label;
        size_t h=j;
        while(h>0){--h;if(func.instrs[h].type==IRInstrType::LABEL && func.instrs[h].label==header)break;}
        if(func.instrs[h].type!=IRInstrType::LABEL||func.instrs[h].label!=header||h>=j) continue;
        int branchCount=0; bool ok=true;
        for(size_t k=h+1;k<j;++k){
            auto t=func.instrs[k].type;
            if(t==IRInstrType::LABEL||t==IRInstrType::JUMP||t==IRInstrType::RET){ok=false;break;}
            if(t==IRInstrType::BRANCH_ZERO||t==IRInstrType::BRANCH_NONZERO){
                ++branchCount; if(func.instrs[k].label!=endLabel){ok=false;break;}
            }
        }
        if(!ok||branchCount!=1) continue;
        const size_t segBegin=h+1, segEnd=j;
        const size_t segLen=segEnd-segBegin;
        if(segLen==0 || segLen>12) continue; // avoid pathological code growth
        std::vector<IRInstr> repl; repl.reserve(func.instrs.size()+segLen*3);
        repl.insert(repl.end(),func.instrs.begin(),func.instrs.begin()+static_cast<long>(j));
        // Existing first copy is already present before j; append three more.
        for(int copy=0;copy<3;++copy) repl.insert(repl.end(),func.instrs.begin()+static_cast<long>(segBegin),func.instrs.begin()+static_cast<long>(segEnd));
        repl.insert(repl.end(),func.instrs.begin()+static_cast<long>(j),func.instrs.end());
        func.instrs.swap(repl); changed=true;
        // Continue scanning earlier original loops; indices before h are unchanged.
        j=h+1;
    }
    return changed;
}

void optimizeFunction(IRFunction& func) {
    // Do this before local simplification so the exact IRBuilder tail-call pattern
    // is still visible.
    eliminateDirectTailRecursion(func);

    // A few rounds are enough because each pass only removes/simplifies instructions.
    for (int round = 0; round < 4; ++round) {
        bool changed = false;
        changed |= propagateConstantsAcrossCFG(func);
        changed |= simplifyLocally(func);
        changed |= removeUnreachableAfterJump(func);
        changed |= eliminateUnreachableCFG(func);
        changed |= eliminateDeadPureLoops(func);
        changed |= eliminateDeadStores(func);
        changed |= eliminateDeadRegisterComputations(func);
        changed |= removeRedundantDirectReturnCopies(func);
        changed |= removeJumpToNextLabel(func);
        if (!changed) break;
    }
    hoistSimpleLoopInvariants(func);
    unrollSimpleLoops(func);
}


bool isLeafInlineCandidate(const IRFunction& f, size_t maxInstr, int maxLocal) {
    if (f.name == "main" || f.paramCount > 8 || f.instrs.size() > maxInstr || f.localSize > maxLocal) return false;
    for (const auto& ins : f.instrs) {
        if (ins.type == IRInstrType::CALL || ins.type == IRInstrType::LOAD_ARG || ins.type == IRInstrType::STORE_ARG)
            return false;
    }
    return true;
}

std::vector<bool> loopHotInstructions(const IRFunction& f) {
    std::vector<bool> hot(f.instrs.size(), false);
    std::unordered_map<std::string,size_t> labels;
    for(size_t i=0;i<f.instrs.size();++i) if(f.instrs[i].type==IRInstrType::LABEL) labels[f.instrs[i].label]=i;
    for(size_t j=0;j<f.instrs.size();++j) {
        if(f.instrs[j].type!=IRInstrType::JUMP) continue;
        auto it=labels.find(f.instrs[j].label); if(it==labels.end()||it->second>=j) continue;
        for(size_t k=it->second;k<=j;++k) hot[k]=true;
    }
    return hot;
}

bool inlineSmallLeafFunctions(IRProgram& program) {
    // Keep the old small-function threshold everywhere, but allow a somewhat
    // larger leaf at a hot loop call-site: paying code size once is usually much
    // cheaper than millions of ABI/prologue/epilogue sequences at runtime.
    std::unordered_map<std::string, const IRFunction*> candidates;
    for (const auto& f : program.functions)
        if (isLeafInlineCandidate(f, 96, 384)) candidates[f.name] = &f;
    if (candidates.empty()) return false;

    bool any=false;
    std::uint64_t serial=0;
    for (auto& caller : program.functions) {
        const auto hot = loopHotInstructions(caller);
        std::vector<IRInstr> out;
        out.reserve(caller.instrs.size());
        bool changed=false;
        for (size_t callPos=0; callPos<caller.instrs.size(); ++callPos) {
            const auto& ins=caller.instrs[callPos];
            if (ins.type != IRInstrType::CALL || ins.src1 == caller.name) { out.push_back(ins); continue; }
            auto it=candidates.find(ins.src1);
            if (it==candidates.end()) { out.push_back(ins); continue; }
            const IRFunction& callee=*it->second;
            const bool ordinarySmall = callee.instrs.size()<=48 && callee.localSize<=192;
            const bool hotSite = callPos<hot.size() && hot[callPos];
            if (!ordinarySmall && !hotSite) { out.push_back(ins); continue; }
            const int base=caller.localSize;
            caller.localSize += callee.localSize;
            const std::string suffix=".inl"+std::to_string(serial++);
            std::unordered_map<std::string,std::string> labels;
            for(const auto& ci:callee.instrs) if(ci.type==IRInstrType::LABEL) labels[ci.label]=ci.label+suffix;
            for(const auto& ci0:callee.instrs){
                IRInstr ci=ci0;
                if(ci.type==IRInstrType::LOAD) ci.src1=std::to_string(std::stoi(ci.src1)+base);
                else if(ci.type==IRInstrType::STORE) ci.src2=std::to_string(std::stoi(ci.src2)+base);
                if(ci.type==IRInstrType::LABEL||ci.type==IRInstrType::JUMP||
                   ci.type==IRInstrType::BRANCH_ZERO||ci.type==IRInstrType::BRANCH_NONZERO){
                    auto l=labels.find(ci.label); if(l!=labels.end()) ci.label=l->second;
                }
                out.push_back(std::move(ci));
            }
            changed=true; any=true;
        }
        if(changed){ caller.instrs.swap(out); optimizeFunction(caller); }
    }
    return any;
}


bool propagateImmutableGlobals(IRProgram& program) {
    if (program.globalVars.empty()) return false;
    std::unordered_map<std::string, int32_t> initial;
    for (const auto& [name, value] : program.globalVars) initial[name]=static_cast<int32_t>(value);
    std::unordered_set<std::string> written;
    for (const auto& f : program.functions)
        for (const auto& ins : f.instrs)
            if (ins.type==IRInstrType::STORE_GLOBAL) written.insert(ins.src1);
    bool changed=false;
    for (auto& f : program.functions) {
        for (auto& ins : f.instrs) {
            if (ins.type!=IRInstrType::LOAD_GLOBAL || written.count(ins.src1)) continue;
            auto it=initial.find(ins.src1); if(it==initial.end()) continue;
            ins.type=IRInstrType::LI;
            ins.src1=std::to_string(it->second);
            ins.src2.clear(); ins.label.clear();
            changed=true;
        }
    }
    return changed;
}

} // namespace

void IROptimizer::optimize(IRProgram& program) const {
    propagateImmutableGlobals(program);
    for (auto& func : program.functions) optimizeFunction(func);
    inlineSmallLeafFunctions(program);
}

} // namespace toycc
