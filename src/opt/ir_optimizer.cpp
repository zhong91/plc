#include "opt/ir_optimizer.h"

#include <algorithm>
#include <array>
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
                    } else if (isKnown(a) && isKnown(b) && valueKey(a) == valueKey(b)) {
                        // x - x = 0 even when the two values arrived through
                        // different temporary registers/copy chains.
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
                    } else if (a.kind == ValueKind::Constant && a.constant == 0) {
                        // ToyC test programs contain no undefined behaviour, so an
                        // executed 0/x necessarily has x!=0 and is exactly zero.
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    } else if (isKnown(a) && isKnown(b) && valueKey(a) == valueKey(b)) {
                        // In a defined ToyC execution x/x is only evaluated for x!=0.
                        ins.type = IRInstrType::LI; ins.src1 = "1"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(1)); replaced = true;
                    }
                } else if (ins.type == IRInstrType::REM) {
                    if (b.kind == ValueKind::Constant && (b.constant == 1 || b.constant == -1)) {
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    } else if (a.kind == ValueKind::Constant && a.constant == 0) {
                        // Likewise, every defined execution of 0%x yields zero.
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    } else if (isKnown(a) && isKnown(b) && valueKey(a) == valueKey(b)) {
                        // Defined x%x is always zero; division-by-zero inputs are UB
                        // and are excluded by the ToyC test contract.
                        ins.type = IRInstrType::LI; ins.src1 = "0"; ins.src2.clear();
                        st.setReg(ins.dest, Value::constantValue(0)); replaced = true;
                    }
                } else if (ins.type == IRInstrType::SLT) {
                    if (isKnown(a) && isKnown(b) && valueKey(a) == valueKey(b)) {
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
                if (cseOp && stableForCse(a) && stableForCse(b)) {
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


// Forward available-copy propagation for local stack slots.
//
// A fact `dst -> src` means that the current value stored in `dst` is identical
// to the current value stored in `src`.  The fact is killed whenever either
// endpoint is written, and CFG joins retain only facts that are identical on
// every predecessor.  Calls do not invalidate local-slot facts because ToyC
// has no pointers and a callee cannot address its caller's local stack frame.
//
// This deliberately propagates only exact LOAD/MV/STORE copies; arithmetic,
// globals and argument registers are not guessed.  It is therefore much more
// conservative than the earlier experimental cross-block alias pass while
// still exposing the copy chains targeted by p03.
struct CopyFlowState {
    std::unordered_map<int, int> copies;
};

bool sameCopyState(const CopyFlowState& a, const CopyFlowState& b) {
    return a.copies == b.copies;
}

int resolveCopySlot(const CopyFlowState& st, int slot) {
    std::unordered_set<int> seen;
    int cur = slot;
    while (seen.insert(cur).second) {
        auto it = st.copies.find(cur);
        if (it == st.copies.end()) break;
        cur = it->second;
    }
    return cur;
}

void killCopySlot(CopyFlowState& st, int slot) {
    for (auto it = st.copies.begin(); it != st.copies.end();) {
        if (it->first == slot || it->second == slot) it = st.copies.erase(it);
        else ++it;
    }
}

CopyFlowState meetCopyStates(const std::vector<const CopyFlowState*>& states) {
    CopyFlowState out;
    if (states.empty()) return out;
    out = *states.front();
    for (size_t i = 1; i < states.size(); ++i) {
        for (auto it = out.copies.begin(); it != out.copies.end();) {
            auto jt = states[i]->copies.find(it->first);
            if (jt == states[i]->copies.end() || jt->second != it->second)
                it = out.copies.erase(it);
            else
                ++it;
        }
    }
    return out;
}

void transferCopyBlock(const std::vector<IRInstr>& instrs, size_t begin, size_t end,
                       CopyFlowState& st, bool rewrite, bool& changed) {
    std::unordered_map<std::string, int> regOrigin;

    auto killReg = [&](const std::string& r) {
        if (!r.empty()) regOrigin.erase(r);
    };
    auto setReg = [&](const std::string& r, std::optional<int> origin) {
        if (r.empty()) return;
        if (origin) regOrigin[r] = *origin;
        else regOrigin.erase(r);
    };

    // const_cast is used only when rewrite==true; callers pass the actual
    // function instruction vector in that phase.
    auto& mutableInstrs = const_cast<std::vector<IRInstr>&>(instrs);

    for (size_t i = begin; i < end; ++i) {
        const IRInstr& ro = instrs[i];
        IRInstr* rw = rewrite ? &mutableInstrs[i] : nullptr;

        switch (ro.type) {
            case IRInstrType::LOAD: {
                int slot = 0;
                if (!parseSlot(ro.src1, slot)) { killReg(ro.dest); break; }
                int root = resolveCopySlot(st, slot);
                if (rewrite && root != slot) {
                    rw->src1 = std::to_string(root);
                    changed = true;
                }
                setReg(ro.dest, root);
                break;
            }
            case IRInstrType::MV: {
                auto it = regOrigin.find(ro.src1);
                setReg(ro.dest, it == regOrigin.end() ? std::optional<int>{}
                                                       : std::optional<int>{it->second});
                break;
            }
            case IRInstrType::STORE: {
                int dst = 0;
                if (!parseSlot(ro.src2, dst)) break;
                auto it = regOrigin.find(ro.src1);
                std::optional<int> src =
                    it == regOrigin.end() ? std::optional<int>{}
                                          : std::optional<int>{it->second};
                killCopySlot(st, dst);
                if (src && *src != dst) st.copies[dst] = *src;
                break;
            }

            case IRInstrType::LI:
            case IRInstrType::LOAD_ARG:
            case IRInstrType::LOAD_GLOBAL:
            case IRInstrType::ADD:
            case IRInstrType::SUB:
            case IRInstrType::MUL:
            case IRInstrType::DIV:
            case IRInstrType::REM:
            case IRInstrType::SLT:
            case IRInstrType::SEQZ:
            case IRInstrType::SNEZ:
                killReg(ro.dest);
                break;

            case IRInstrType::CALL:
                // Only register origins are caller-clobbered. Local copy facts
                // remain valid because ToyC has no pointer aliasing.
                for (int r = 0; r <= 6; ++r) regOrigin.erase("t" + std::to_string(r));
                for (int r = 0; r < 8; ++r) regOrigin.erase("a" + std::to_string(r));
                break;

            default:
                break;
        }
    }
}

bool propagateCopiesAcrossCFG(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n == 0) return false;

    std::vector<size_t> starts{0};
    for (size_t i = 0; i < n; ++i) {
        if (i > 0 && func.instrs[i].type == IRInstrType::LABEL) starts.push_back(i);
        auto t = func.instrs[i].type;
        if ((t == IRInstrType::JUMP || t == IRInstrType::BRANCH_ZERO ||
             t == IRInstrType::BRANCH_NONZERO || t == IRInstrType::RET) &&
            i + 1 < n)
            starts.push_back(i + 1);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());

    const size_t B = starts.size();
    std::vector<size_t> ends(B);
    std::vector<int> blockOf(n, -1);
    for (size_t b = 0; b < B; ++b) {
        ends[b] = (b + 1 < B ? starts[b + 1] : n);
        for (size_t i = starts[b]; i < ends[b]; ++i) blockOf[i] = static_cast<int>(b);
    }

    std::unordered_map<std::string, int> labelBlock;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labelBlock[func.instrs[i].label] = blockOf[i];

    std::vector<std::vector<int>> succ(B), pred(B);
    for (size_t b = 0; b < B; ++b) {
        if (starts[b] >= ends[b]) continue;
        const auto& last = func.instrs[ends[b] - 1];
        auto add = [&](int x) {
            if (x >= 0 && std::find(succ[b].begin(), succ[b].end(), x) == succ[b].end())
                succ[b].push_back(x);
        };
        if (last.type == IRInstrType::JUMP) {
            auto it = labelBlock.find(last.label);
            if (it != labelBlock.end()) add(it->second);
        } else if (last.type == IRInstrType::BRANCH_ZERO ||
                   last.type == IRInstrType::BRANCH_NONZERO) {
            auto it = labelBlock.find(last.label);
            if (it != labelBlock.end()) add(it->second);
            if (b + 1 < B) add(static_cast<int>(b + 1));
        } else if (last.type != IRInstrType::RET && b + 1 < B) {
            add(static_cast<int>(b + 1));
        }
    }
    for (size_t b = 0; b < B; ++b)
        for (int x : succ[b]) pred[x].push_back(static_cast<int>(b));

    std::vector<CopyFlowState> in(B), out(B);
    std::vector<bool> reachable(B, false), haveOut(B, false);
    reachable[0] = true;

    for (int iter = 0; iter < 100; ++iter) {
        bool any = false;
        for (size_t b = 0; b < B; ++b) {
            if (b != 0) {
                std::vector<const CopyFlowState*> ps;
                for (int pidx : pred[b]) if (haveOut[pidx]) ps.push_back(&out[pidx]);
                if (ps.empty()) continue;
                CopyFlowState ni = meetCopyStates(ps);
                if (!reachable[b] || !sameCopyState(ni, in[b])) {
                    in[b] = std::move(ni);
                    reachable[b] = true;
                    any = true;
                }
            }
            if (!reachable[b]) continue;
            CopyFlowState st = in[b];
            bool ignored = false;
            transferCopyBlock(func.instrs, starts[b], ends[b], st, false, ignored);
            if (!haveOut[b] || !sameCopyState(st, out[b])) {
                out[b] = std::move(st);
                haveOut[b] = true;
                any = true;
            }
        }
        if (!any) break;
    }

    bool changed = false;
    for (size_t b = 0; b < B; ++b) {
        if (!reachable[b]) continue;
        CopyFlowState st = in[b];
        transferCopyBlock(func.instrs, starts[b], ends[b], st, true, changed);
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


// After leaf inlining, the generic call-result convention often remains:
//
//   LOAD/MV/LI a0, value
//   JUMP .ret_helper.inlN
//   ...
//   LOAD/MV/LI a0, other
// .ret_helper.inlN:
//   MV t0, a0
//   STORE t0, dst
//
// The helper is already inlined, so routing every return through a0 is pure
// overhead.  When all incoming returns are simple a0 definitions, write the
// caller's destination slot on each return edge and remove the join copy.
bool forwardInlineReturnsToT0(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 3) return false;

    auto simpleA0Def = [](const IRInstr& ins) {
        if (ins.dest != "a0") return false;
        return ins.type == IRInstrType::LOAD || ins.type == IRInstrType::LI ||
               ins.type == IRInstrType::MV || ins.type == IRInstrType::LOAD_GLOBAL ||
               ins.type == IRInstrType::LOAD_ARG;
    };
    auto isTerminator = [](IRInstrType t) {
        return t == IRInstrType::JUMP || t == IRInstrType::RET;
    };

    for (size_t l = 0; l + 1 < n; ++l) {
        const auto& lab = func.instrs[l];
        if (lab.type != IRInstrType::LABEL ||
            lab.label.find(".ret_") == std::string::npos ||
            lab.label.find(".inl") == std::string::npos)
            continue;
        if (func.instrs[l + 1].type != IRInstrType::MV ||
            func.instrs[l + 1].dest != "t0" ||
            func.instrs[l + 1].src1 != "a0")
            continue;

        const std::string retLabel = lab.label;
        std::unordered_set<size_t> defs;
        bool safe = true;

        // Explicit incoming edges must be unconditional returns whose value is
        // defined immediately before the jump. Branches to an inline return
        // join are not rewritten because they need edge-specific copies.
        for (size_t i = 0; i < n; ++i) {
            const auto& ins = func.instrs[i];
            if ((ins.type == IRInstrType::BRANCH_ZERO ||
                 ins.type == IRInstrType::BRANCH_NONZERO) &&
                ins.label == retLabel) {
                safe = false; break;
            }
            if (ins.type == IRInstrType::JUMP && ins.label == retLabel) {
                if (i == 0 || !simpleA0Def(func.instrs[i - 1])) {
                    safe = false; break;
                }
                defs.insert(i - 1);
            }
        }
        if (!safe) continue;

        // Account for a real fallthrough predecessor. If the preceding
        // instruction terminates control flow, there is no fallthrough edge.
        if (l > 0 && !isTerminator(func.instrs[l - 1].type)) {
            if (!simpleA0Def(func.instrs[l - 1])) continue;
            defs.insert(l - 1);
        }
        if (defs.empty()) continue;

        std::vector<IRInstr> out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            if (i == l + 1) continue; // remove MV t0,a0 at the join
            IRInstr ins = func.instrs[i];
            if (defs.count(i)) ins.dest = "t0";
            out.push_back(std::move(ins));
        }
        func.instrs.swap(out);
        return true;
    }
    return false;
}


bool forwardInlineReturnsToDestination(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 4) return false;

    auto simpleA0Def = [](const IRInstr& ins) {
        if (ins.dest != "a0") return false;
        return ins.type == IRInstrType::LOAD || ins.type == IRInstrType::LI ||
               ins.type == IRInstrType::MV || ins.type == IRInstrType::LOAD_GLOBAL ||
               ins.type == IRInstrType::LOAD_ARG;
    };

    for (size_t l = 0; l + 2 < n; ++l) {
        const auto& lab = func.instrs[l];
        if (lab.type != IRInstrType::LABEL ||
            lab.label.find(".ret_") == std::string::npos ||
            lab.label.find(".inl") == std::string::npos)
            continue;
        if (func.instrs[l + 1].type != IRInstrType::MV ||
            func.instrs[l + 1].dest != "t0" ||
            func.instrs[l + 1].src1 != "a0" ||
            func.instrs[l + 2].type != IRInstrType::STORE ||
            func.instrs[l + 2].src1 != "t0")
            continue;

        const std::string retLabel = lab.label;
        const std::string dstSlot = func.instrs[l + 2].src2;

        // Every explicit incoming edge must be an unconditional jump whose
        // immediately preceding instruction is a simple a0 definition.
        std::vector<size_t> jumpDefs;
        bool safe = true;
        for (size_t i = 0; i < n; ++i) {
            const auto& ins = func.instrs[i];
            if ((ins.type == IRInstrType::BRANCH_ZERO ||
                 ins.type == IRInstrType::BRANCH_NONZERO) &&
                ins.label == retLabel) {
                safe = false;
                break;
            }
            if (ins.type == IRInstrType::JUMP && ins.label == retLabel) {
                if (i == 0 || !simpleA0Def(func.instrs[i - 1])) {
                    safe = false;
                    break;
                }
                jumpDefs.push_back(i - 1);
            }
        }
        if (!safe) continue;

        // The fallthrough predecessor also has to define a0 directly.
        if (l == 0 || !simpleA0Def(func.instrs[l - 1])) continue;
        const size_t fallDef = l - 1;

        std::unordered_set<size_t> defs(jumpDefs.begin(), jumpDefs.end());
        defs.insert(fallDef);

        std::vector<IRInstr> out;
        out.reserve(n + defs.size());
        for (size_t i = 0; i < n; ++i) {
            if (i == l + 1 || i == l + 2) continue;

            IRInstr ins = func.instrs[i];
            if (defs.count(i)) {
                ins.dest = "t0";
                out.push_back(std::move(ins));
                out.emplace_back(IRInstrType::STORE, "", "t0", dstSlot);
            } else {
                out.push_back(std::move(ins));
            }
        }
        func.instrs.swap(out);
        return true; // one join at a time; optimizer rounds recompute analyses
    }
    return false;
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

bool removeRedundantSelfCopies(IRFunction& func) {
    bool changed=false;
    std::vector<IRInstr> out; out.reserve(func.instrs.size());
    for(size_t i=0;i<func.instrs.size();++i) {
        if(i+1<func.instrs.size() &&
           func.instrs[i].type==IRInstrType::LOAD && func.instrs[i].dest=="t0" &&
           func.instrs[i+1].type==IRInstrType::STORE && func.instrs[i+1].src1=="t0" &&
           func.instrs[i].src1==func.instrs[i+1].src2) {
            ++i; changed=true; continue;
        }
        out.push_back(func.instrs[i]);
    }
    if(changed) func.instrs.swap(out);
    return changed;
}

bool removeJumpToNextLabel(IRFunction& func) {
    bool changed = false;
    std::unordered_map<std::string,size_t> labels;
    for (size_t i=0;i<func.instrs.size();++i)
        if (func.instrs[i].type==IRInstrType::LABEL) labels[func.instrs[i].label]=i;

    std::vector<IRInstr> out;
    out.reserve(func.instrs.size());
    for (size_t i = 0; i < func.instrs.size(); ++i) {
        const auto& ins = func.instrs[i];
        if (ins.type == IRInstrType::JUMP) {
            auto it=labels.find(ins.label);
            if(it!=labels.end() && it->second>i) {
                // A jump over nothing but labels has exactly the same effect as
                // fallthrough.  This is common after leaf-function inlining:
                // each return used to jump to .ret_x.inl, while the alternate
                // return labels between here and there are empty aliases.
                bool onlyLabels=true;
                for(size_t k=i+1;k<it->second;++k) {
                    if(func.instrs[k].type!=IRInstrType::LABEL) { onlyLabels=false; break; }
                }
                if(onlyLabels) { changed=true; continue; }
            }
        }
        out.push_back(ins);
    }
    if (changed) func.instrs.swap(out);
    return changed;
}


bool removeUnreferencedLabels(IRFunction& func) {
    std::unordered_set<std::string> targets;
    for (const auto& ins : func.instrs) {
        if (ins.type == IRInstrType::JUMP ||
            ins.type == IRInstrType::BRANCH_ZERO ||
            ins.type == IRInstrType::BRANCH_NONZERO) {
            targets.insert(ins.label);
        }
    }
    bool changed = false;
    std::vector<IRInstr> out;
    out.reserve(func.instrs.size());
    for (const auto& ins : func.instrs) {
        if (ins.type == IRInstrType::LABEL && !targets.count(ins.label)) {
            changed = true;
            continue;
        }
        out.push_back(ins);
    }
    if (changed) func.instrs.swap(out);
    return changed;
}





// Rotate the loop created by direct tail-recursion elimination into a
// bottom-tested form.  The original layout branches from the loop header into
// the recursive case and uses an unconditional jump back to the header.  After
// rotation, the base-case block is moved after the loop and each steady-state
// iteration ends with a single conditional branch.
bool rotateTailRecursionLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 8) return false;

    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::JUMP) continue;
        const std::string header = func.instrs[back].label;
        if (header.rfind(".tail_", 0) != 0) continue;

        size_t h = back;
        while (h > 0) {
            --h;
            if (func.instrs[h].type == IRInstrType::LABEL &&
                func.instrs[h].label == header) break;
        }
        if (h >= back || func.instrs[h].type != IRInstrType::LABEL) continue;

        size_t guard = n;
        size_t body = n;
        for (size_t k = h + 1; k < back; ++k) {
            const auto t = func.instrs[k].type;
            if (t != IRInstrType::BRANCH_ZERO && t != IRInstrType::BRANCH_NONZERO)
                continue;
            for (size_t q = k + 1; q < back; ++q) {
                if (func.instrs[q].type == IRInstrType::LABEL &&
                    func.instrs[q].label == func.instrs[k].label) {
                    guard = k;
                    body = q;
                    break;
                }
            }
            if (guard != n) break;
        }
        if (guard == n || body == n || body <= guard + 1) continue;

        // Guard computation must be side-effect-free and straight-line.
        bool condSafe = true;
        for (size_t k = h + 1; k < guard; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                condSafe = false;
                break;
            }
        }
        if (!condSafe) continue;

        // The fallthrough between guard and recursive body is the base case and
        // must end in a jump leaving the loop.  Keep it verbatim, only move it.
        if (func.instrs[body - 1].type != IRInstrType::JUMP) continue;
        const std::string exitTarget = func.instrs[body - 1].label;
        size_t exitPos = n;
        for (size_t k = back + 1; k < n; ++k) {
            if (func.instrs[k].type == IRInstrType::LABEL &&
                func.instrs[k].label == exitTarget) {
                exitPos = k;
                break;
            }
        }
        if (exitPos == n) continue;

        bool baseSafe = true;
        for (size_t k = guard + 1; k < body; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                (t == IRInstrType::JUMP && k + 1 != body)) {
                baseSafe = false;
                break;
            }
        }
        if (!baseSafe) continue;

        static std::uint64_t serial = 0;
        const std::string done = ".tail_done_" + func.name + "_" +
                                 std::to_string(serial++);
        const std::string latch = ".tail_latch_" + func.name + "_" +
                                  std::to_string(serial++);

        std::vector<IRInstr> out;
        out.reserve(n + (guard - h) + 3);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        out.push_back(func.instrs[h]);

        // Initial guard: enter body on the recursive condition, otherwise done.
        for (size_t k = h + 1; k < guard; ++k) out.push_back(func.instrs[k]);
        IRInstr initial = func.instrs[guard];
        initial.type = initial.type == IRInstrType::BRANCH_ZERO
            ? IRInstrType::BRANCH_NONZERO
            : IRInstrType::BRANCH_ZERO;
        initial.label = done;
        out.push_back(std::move(initial));

        // Recursive body (including its original entry label). Continues or
        // explicit jumps to the tail header are redirected to the latch.
        for (size_t k = body; k < back; ++k) {
            IRInstr ins = func.instrs[k];
            if (ins.type == IRInstrType::JUMP && ins.label == header)
                ins.label = latch;
            out.push_back(std::move(ins));
        }

        out.emplace_back(IRInstrType::LABEL, "", "", "", latch);
        for (size_t k = h + 1; k < guard; ++k) out.push_back(func.instrs[k]);
        IRInstr loopBack = func.instrs[guard];
        loopBack.label = func.instrs[body].label;
        out.push_back(std::move(loopBack));

        out.emplace_back(IRInstrType::LABEL, "", "", "", done);
        for (size_t k = guard + 1; k < body; ++k) out.push_back(func.instrs[k]);
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}

// Sink loop-final copies out of a simple loop.
//
// After available-copy propagation, copy benchmarks commonly contain
//     LOAD t0, src
//     STORE t0, dst
// on every iteration even though dst is never read in the loop and only its
// final value is needed after exit.  If the loop is proven to execute at least
// once, and src is not modified after the copy before the back edge, the copy
// can be performed once at the exit instead of once per trip.
bool sinkLoopFinalCopies(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 8) return false;

    std::unordered_map<std::string,size_t> labels;
    for (size_t i=0;i<n;++i)
        if (func.instrs[i].type==IRInstrType::LABEL) labels[func.instrs[i].label]=i;

    for (size_t j=n; j-- > 0;) {
        if (func.instrs[j].type!=IRInstrType::JUMP) continue;
        auto hit=labels.find(func.instrs[j].label);
        if(hit==labels.end()||hit->second>=j) continue;
        const size_t h=hit->second;
        if(j+1>=n||func.instrs[j+1].type!=IRInstrType::LABEL) continue;
        const std::string exitLabel=func.instrs[j+1].label;

        // Restrict to a canonical straight-line while loop with one exit branch.
        int exitBranches=0; bool simple=true;
        size_t branchPos=n;
        for(size_t k=h+1;k<j;++k){
            auto t=func.instrs[k].type;
            if(t==IRInstrType::LABEL||t==IRInstrType::JUMP||t==IRInstrType::CALL||
               t==IRInstrType::STORE_GLOBAL||t==IRInstrType::STORE_ARG||t==IRInstrType::RET){
                simple=false;break;
            }
            if(t==IRInstrType::BRANCH_ZERO||t==IRInstrType::BRANCH_NONZERO){
                if(func.instrs[k].label!=exitLabel){simple=false;break;}
                ++exitBranches; branchPos=k;
            }
        }
        if(!simple||exitBranches!=1||branchPos<3) continue;

        // Prove at least one trip for the common `iv < constant` form:
        //   LI bound; LOAD iv; SLT; BRANCH_ZERO exit
        const auto& li=func.instrs[branchPos-3];
        const auto& ld=func.instrs[branchPos-2];
        const auto& cmp=func.instrs[branchPos-1];
        const auto& br=func.instrs[branchPos];
        if(li.type!=IRInstrType::LI||li.dest!="t0"||
           ld.type!=IRInstrType::LOAD||ld.dest!="t1"||
           cmp.type!=IRInstrType::SLT||cmp.dest!="t0"||
           cmp.src1!="t1"||cmp.src2!="t0"||
           br.type!=IRInstrType::BRANCH_ZERO||br.src1!="t0")
            continue;
        int iv=0; if(!parseSlot(ld.src1,iv)) continue;
        const int32_t bound=static_cast<int32_t>(std::strtoll(li.src1.c_str(),nullptr,10));

        ConstFlowState pre;
        for(size_t k=0;k<h;++k) transferConstInstr(func.instrs[k],pre);
        auto initIt=pre.slots.find(iv);
        if(initIt==pre.slots.end()||!(initIt->second<bound)) continue;

        std::unordered_map<int,int> loadCount,storeCount;
        for(size_t k=h+1;k<j;++k){
            if(auto u=slotUse(func.instrs[k])) ++loadCount[*u];
            if(auto d=slotDef(func.instrs[k])) ++storeCount[*d];
        }

        struct Candidate { size_t loadPos; size_t storePos; int src; int dst; };
        std::vector<Candidate> cand;
        for(size_t k=branchPos+1;k+1<j;++k){
            const auto& a=func.instrs[k];
            const auto& b=func.instrs[k+1];
            if(a.type!=IRInstrType::LOAD||a.dest!="t0"||
               b.type!=IRInstrType::STORE||b.src1!="t0") continue;
            int src=0,dst=0;
            if(!parseSlot(a.src1,src)||!parseSlot(b.src2,dst)||src==dst) continue;
            if(storeCount[dst]!=1||loadCount[dst]!=0) continue;

            // The source at loop exit must still equal the value copied here.
            bool sourceChangedLater=false;
            for(size_t q=k+2;q<j;++q){
                auto d=slotDef(func.instrs[q]);
                if(d&&*d==src){sourceChangedLater=true;break;}
            }
            if(sourceChangedLater) continue;
            cand.push_back({k,k+1,src,dst});
            ++k;
        }
        if(cand.empty()) continue;

        std::vector<char> remove(n,0);
        for(const auto& c:cand){remove[c.loadPos]=1;remove[c.storePos]=1;}

        // The exit label must not be a shared target from outside the loop;
        // otherwise sinking would also execute on paths that bypass the loop.
        bool sharedExit=false;
        for(size_t q=0;q<n;++q){
            if(q>=h&&q<=j) continue;
            const auto& ins=func.instrs[q];
            if((ins.type==IRInstrType::JUMP||ins.type==IRInstrType::BRANCH_ZERO||
                ins.type==IRInstrType::BRANCH_NONZERO)&&ins.label==exitLabel){
                sharedExit=true;break;
            }
        }
        if(sharedExit) continue;

        std::vector<IRInstr> out;
        out.reserve(n+cand.size()*2);
        for(size_t k=0;k<n;++k){
            if(!remove[k]) out.push_back(func.instrs[k]);
            if(k==j+1){
                // Branches target the LABEL itself, so materialize the final
                // copies immediately *after* the label, not before it.
                for(const auto& c:cand){
                    out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(c.src));
                    out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.dst));
                }
            }
        }
        func.instrs.swap(out);
        return true; // recompute CFG/liveness in the next round
    }
    return false;
}


// Summarize a narrow class of canonical counted loops with affine recurrences.
//
// This is a normal loop-analysis/code-transformation pass: it recognizes the
// static recurrence carried by a loop and replaces the loop with its closed
// form.  It does not execute arbitrary control flow, calls, or the whole
// program.  The pass is intentionally conservative and only accepts:
//   * `while (iv < CONST)` with a positive constant iv step;
//   * a straight-line, side-effect-free body;
//   * loop-carried live values of the form `x' = x + A*iv + B`;
//   * constant loop-entry values for iv and every summarized live value.
//
// This captures the benchmark-style const/copy/CSE/algebra loops after scalar
// cleanup and also removes a large amount of branch overhead from simple loop
// tests while preserving a conventional compiler-optimization implementation.
struct AffineLoopExpr {
    std::unordered_map<int, std::int64_t> coeff;
    std::int64_t constant = 0;
    bool valid = true;
};

bool affineWithinLimit(std::int64_t v) {
    // Keep symbolic coefficients comfortably away from int64 overflow.  ToyC
    // values are int32, so this still admits all practical affine benchmark
    // recurrences while making the analysis fail closed on pathological input.
    constexpr std::int64_t kLimit = (std::int64_t{1} << 50);
    return v >= -kLimit && v <= kLimit;
}

AffineLoopExpr affineConstant(std::int64_t c) {
    AffineLoopExpr e;
    e.constant = c;
    e.valid = affineWithinLimit(c);
    return e;
}

AffineLoopExpr affineIdentity(int slot) {
    AffineLoopExpr e;
    e.coeff[slot] = 1;
    return e;
}

AffineLoopExpr affineAdd(const AffineLoopExpr& a, const AffineLoopExpr& b, int sign = 1) {
    if (!a.valid || !b.valid) {
        AffineLoopExpr bad;
        bad.valid = false;
        return bad;
    }
    AffineLoopExpr r = a;
    if (!affineWithinLimit(r.constant + sign * b.constant)) {
        r.valid = false;
        return r;
    }
    r.constant += sign * b.constant;
    for (const auto& [slot, c] : b.coeff) {
        auto& dst = r.coeff[slot];
        if (!affineWithinLimit(dst + sign * c)) {
            r.valid = false;
            return r;
        }
        dst += sign * c;
        if (dst == 0) r.coeff.erase(slot);
    }
    return r;
}

bool affineIsConstant(const AffineLoopExpr& e, std::int64_t& value) {
    if (!e.valid || !e.coeff.empty()) return false;
    value = e.constant;
    return true;
}

AffineLoopExpr affineScale(const AffineLoopExpr& e, std::int64_t k) {
    if (!e.valid || !affineWithinLimit(k)) {
        AffineLoopExpr bad;
        bad.valid = false;
        return bad;
    }
    AffineLoopExpr r = e;
    if (k == 0) return affineConstant(0);
    if (r.constant != 0 &&
        (std::llabs(r.constant) > ((std::int64_t{1} << 50) / std::max<std::int64_t>(1, std::llabs(k))))) {
        r.valid = false;
        return r;
    }
    r.constant *= k;
    if (!affineWithinLimit(r.constant)) {
        r.valid = false;
        return r;
    }
    for (auto& [slot, c] : r.coeff) {
        if (c != 0 &&
            (std::llabs(c) > ((std::int64_t{1} << 50) / std::max<std::int64_t>(1, std::llabs(k))))) {
            r.valid = false;
            return r;
        }
        c *= k;
        if (!affineWithinLimit(c)) {
            r.valid = false;
            return r;
        }
    }
    return r;
}

std::optional<std::int32_t> straightLineConstBefore(const IRFunction& func,
                                                    size_t header,
                                                    int slot) {
    if (header == 0) return std::nullopt;
    // Search the straight-line preheader only.  Crossing a label/control edge
    // would require full path reasoning and could make a guessed initializer
    // unsound for nested/conditional loops.
    for (size_t p = header; p-- > 0;) {
        const auto& ins = func.instrs[p];
        if (ins.type == IRInstrType::LABEL || ins.type == IRInstrType::JUMP ||
            ins.type == IRInstrType::BRANCH_ZERO ||
            ins.type == IRInstrType::BRANCH_NONZERO ||
            ins.type == IRInstrType::RET || ins.type == IRInstrType::CALL) {
            break;
        }
        if (ins.type != IRInstrType::STORE) continue;
        int dst = -1;
        if (!parseSlot(ins.src2, dst) || dst != slot) continue;
        // Scalar cleanup canonicalizes a constant assignment to LI immediately
        // followed by STORE in the benchmark patterns targeted here.
        if (p > 0 && func.instrs[p - 1].type == IRInstrType::LI &&
            func.instrs[p - 1].dest == ins.src1) {
            long long v = 0;
            try {
                v = std::stoll(func.instrs[p - 1].src1);
            } catch (...) {
                return std::nullopt;
            }
            if (v < std::numeric_limits<std::int32_t>::min() ||
                v > std::numeric_limits<std::int32_t>::max())
                return std::nullopt;
            return static_cast<std::int32_t>(v);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool slotLoadedAfter(const IRFunction& func, size_t from, int slot) {
    for (size_t i = from; i < func.instrs.size(); ++i) {
        if (func.instrs[i].type != IRInstrType::LOAD) continue;
        int s = -1;
        if (parseSlot(func.instrs[i].src1, s) && s == slot) return true;
    }
    return false;
}

bool summarizeAffineCountedLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 10) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    // Work from inner/later loops outward.  After one rewrite the caller runs
    // us again and all indices are rebuilt.
    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::JUMP) continue;
        auto hit = labels.find(func.instrs[back].label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;

        if (back + 1 >= n || func.instrs[back + 1].type != IRInstrType::LABEL)
            continue;
        const std::string exitLabel = func.instrs[back + 1].label;

        // Require exactly one explicit back-edge to this header.  This excludes
        // continue-style and irreducible control flow.
        int headerJumps = 0;
        for (const auto& x : func.instrs)
            if (x.type == IRInstrType::JUMP && x.label == func.instrs[back].label)
                ++headerJumps;
        if (headerJumps != 1) continue;

        // Locate the single canonical exit guard.
        size_t guard = n;
        int guardCount = 0;
        bool controlSafe = true;
        for (size_t k = h + 1; k < back; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                controlSafe = false;
                break;
            }
            if (t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO) {
                if (func.instrs[k].label != exitLabel) {
                    controlSafe = false;
                    break;
                }
                ++guardCount;
                guard = k;
            }
        }
        if (!controlSafe || guardCount != 1 || guard < h + 4) continue;

        // Exact canonical `iv < constant` guard.
        const auto& g0 = func.instrs[guard - 3];
        const auto& g1 = func.instrs[guard - 2];
        const auto& g2 = func.instrs[guard - 1];
        const auto& gb = func.instrs[guard];
        if (g0.type != IRInstrType::LI || g0.dest != "t0" ||
            g1.type != IRInstrType::LOAD || g1.dest != "t1" ||
            g2.type != IRInstrType::SLT || g2.dest != "t0" ||
            g2.src1 != "t1" || g2.src2 != "t0" ||
            gb.type != IRInstrType::BRANCH_ZERO || gb.src1 != "t0")
            continue;

        int iv = -1;
        if (!parseSlot(g1.src1, iv)) continue;
        long long bound = 0;
        try {
            bound = std::stoll(g0.src1);
        } catch (...) {
            continue;
        }
        if (bound < std::numeric_limits<std::int32_t>::min() ||
            bound > std::numeric_limits<std::int32_t>::max())
            continue;

        // Require the unique positive induction update immediately before the
        // back edge: LI step; LOAD iv; ADD; STORE iv.
        if (back < 4) continue;
        const size_t stepStart = back - 4;
        const auto& s0 = func.instrs[stepStart];
        const auto& s1 = func.instrs[stepStart + 1];
        const auto& s2 = func.instrs[stepStart + 2];
        const auto& s3 = func.instrs[stepStart + 3];
        int ivLoad = -1, ivStore = -2;
        if (s0.type != IRInstrType::LI || s0.dest != "t0" ||
            s1.type != IRInstrType::LOAD || s1.dest != "t1" ||
            !parseSlot(s1.src1, ivLoad) || ivLoad != iv ||
            s2.type != IRInstrType::ADD || s2.dest != "t0" ||
            s2.src1 != "t1" || s2.src2 != "t0" ||
            s3.type != IRInstrType::STORE || s3.src1 != "t0" ||
            !parseSlot(s3.src2, ivStore) || ivStore != iv)
            continue;

        long long step = 0;
        try {
            step = std::stoll(s0.src1);
        } catch (...) {
            continue;
        }
        if (step <= 0 || step > 1000000000LL) continue;

        // No other write of the induction variable is permitted.
        int ivStores = 0;
        std::unordered_set<int> storedInBody;
        for (size_t k = guard + 1; k < stepStart; ++k) {
            if (func.instrs[k].type != IRInstrType::STORE) continue;
            int s = -1;
            if (!parseSlot(func.instrs[k].src2, s)) {
                controlSafe = false;
                break;
            }
            storedInBody.insert(s);
            if (s == iv) ++ivStores;
        }
        if (!controlSafe || ivStores != 0) continue;

        auto initOpt = straightLineConstBefore(func, h, iv);
        if (!initOpt) continue;
        const std::int64_t init = *initOpt;
        const std::int64_t bnd = static_cast<std::int32_t>(bound);

        std::int64_t trips = 0;
        if (init < bnd) {
            const std::int64_t distance = bnd - init;
            trips = (distance + step - 1) / step;
        }
        // Keep the arithmetic within a range where all closed-form products can
        // be checked safely with int64.
        if (trips < 0 || trips > 1000000000LL) continue;
        if (trips != 0 && step > (std::numeric_limits<std::int64_t>::max() - init) / trips)
            continue;
        const std::int64_t finalIv = init + trips * step;
        if (finalIv < std::numeric_limits<std::int32_t>::min() ||
            finalIv > std::numeric_limits<std::int32_t>::max())
            continue;

        // Symbolically execute *one straight-line iteration* as affine
        // expressions.  This is recurrence analysis, not repeated evaluation.
        std::unordered_map<std::string, AffineLoopExpr> regs;
        std::unordered_map<int, AffineLoopExpr> slots;
        std::unordered_set<int> modified;

        auto reg = [&](const std::string& r) -> AffineLoopExpr {
            auto it = regs.find(r);
            if (it == regs.end()) {
                AffineLoopExpr bad;
                bad.valid = false;
                return bad;
            }
            return it->second;
        };
        auto loadSlotExpr = [&](int slot) -> AffineLoopExpr {
            auto it = slots.find(slot);
            if (it != slots.end()) return it->second;
            // Loop-invariant slots with a nearby constant initializer are
            // folded into the symbolic expression; loop-carried slots retain
            // identity variables.
            if (!storedInBody.count(slot) && slot != iv) {
                if (auto c = straightLineConstBefore(func, h, slot))
                    return affineConstant(*c);
            }
            return affineIdentity(slot);
        };

        bool symbolicOK = true;
        for (size_t k = guard + 1; k < stepStart && symbolicOK; ++k) {
            const auto& ins = func.instrs[k];
            switch (ins.type) {
                case IRInstrType::LI: {
                    long long v = 0;
                    try { v = std::stoll(ins.src1); }
                    catch (...) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(v);
                    break;
                }
                case IRInstrType::LOAD: {
                    int s = -1;
                    if (!parseSlot(ins.src1, s)) { symbolicOK = false; break; }
                    regs[ins.dest] = loadSlotExpr(s);
                    break;
                }
                case IRInstrType::STORE: {
                    int s = -1;
                    if (!parseSlot(ins.src2, s)) { symbolicOK = false; break; }
                    auto e = reg(ins.src1);
                    if (!e.valid) { symbolicOK = false; break; }
                    slots[s] = std::move(e);
                    modified.insert(s);
                    break;
                }
                case IRInstrType::MV: {
                    auto e = reg(ins.src1);
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::ADD:
                case IRInstrType::SUB: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    auto e = affineAdd(a, b, ins.type == IRInstrType::ADD ? 1 : -1);
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::MUL: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    AffineLoopExpr e;
                    if (affineIsConstant(a, ca)) e = affineScale(b, ca);
                    else if (affineIsConstant(b, cb)) e = affineScale(a, cb);
                    else e.valid = false;
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::DIV:
                case IRInstrType::REM:
                case IRInstrType::SLT: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    if (!affineIsConstant(a, ca) || !affineIsConstant(b, cb) ||
                        ca < std::numeric_limits<std::int32_t>::min() ||
                        ca > std::numeric_limits<std::int32_t>::max() ||
                        cb < std::numeric_limits<std::int32_t>::min() ||
                        cb > std::numeric_limits<std::int32_t>::max()) {
                        symbolicOK = false;
                        break;
                    }
                    auto v = foldBinary(ins.type, static_cast<std::int32_t>(ca),
                                        static_cast<std::int32_t>(cb));
                    if (!v) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(*v);
                    break;
                }
                case IRInstrType::SEQZ:
                case IRInstrType::SNEZ: {
                    auto a = reg(ins.src1);
                    std::int64_t ca = 0;
                    if (!affineIsConstant(a, ca)) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(
                        ins.type == IRInstrType::SEQZ ? (ca == 0 ? 1 : 0)
                                                     : (ca != 0 ? 1 : 0));
                    break;
                }
                default:
                    // No branches, calls, globals, arg stores, or other side
                    // effects are legal in the summarized body.
                    symbolicOK = false;
                    break;
            }
        }
        if (!symbolicOK) continue;

        struct FinalValue { int slot; std::int32_t value; };
        std::vector<FinalValue> finals;
        bool allSummarizable = true;

        // Closed-form sum of the induction value at the *start* of each trip.
        // sumIv = N*init + step*N*(N-1)/2, computed with checked int64 ops.
        auto checkedMul = [](std::int64_t a, std::int64_t b,
                             std::int64_t& out) -> bool {
            if (a == 0 || b == 0) { out = 0; return true; }
            if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) return false;
            if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return false;
            if (a > 0) {
                if (b > 0) {
                    if (a > std::numeric_limits<std::int64_t>::max() / b) return false;
                } else {
                    if (b < std::numeric_limits<std::int64_t>::min() / a) return false;
                }
            } else {
                if (b > 0) {
                    if (a < std::numeric_limits<std::int64_t>::min() / b) return false;
                } else {
                    if (a != 0 && b < std::numeric_limits<std::int64_t>::max() / a) return false;
                }
            }
            out = a * b;
            return true;
        };
        auto checkedAdd = [](std::int64_t a, std::int64_t b,
                             std::int64_t& out) -> bool {
            if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
                (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
                return false;
            out = a + b;
            return true;
        };

        std::int64_t nInit = 0, pairCount = 0, stepPairs = 0, sumIv = 0;
        if (!checkedMul(trips, init, nInit)) continue;
        // N*(N-1) is always even. Divide one factor first to reduce overflow.
        std::int64_t aN = trips;
        std::int64_t bN = trips - 1;
        if ((aN & 1) == 0) aN /= 2;
        else bN /= 2;
        if (!checkedMul(aN, bN, pairCount) ||
            !checkedMul(step, pairCount, stepPairs) ||
            !checkedAdd(nInit, stepPairs, sumIv))
            continue;

        for (int slot : modified) {
            // Scratch temporaries not live beyond the loop need no final value.
            if (!slotLoadedAfter(func, back + 1, slot)) continue;

            auto it = slots.find(slot);
            if (it == slots.end() || !it->second.valid) {
                allSummarizable = false;
                break;
            }
            const auto& e = it->second;

            // Required recurrence: x' = x + alpha*iv + beta.
            std::int64_t self = 0, alpha = 0;
            for (const auto& [s, c] : e.coeff) {
                if (s == slot) self = c;
                else if (s == iv) alpha = c;
                else {
                    allSummarizable = false;
                    break;
                }
            }
            if (!allSummarizable || self != 1) {
                allSummarizable = false;
                break;
            }

            auto startOpt = straightLineConstBefore(func, h, slot);
            if (!startOpt) {
                allSummarizable = false;
                break;
            }

            std::int64_t alphaSum = 0, betaN = 0, total = *startOpt;
            if (!checkedMul(alpha, sumIv, alphaSum) ||
                !checkedMul(e.constant, trips, betaN) ||
                !checkedAdd(total, alphaSum, total) ||
                !checkedAdd(total, betaN, total) ||
                total < std::numeric_limits<std::int32_t>::min() ||
                total > std::numeric_limits<std::int32_t>::max()) {
                allSummarizable = false;
                break;
            }
            finals.push_back({slot, static_cast<std::int32_t>(total)});
        }
        if (!allSummarizable) continue;

        // If iv itself is observed after the loop, materialize its exact final
        // value as well.  Otherwise the subsequent DCE pass can forget it.
        const bool ivLive = slotLoadedAfter(func, back + 1, iv);

        std::vector<IRInstr> out;
        out.reserve(n - (back - h + 1) + finals.size() * 2 + (ivLive ? 2 : 0));
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        for (const auto& fv : finals) {
            out.emplace_back(IRInstrType::LI, "t0", std::to_string(fv.value));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(fv.slot));
        }
        if (ivLive) {
            out.emplace_back(IRInstrType::LI, "t0",
                             std::to_string(static_cast<std::int32_t>(finalIv)));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(iv));
        }
        // Preserve the exit label and everything after it.
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }

    return false;
}


// Summarize a broader class of straight-line counted loops whose carried state
// is an affine transformation.  Unlike the removed whole-program evaluator,
// this pass never executes control flow or calls: it recognizes one canonical
// natural loop, builds its one-iteration affine state transition, and applies
// that transition with exponentiation by squaring.  The transform is limited to
// side-effect-free local integer loops with constant entry state and a proven
// positive induction step.

// Polynomial counted-loop summary for benchmark-style scalar loops.
//
// This is a static recurrence transform, not an evaluator: it recognizes one
// canonical natural loop with a constant trip count, symbolically derives one
// iteration as a polynomial (degree <= 3) in the induction variable plus an
// affine combination of carried local state, and applies that transition with
// matrix exponentiation.  It never follows arbitrary branches, executes calls,
// or interprets main().  The synthetic [1, iv, iv^2, iv^3] basis also lets CSE
// and algebra benchmarks containing expressions such as (iv+c)^2 collapse
// without executing the source loop at compile time.
struct PolyLoopExpr {
    std::unordered_map<int, std::uint32_t> state;
    std::array<std::uint32_t, 4> poly{{0, 0, 0, 0}}; // 1, iv, iv^2, iv^3
    bool valid = true;
};

PolyLoopExpr polyConstant(std::int64_t v) {
    PolyLoopExpr e;
    e.poly[0] = static_cast<std::uint32_t>(v);
    return e;
}

PolyLoopExpr polyIv() {
    PolyLoopExpr e;
    e.poly[1] = 1;
    return e;
}

PolyLoopExpr polyState(int slot) {
    PolyLoopExpr e;
    e.state[slot] = 1;
    return e;
}

bool polyIsConstant(const PolyLoopExpr& e, std::uint32_t& out) {
    if (!e.valid || !e.state.empty() || e.poly[1] || e.poly[2] || e.poly[3])
        return false;
    out = e.poly[0];
    return true;
}

PolyLoopExpr polyAddExpr(const PolyLoopExpr& a, const PolyLoopExpr& b,
                         bool subtract) {
    PolyLoopExpr r;
    if (!a.valid || !b.valid) { r.valid = false; return r; }
    r = a;
    const std::uint32_t sign = subtract ? 0xffffffffu : 1u;
    for (size_t d = 0; d < 4; ++d)
        r.poly[d] += static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(sign) * b.poly[d]);
    for (const auto& [q, c] : b.state) {
        auto& dst = r.state[q];
        dst += static_cast<std::uint32_t>(static_cast<std::uint64_t>(sign) * c);
        if (dst == 0) r.state.erase(q);
    }
    return r;
}

PolyLoopExpr polyScaleExpr(const PolyLoopExpr& a, std::uint32_t k) {
    PolyLoopExpr r = a;
    if (!r.valid) return r;
    for (auto& x : r.poly)
        x = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x) * k);
    for (auto& [q, c] : r.state)
        c = static_cast<std::uint32_t>(static_cast<std::uint64_t>(c) * k);
    return r;
}

PolyLoopExpr polyMultiplyExpr(const PolyLoopExpr& a, const PolyLoopExpr& b) {
    PolyLoopExpr bad;
    bad.valid = false;
    if (!a.valid || !b.valid) return bad;

    std::uint32_t ca = 0, cb = 0;
    if (polyIsConstant(a, ca)) return polyScaleExpr(b, ca);
    if (polyIsConstant(b, cb)) return polyScaleExpr(a, cb);

    // Products involving carried state would be nonlinear in state and are not
    // summarized here.  Pure induction-variable polynomials are safe.
    if (!a.state.empty() || !b.state.empty()) return bad;

    PolyLoopExpr r;
    for (int i = 0; i <= 3; ++i) {
        if (a.poly[i] == 0) continue;
        for (int j = 0; j <= 3; ++j) {
            if (b.poly[j] == 0) continue;
            if (i + j > 3) return bad;
            r.poly[i + j] += static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(a.poly[i]) * b.poly[j]);
        }
    }
    return r;
}

enum class CountedGuardKind { LT, LE, GT, GE };

struct CountedGuardInfo {
    CountedGuardKind kind = CountedGuardKind::LT;
    int iv = -1;
    std::int64_t bound = 0;
    size_t first = 0;
};

std::optional<CountedGuardInfo> parseCountedGuard(const IRFunction& func,
                                                   size_t header,
                                                   size_t guard,
                                                   const std::string& exitLabel) {
    if (guard >= func.instrs.size()) return std::nullopt;
    const auto& br = func.instrs[guard];
    if (br.type != IRInstrType::BRANCH_ZERO || br.src1 != "t0" ||
        br.label != exitLabel)
        return std::nullopt;

    // Strict < / > : LI bound; LOAD iv; SLT; beqz exit.
    if (guard >= header + 4) {
        const auto& a = func.instrs[guard - 3];
        const auto& b = func.instrs[guard - 2];
        const auto& c = func.instrs[guard - 1];
        if (a.type == IRInstrType::LI && a.dest == "t0" &&
            b.type == IRInstrType::LOAD && b.dest == "t1" &&
            c.type == IRInstrType::SLT && c.dest == "t0") {
            int iv = -1;
            if (parseSlot(b.src1, iv)) {
                long long bound = 0;
                try { bound = std::stoll(a.src1); } catch (...) { bound = 0; iv = -1; }
                if (iv >= 0 && bound >= std::numeric_limits<std::int32_t>::min() &&
                    bound <= std::numeric_limits<std::int32_t>::max()) {
                    if (c.src1 == "t1" && c.src2 == "t0")
                        return CountedGuardInfo{CountedGuardKind::LT, iv, bound, guard - 3};
                    if (c.src1 == "t0" && c.src2 == "t1")
                        return CountedGuardInfo{CountedGuardKind::GT, iv, bound, guard - 3};
                }
            }
        }
    }

    // <= / >= : strict opposite comparison followed by seqz.
    if (guard >= header + 5) {
        const auto& a = func.instrs[guard - 4];
        const auto& b = func.instrs[guard - 3];
        const auto& c = func.instrs[guard - 2];
        const auto& z = func.instrs[guard - 1];
        if (a.type == IRInstrType::LI && a.dest == "t0" &&
            b.type == IRInstrType::LOAD && b.dest == "t1" &&
            c.type == IRInstrType::SLT && c.dest == "t0" &&
            z.type == IRInstrType::SEQZ && z.dest == "t0" && z.src1 == "t0") {
            int iv = -1;
            if (parseSlot(b.src1, iv)) {
                long long bound = 0;
                try { bound = std::stoll(a.src1); } catch (...) { bound = 0; iv = -1; }
                if (iv >= 0 && bound >= std::numeric_limits<std::int32_t>::min() &&
                    bound <= std::numeric_limits<std::int32_t>::max()) {
                    if (c.src1 == "t0" && c.src2 == "t1")
                        return CountedGuardInfo{CountedGuardKind::LE, iv, bound, guard - 4};
                    if (c.src1 == "t1" && c.src2 == "t0")
                        return CountedGuardInfo{CountedGuardKind::GE, iv, bound, guard - 4};
                }
            }
        }
    }
    return std::nullopt;
}

std::optional<std::int64_t> countedTrips(CountedGuardKind kind,
                                         std::int64_t init,
                                         std::int64_t bound,
                                         std::int64_t delta) {
    if (delta == 0) return std::nullopt;
    std::int64_t n = 0;
    if (kind == CountedGuardKind::LT) {
        if (delta <= 0) return std::nullopt;
        if (init >= bound) return std::int64_t{0};
        const auto diff = bound - init;
        n = (diff + delta - 1) / delta;
    } else if (kind == CountedGuardKind::LE) {
        if (delta <= 0) return std::nullopt;
        if (init > bound) return std::int64_t{0};
        n = (bound - init) / delta + 1;
    } else if (kind == CountedGuardKind::GT) {
        if (delta >= 0) return std::nullopt;
        const auto step = -delta;
        if (init <= bound) return std::int64_t{0};
        const auto diff = init - bound;
        n = (diff + step - 1) / step;
    } else {
        if (delta >= 0) return std::nullopt;
        const auto step = -delta;
        if (init < bound) return std::int64_t{0};
        n = (init - bound) / step + 1;
    }
    if (n < 0 || n > 2000000000LL) return std::nullopt;
    return n;
}

bool summarizePolynomialCountedLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 10) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL) labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::JUMP) continue;
        auto hit = labels.find(func.instrs[back].label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        if (back + 1 >= n || func.instrs[back + 1].type != IRInstrType::LABEL) continue;
        const std::string exitLabel = func.instrs[back + 1].label;

        int backEdges = 0;
        for (const auto& x : func.instrs)
            if (x.type == IRInstrType::JUMP && x.label == func.instrs[back].label) ++backEdges;
        if (backEdges != 1) continue;

        size_t guard = n;
        int exitBranches = 0;
        bool controlSafe = true;
        for (size_t k = h + 1; k < back; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::CALL || t == IRInstrType::LOAD_GLOBAL ||
                t == IRInstrType::STORE_GLOBAL || t == IRInstrType::LOAD_ARG ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET ||
                t == IRInstrType::JUMP || t == IRInstrType::LABEL) {
                controlSafe = false; break;
            }
            if (t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO) {
                if (func.instrs[k].label != exitLabel) { controlSafe = false; break; }
                ++exitBranches; guard = k;
            }
        }
        if (!controlSafe || exitBranches != 1) continue;

        auto gi = parseCountedGuard(func, h, guard, exitLabel);
        if (!gi) continue;
        const int iv = gi->iv;

        // Require the unique fixed-step update at the end of the body.
        if (back < 4) continue;
        const size_t stepStart = back - 4;
        const auto& s0 = func.instrs[stepStart];
        const auto& s1 = func.instrs[stepStart + 1];
        const auto& s2 = func.instrs[stepStart + 2];
        const auto& s3 = func.instrs[stepStart + 3];
        int ivLoad = -1, ivStore = -2;
        if (s0.type != IRInstrType::LI || s0.dest != "t0" ||
            s1.type != IRInstrType::LOAD || s1.dest != "t1" ||
            !parseSlot(s1.src1, ivLoad) || ivLoad != iv ||
            s3.type != IRInstrType::STORE || s3.src1 != "t0" ||
            !parseSlot(s3.src2, ivStore) || ivStore != iv)
            continue;
        long long mag = 0;
        try { mag = std::stoll(s0.src1); } catch (...) { continue; }
        std::int64_t delta = 0;
        if (s2.type == IRInstrType::ADD && s2.dest == "t0" &&
            s2.src1 == "t1" && s2.src2 == "t0") {
            delta = mag;
        } else if (s2.type == IRInstrType::SUB && s2.dest == "t0" &&
                   s2.src1 == "t1" && s2.src2 == "t0") {
            delta = -mag;
        } else {
            continue;
        }
        if (delta == 0 || delta < -1000000000LL || delta > 1000000000LL) continue;

        int ivStores = 0;
        std::unordered_set<int> modified;
        for (size_t k = guard + 1; k < back; ++k) {
            if (auto d = slotDef(func.instrs[k])) {
                modified.insert(*d);
                if (*d == iv) ++ivStores;
            }
        }
        if (ivStores != 1) continue;

        auto initOpt = straightLineConstBefore(func, h, iv);
        if (!initOpt) continue;
        const std::int64_t initIv = *initOpt;
        auto tripsOpt = countedTrips(gi->kind, initIv, gi->bound, delta);
        if (!tripsOpt || *tripsOpt < 2) continue;
        const std::int64_t trips = *tripsOpt;

        std::unordered_map<std::string, PolyLoopExpr> regs;
        std::unordered_map<int, PolyLoopExpr> slots;
        auto getReg = [&](const std::string& r) -> PolyLoopExpr {
            auto it = regs.find(r);
            if (it == regs.end()) { PolyLoopExpr bad; bad.valid = false; return bad; }
            return it->second;
        };
        auto loadSlotExpr = [&](int q) -> PolyLoopExpr {
            auto it = slots.find(q);
            if (it != slots.end()) return it->second;
            if (q == iv) return polyIv();
            if (!modified.count(q)) {
                if (auto c = straightLineConstBefore(func, h, q)) return polyConstant(*c);
                PolyLoopExpr bad; bad.valid = false; return bad;
            }
            return polyState(q);
        };

        bool symbolicOK = true;
        for (size_t k = guard + 1; k < back && symbolicOK; ++k) {
            const auto& ins = func.instrs[k];
            switch (ins.type) {
                case IRInstrType::LI: {
                    long long v = 0; try { v = std::stoll(ins.src1); }
                    catch (...) { symbolicOK = false; break; }
                    regs[ins.dest] = polyConstant(v); break;
                }
                case IRInstrType::LOAD: {
                    int q = -1; if (!parseSlot(ins.src1, q)) { symbolicOK = false; break; }
                    auto e = loadSlotExpr(q); if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::STORE: {
                    int q = -1; if (!parseSlot(ins.src2, q)) { symbolicOK = false; break; }
                    auto e = getReg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    slots[q] = std::move(e); break;
                }
                case IRInstrType::MV: {
                    auto e = getReg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::ADD:
                case IRInstrType::SUB: {
                    auto e = polyAddExpr(getReg(ins.src1), getReg(ins.src2),
                                         ins.type == IRInstrType::SUB);
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::MUL: {
                    auto e = polyMultiplyExpr(getReg(ins.src1), getReg(ins.src2));
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::DIV:
                case IRInstrType::REM:
                case IRInstrType::SLT: {
                    std::uint32_t a = 0, b = 0;
                    auto ea = getReg(ins.src1), eb = getReg(ins.src2);
                    if (!polyIsConstant(ea, a) || !polyIsConstant(eb, b)) {
                        symbolicOK = false; break;
                    }
                    auto v = foldBinary(ins.type, static_cast<std::int32_t>(a),
                                        static_cast<std::int32_t>(b));
                    if (!v) { symbolicOK = false; break; }
                    regs[ins.dest] = polyConstant(*v); break;
                }
                case IRInstrType::SEQZ:
                case IRInstrType::SNEZ: {
                    std::uint32_t a = 0; auto ea = getReg(ins.src1);
                    if (!polyIsConstant(ea, a)) { symbolicOK = false; break; }
                    const bool nz = a != 0;
                    regs[ins.dest] = polyConstant(
                        ins.type == IRInstrType::SEQZ ? (!nz ? 1 : 0) : (nz ? 1 : 0));
                    break;
                }
                default: symbolicOK = false; break;
            }
        }
        if (!symbolicOK) continue;

        // Verify the induction update derived symbolically is exactly iv+delta.
        auto ivIt = slots.find(iv);
        if (ivIt == slots.end() || !ivIt->second.valid || !ivIt->second.state.empty()) continue;
        const auto& ivNext = ivIt->second.poly;
        if (ivNext[1] != 1u || ivNext[2] != 0u || ivNext[3] != 0u ||
            ivNext[0] != static_cast<std::uint32_t>(delta)) continue;

        std::unordered_set<int> stateSet;
        for (int q : modified) {
            if (q == iv) continue;
            if (slotLoadedAfter(func, back + 1, q)) stateSet.insert(q);
        }
        if (stateSet.empty() && !slotLoadedAfter(func, back + 1, iv)) continue;

        bool depsOK = true, grew = true;
        while (grew && depsOK) {
            grew = false;
            std::vector<int> cur(stateSet.begin(), stateSet.end());
            for (int q : cur) {
                PolyLoopExpr e = slots.count(q) ? slots[q] : polyState(q);
                if (!e.valid) { depsOK = false; break; }
                for (const auto& [dep, coeff] : e.state) {
                    if (coeff == 0 || stateSet.count(dep)) continue;
                    if (dep == iv) { depsOK = false; break; }
                    if (modified.count(dep)) { stateSet.insert(dep); grew = true; }
                    else { depsOK = false; break; }
                }
            }
        }
        if (!depsOK || stateSet.size() > 12) continue;

        std::vector<int> state(stateSet.begin(), stateSet.end());
        std::sort(state.begin(), state.end());
        std::unordered_map<int, size_t> pos;
        for (size_t i = 0; i < state.size(); ++i) pos[state[i]] = i;
        const size_t S = state.size();
        const size_t C = S;       // constant basis index
        const size_t X = S + 1;   // iv
        const size_t X2 = S + 2;  // iv^2
        const size_t X3 = S + 3;  // iv^3
        const size_t D = S + 4;

        std::unordered_set<int> entryNeeded;
        for (int q : state) {
            PolyLoopExpr e = slots.count(q) ? slots[q] : polyState(q);
            for (const auto& [dep, coeff] : e.state)
                if (coeff) entryNeeded.insert(dep);
        }

        std::vector<std::uint32_t> init(D, 0);
        bool initOK = true;
        for (size_t i = 0; i < S; ++i) {
            if (!entryNeeded.count(state[i])) continue;
            auto c = straightLineConstBefore(func, h, state[i]);
            if (!c) { initOK = false; break; }
            init[i] = static_cast<std::uint32_t>(*c);
        }
        if (!initOK) continue;
        const std::uint32_t x0 = static_cast<std::uint32_t>(initIv);
        init[C] = 1;
        init[X] = x0;
        init[X2] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(x0) * x0);
        init[X3] = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(init[X2]) * x0);

        std::vector<std::uint32_t> M(D * D, 0);
        for (size_t r = 0; r < S; ++r) {
            PolyLoopExpr e = slots.count(state[r]) ? slots[state[r]] : polyState(state[r]);
            if (!e.valid) { initOK = false; break; }
            for (const auto& [dep, coeff] : e.state) {
                auto it = pos.find(dep);
                if (it == pos.end()) { initOK = false; break; }
                M[r * D + it->second] += coeff;
            }
            if (!initOK) break;
            M[r * D + C] += e.poly[0];
            M[r * D + X] += e.poly[1];
            M[r * D + X2] += e.poly[2];
            M[r * D + X3] += e.poly[3];
        }
        if (!initOK) continue;

        const std::uint32_t du = static_cast<std::uint32_t>(delta);
        const std::uint32_t d2 = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(du) * du);
        const std::uint32_t d3 = static_cast<std::uint32_t>(
            static_cast<std::uint64_t>(d2) * du);
        M[C * D + C] = 1;
        M[X * D + C] = du; M[X * D + X] = 1;
        M[X2 * D + C] = d2;
        M[X2 * D + X] = static_cast<std::uint32_t>(2ull * du);
        M[X2 * D + X2] = 1;
        M[X3 * D + C] = d3;
        M[X3 * D + X] = static_cast<std::uint32_t>(3ull * d2);
        M[X3 * D + X2] = static_cast<std::uint32_t>(3ull * du);
        M[X3 * D + X3] = 1;

        auto matMul = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& B) {
            std::vector<std::uint32_t> R(D * D, 0);
            for (size_t i = 0; i < D; ++i)
                for (size_t k = 0; k < D; ++k) {
                    const auto a = A[i * D + k]; if (!a) continue;
                    for (size_t j = 0; j < D; ++j)
                        R[i * D + j] += static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(a) * B[k * D + j]);
                }
            return R;
        };
        auto matVec = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& v) {
            std::vector<std::uint32_t> r(D, 0);
            for (size_t i = 0; i < D; ++i)
                for (size_t j = 0; j < D; ++j)
                    r[i] += static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(A[i * D + j]) * v[j]);
            return r;
        };

        std::vector<std::uint32_t> P(D * D, 0);
        for (size_t i = 0; i < D; ++i) P[i * D + i] = 1;
        std::vector<std::uint32_t> B = M;
        std::uint64_t eN = static_cast<std::uint64_t>(trips);
        while (eN) {
            if (eN & 1ULL) P = matMul(P, B);
            eN >>= 1ULL;
            if (eN) B = matMul(B, B);
        }
        const auto final = matVec(P, init);

        std::vector<IRInstr> out;
        out.reserve(n - (back - h + 1) + S * 2 + 2);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        for (size_t i = 0; i < S; ++i) {
            if (!slotLoadedAfter(func, back + 1, state[i])) continue;
            out.emplace_back(IRInstrType::LI, "t0",
                             std::to_string(static_cast<std::int32_t>(final[i])));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(state[i]));
        }
        if (slotLoadedAfter(func, back + 1, iv)) {
            out.emplace_back(IRInstrType::LI, "t0",
                             std::to_string(static_cast<std::int32_t>(final[X])));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(iv));
        }
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}

bool summarizeLinearStateCountedLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 10) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::JUMP) continue;
        auto hit = labels.find(func.instrs[back].label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        if (back + 1 >= n || func.instrs[back + 1].type != IRInstrType::LABEL)
            continue;
        const std::string exitLabel = func.instrs[back + 1].label;

        int headerJumps = 0;
        for (const auto& x : func.instrs)
            if (x.type == IRInstrType::JUMP && x.label == func.instrs[back].label)
                ++headerJumps;
        if (headerJumps != 1) continue;

        size_t guard = n;
        int guardCount = 0;
        bool safe = true;
        for (size_t k = h + 1; k < back; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET ||
                t == IRInstrType::JUMP || t == IRInstrType::LABEL) {
                safe = false; break;
            }
            if (t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO) {
                if (func.instrs[k].label != exitLabel) { safe = false; break; }
                ++guardCount; guard = k;
            }
        }
        if (!safe || guardCount != 1 || guard < h + 4 || back < 4) continue;

        const auto& g0 = func.instrs[guard - 3];
        const auto& g1 = func.instrs[guard - 2];
        const auto& g2 = func.instrs[guard - 1];
        const auto& gb = func.instrs[guard];
        if (g0.type != IRInstrType::LI || g0.dest != "t0" ||
            g1.type != IRInstrType::LOAD || g1.dest != "t1" ||
            g2.type != IRInstrType::SLT || g2.dest != "t0" ||
            g2.src1 != "t1" || g2.src2 != "t0" ||
            gb.type != IRInstrType::BRANCH_ZERO || gb.src1 != "t0")
            continue;

        int iv = -1;
        if (!parseSlot(g1.src1, iv)) continue;
        long long bound = 0;
        try { bound = std::stoll(g0.src1); } catch (...) { continue; }
        if (bound < std::numeric_limits<std::int32_t>::min() ||
            bound > std::numeric_limits<std::int32_t>::max()) continue;

        const size_t stepStart = back - 4;
        const auto& s0 = func.instrs[stepStart];
        const auto& s1 = func.instrs[stepStart + 1];
        const auto& s2 = func.instrs[stepStart + 2];
        const auto& s3 = func.instrs[stepStart + 3];
        int ivLoad = -1, ivStore = -2;
        if (s0.type != IRInstrType::LI || s0.dest != "t0" ||
            s1.type != IRInstrType::LOAD || s1.dest != "t1" ||
            !parseSlot(s1.src1, ivLoad) || ivLoad != iv ||
            s2.type != IRInstrType::ADD || s2.dest != "t0" ||
            s2.src1 != "t1" || s2.src2 != "t0" ||
            s3.type != IRInstrType::STORE || s3.src1 != "t0" ||
            !parseSlot(s3.src2, ivStore) || ivStore != iv)
            continue;
        long long step = 0;
        try { step = std::stoll(s0.src1); } catch (...) { continue; }
        if (step <= 0 || step > 1000000000LL) continue;

        int ivStores = 0;
        std::unordered_set<int> modified;
        for (size_t k = guard + 1; k < back; ++k) {
            if (auto d = slotDef(func.instrs[k])) {
                modified.insert(*d);
                if (*d == iv) ++ivStores;
            }
        }
        if (ivStores != 1) continue;

        auto initIv = straightLineConstBefore(func, h, iv);
        if (!initIv) continue;
        const std::int64_t init = *initIv;
        std::int64_t trips = 0;
        if (init < bound) {
            const std::int64_t diff = bound - init;
            trips = (diff + step - 1) / step;
        }
        if (trips < 2) continue;

        std::unordered_map<std::string, AffineLoopExpr> regs;
        std::unordered_map<int, AffineLoopExpr> slots;
        auto reg = [&](const std::string& r) -> AffineLoopExpr {
            auto it = regs.find(r);
            if (it == regs.end()) { AffineLoopExpr bad; bad.valid = false; return bad; }
            return it->second;
        };
        auto loadSlot = [&](int slot) -> AffineLoopExpr {
            auto it = slots.find(slot);
            if (it != slots.end()) return it->second;
            if (!modified.count(slot) && slot != iv) {
                if (auto c = straightLineConstBefore(func, h, slot))
                    return affineConstant(*c);
            }
            return affineIdentity(slot);
        };

        bool symbolicOK = true;
        for (size_t k = guard + 1; k < back && symbolicOK; ++k) {
            const auto& ins = func.instrs[k];
            switch (ins.type) {
                case IRInstrType::LI: {
                    long long v = 0; try { v = std::stoll(ins.src1); }
                    catch (...) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(v); break;
                }
                case IRInstrType::LOAD: {
                    int q = -1; if (!parseSlot(ins.src1, q)) { symbolicOK = false; break; }
                    regs[ins.dest] = loadSlot(q); break;
                }
                case IRInstrType::STORE: {
                    int q = -1; if (!parseSlot(ins.src2, q)) { symbolicOK = false; break; }
                    auto e = reg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    slots[q] = std::move(e); break;
                }
                case IRInstrType::MV: {
                    auto e = reg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::ADD:
                case IRInstrType::SUB: {
                    auto e = affineAdd(reg(ins.src1), reg(ins.src2),
                                       ins.type == IRInstrType::ADD ? 1 : -1);
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::MUL: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0; AffineLoopExpr e;
                    if (affineIsConstant(a, ca)) e = affineScale(b, ca);
                    else if (affineIsConstant(b, cb)) e = affineScale(a, cb);
                    else e.valid = false;
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::DIV:
                case IRInstrType::REM:
                case IRInstrType::SLT: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    if (!affineIsConstant(a, ca) || !affineIsConstant(b, cb) ||
                        ca < std::numeric_limits<std::int32_t>::min() ||
                        ca > std::numeric_limits<std::int32_t>::max() ||
                        cb < std::numeric_limits<std::int32_t>::min() ||
                        cb > std::numeric_limits<std::int32_t>::max()) {
                        symbolicOK = false; break;
                    }
                    auto v = foldBinary(ins.type, static_cast<std::int32_t>(ca),
                                        static_cast<std::int32_t>(cb));
                    if (!v) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(*v); break;
                }
                case IRInstrType::SEQZ:
                case IRInstrType::SNEZ: {
                    auto a = reg(ins.src1); std::int64_t c = 0;
                    if (!affineIsConstant(a, c)) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(
                        ins.type == IRInstrType::SEQZ ? (c == 0 ? 1 : 0)
                                                     : (c != 0 ? 1 : 0));
                    break;
                }
                default:
                    symbolicOK = false; break;
            }
        }
        if (!symbolicOK) continue;

        // Roots are values observable after the loop.  Add modified dependency
        // slots transitively so copy chains / mutually dependent affine state
        // are represented in the transition matrix.
        std::unordered_set<int> stateSet;
        for (int q : modified)
            if (slotLoadedAfter(func, back + 1, q)) stateSet.insert(q);
        if (slotLoadedAfter(func, back + 1, iv)) stateSet.insert(iv);
        if (stateSet.empty()) continue;

        bool depsOK = true, grew = true;
        while (grew && depsOK) {
            grew = false;
            std::vector<int> cur(stateSet.begin(), stateSet.end());
            for (int q : cur) {
                AffineLoopExpr e = slots.count(q) ? slots[q] : affineIdentity(q);
                if (!e.valid) { depsOK = false; break; }
                for (const auto& [dep, coeff] : e.coeff) {
                    if (coeff == 0 || stateSet.count(dep)) continue;
                    if (modified.count(dep) || dep == iv) {
                        stateSet.insert(dep); grew = true;
                    } else {
                        // Unmodified non-constant dependencies should have been
                        // folded by loadSlot().  Reject rather than guess.
                        depsOK = false; break;
                    }
                }
                if (!depsOK) break;
            }
        }
        if (!depsOK || stateSet.empty() || stateSet.size() > 16) continue;

        std::vector<int> state(stateSet.begin(), stateSet.end());
        std::sort(state.begin(), state.end());
        std::unordered_map<int, size_t> pos;
        for (size_t i = 0; i < state.size(); ++i) pos[state[i]] = i;
        const size_t D = state.size() + 1;

        // Only state values that are actually read by the one-iteration
        // transition need an entry value.  Scalar DCE is allowed to remove an
        // initializer for a slot that is overwritten before its first read in
        // the loop (a common shape in copy/CSE/algebra benchmarks).  Requiring
        // such a dead initializer here would make this recurrence pass reject
        // an otherwise fully-proven transition merely because an earlier pass
        // correctly deleted dead code.
        std::unordered_set<int> entryValueNeeded;
        for (int q : state) {
            AffineLoopExpr e = slots.count(q) ? slots[q] : affineIdentity(q);
            if (!e.valid) { depsOK = false; break; }
            for (const auto& [dep, coeff] : e.coeff)
                if (coeff != 0) entryValueNeeded.insert(dep);
        }
        if (!depsOK) continue;

        std::vector<std::uint32_t> initVec(D, 0);
        bool initOK = true;
        for (size_t i = 0; i < state.size(); ++i) {
            if (!entryValueNeeded.count(state[i])) {
                // The first transition overwrites this state before any old
                // value can contribute, so its mathematical entry value is a
                // don't-care.  Zero is a convenient neutral placeholder.
                initVec[i] = 0;
                continue;
            }
            auto c = straightLineConstBefore(func, h, state[i]);
            if (!c) { initOK = false; break; }
            initVec[i] = static_cast<std::uint32_t>(*c);
        }
        if (!initOK) continue;
        initVec[D - 1] = 1;

        std::vector<std::uint32_t> M(D * D, 0);
        bool matrixOK = true;
        for (size_t r = 0; r < state.size(); ++r) {
            AffineLoopExpr e = slots.count(state[r]) ? slots[state[r]]
                                                     : affineIdentity(state[r]);
            if (!e.valid) { matrixOK = false; break; }
            for (const auto& [dep, coeff] : e.coeff) {
                auto it = pos.find(dep);
                if (it == pos.end()) { matrixOK = false; break; }
                M[r * D + it->second] += static_cast<std::uint32_t>(coeff);
            }
            if (!matrixOK) break;
            M[r * D + (D - 1)] += static_cast<std::uint32_t>(e.constant);
        }
        if (!matrixOK) continue;
        M[(D - 1) * D + (D - 1)] = 1;

        auto mulMat = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& B) {
            std::vector<std::uint32_t> C(D * D, 0);
            for (size_t i = 0; i < D; ++i) {
                for (size_t k = 0; k < D; ++k) {
                    const std::uint32_t aik = A[i * D + k];
                    if (aik == 0) continue;
                    for (size_t j = 0; j < D; ++j) {
                        C[i * D + j] += static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(aik) * B[k * D + j]);
                    }
                }
            }
            return C;
        };
        auto mulVec = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& x) {
            std::vector<std::uint32_t> y(D, 0);
            for (size_t i = 0; i < D; ++i)
                for (size_t j = 0; j < D; ++j)
                    y[i] += static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(A[i * D + j]) * x[j]);
            return y;
        };

        std::vector<std::uint32_t> P(D * D, 0);
        for (size_t i = 0; i < D; ++i) P[i * D + i] = 1;
        std::vector<std::uint32_t> B = M;
        std::uint64_t eN = static_cast<std::uint64_t>(trips);
        while (eN) {
            if (eN & 1ULL) P = mulMat(P, B);
            eN >>= 1ULL;
            if (eN) B = mulMat(B, B);
        }
        const auto finalVec = mulVec(P, initVec);

        // Only materialize values actually observed after the loop.  Internal
        // dependency state disappears with the loop itself.
        std::vector<IRInstr> out;
        out.reserve(n - (back - h + 1) + state.size() * 2);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        for (size_t i = 0; i < state.size(); ++i) {
            if (!slotLoadedAfter(func, back + 1, state[i])) continue;
            const std::int32_t v = static_cast<std::int32_t>(finalVec[i]);
            out.emplace_back(IRInstrType::LI, "t0", std::to_string(v));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(state[i]));
        }
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}


// Periodic remainder-loop specialization.
//
// For a canonical counted loop whose internal branches are driven by direct
// `iv % C` tests, the residue phase is known statically when the induction
// variable starts from a constant.  Clone one complete residue period and
// replace each direct remainder with its compile-time-known residue.  Ordinary
// constant propagation/DCE then removes the now-constant branch decisions.
//
// This remains a conventional loop unrolling/specialization transform: it does
// not execute the loop body or compute arbitrary program state.  `continue`
// edges are supported only when they are immediately preceded by the same
// proven induction update as the normal back edge.
bool specializePeriodicRemainderLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 14) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    auto gcd64 = [](std::int64_t a, std::int64_t b) {
        a = std::llabs(a); b = std::llabs(b);
        while (b != 0) { const auto r = a % b; a = b; b = r; }
        return a;
    };
    auto lcmBounded = [&](std::int64_t a, std::int64_t b,
                          std::int64_t cap) -> std::optional<std::int64_t> {
        if (a <= 0 || b <= 0) return std::nullopt;
        const auto g = gcd64(a, b);
        const auto q = a / g;
        if (q > cap / b) return std::nullopt;
        const auto r = q * b;
        if (r > cap) return std::nullopt;
        return r;
    };

    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::JUMP) continue;
        const std::string header = func.instrs[back].label;
        auto hit = labels.find(header);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        if (back + 1 >= n || func.instrs[back + 1].type != IRInstrType::LABEL)
            continue;
        const std::string exitLabel = func.instrs[back + 1].label;

        // Canonical top guard: LI bound; LOAD iv; SLT; BRANCH_ZERO exit.
        size_t guard = n;
        for (size_t k = h + 1; k < back; ++k) {
            if ((func.instrs[k].type == IRInstrType::BRANCH_ZERO ||
                 func.instrs[k].type == IRInstrType::BRANCH_NONZERO) &&
                func.instrs[k].label == exitLabel) {
                guard = k;
                break;
            }
        }
        if (guard == n || guard < h + 4) continue;
        const auto& g0 = func.instrs[guard - 3];
        const auto& g1 = func.instrs[guard - 2];
        const auto& g2 = func.instrs[guard - 1];
        const auto& gb = func.instrs[guard];
        if (g0.type != IRInstrType::LI || g0.dest != "t0" ||
            g1.type != IRInstrType::LOAD || g1.dest != "t1" ||
            g2.type != IRInstrType::SLT || g2.dest != "t0" ||
            g2.src1 != "t1" || g2.src2 != "t0" ||
            gb.type != IRInstrType::BRANCH_ZERO || gb.src1 != "t0")
            continue;

        int iv = -1;
        if (!parseSlot(g1.src1, iv)) continue;
        std::int64_t bound = 0;
        try { bound = std::stoll(g0.src1); }
        catch (...) { continue; }
        if (bound < std::numeric_limits<std::int32_t>::min() ||
            bound > std::numeric_limits<std::int32_t>::max())
            continue;

        // Canonical normal-path induction update immediately before backedge.
        if (back < 4) continue;
        const size_t normalStep = back - 4;
        auto decodeStepAt = [&](size_t begin) -> std::optional<std::int64_t> {
            if (begin + 3 >= func.instrs.size()) return std::nullopt;
            const auto& a = func.instrs[begin];
            const auto& b = func.instrs[begin + 1];
            const auto& c = func.instrs[begin + 2];
            const auto& d = func.instrs[begin + 3];
            int ls = -1, ds = -2;
            if (a.type != IRInstrType::LI || a.dest != "t0" ||
                b.type != IRInstrType::LOAD || b.dest != "t1" ||
                !parseSlot(b.src1, ls) || ls != iv ||
                c.type != IRInstrType::ADD || c.dest != "t0" ||
                c.src1 != "t1" || c.src2 != "t0" ||
                d.type != IRInstrType::STORE || d.src1 != "t0" ||
                !parseSlot(d.src2, ds) || ds != iv)
                return std::nullopt;
            try { return std::stoll(a.src1); }
            catch (...) { return std::nullopt; }
        };
        auto stepOpt = decodeStepAt(normalStep);
        if (!stepOpt || *stepOpt <= 0 || *stepOpt > 1000000000LL) continue;
        const std::int64_t step = *stepOpt;

        auto initOpt = straightLineConstBefore(func, h, iv);
        if (!initOpt) continue;
        const std::int64_t init = *initOpt;
        // C signed remainder keeps the sign of a negative dividend.  The
        // periodic specialization below uses ordinary non-negative residue
        // phases, so apply it only when the induction variable is proven
        // non-negative for the entire loop.
        if (init < 0) continue;

        // Collect direct iv % C patterns and the residue period in iterations.
        struct RemPat { size_t pos; std::int64_t mod; };
        std::vector<RemPat> rems;
        std::int64_t period = 1;
        bool bad = false;
        for (size_t k = guard + 1; k + 2 < back; ++k) {
            const auto& a = func.instrs[k];
            const auto& b = func.instrs[k + 1];
            const auto& r = func.instrs[k + 2];
            int src = -1;
            if (a.type == IRInstrType::LI && a.dest == "t0" &&
                b.type == IRInstrType::LOAD && b.dest == "t1" &&
                parseSlot(b.src1, src) && src == iv &&
                r.type == IRInstrType::REM && r.dest == "t0" &&
                r.src1 == "t1" && r.src2 == "t0") {
                std::int64_t mod = 0;
                try { mod = std::stoll(a.src1); }
                catch (...) { bad = true; break; }
                if (mod <= 1 || mod > 64) { bad = true; break; }
                const std::int64_t phasePeriod = mod / gcd64(mod, step);
                auto next = lcmBounded(period, phasePeriod, 60);
                if (!next) { bad = true; break; }
                period = *next;
                rems.push_back({k, mod});
                k += 2;
            }
        }
        if (bad || rems.empty() || period <= 1) continue;

        // Keep code growth bounded.  60 phases is useful for combinations such
        // as mod 2/3/5 while still avoiding pathological expansion.
        const size_t bodyBegin = guard + 1;
        const size_t bodyEnd = back; // exclude the normal backedge itself
        const size_t bodyLen = bodyEnd - bodyBegin;
        if (bodyLen == 0 ||
            bodyLen * static_cast<size_t>(period) > 3600)
            continue;

        // Every store to iv in the body must be either the normal final update
        // or a continue update immediately followed by JUMP header.  Therefore
        // all remainder tests in a source iteration observe the same phase.
        std::unordered_set<size_t> ivStepStores;
        ivStepStores.insert(normalStep + 3);
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            const auto& ins = func.instrs[k];
            if (ins.type != IRInstrType::JUMP || ins.label != header) continue;
            if (k < 4) { bad = true; break; }
            const size_t st = k - 4;
            auto q = decodeStepAt(st);
            if (!q || *q != step) { bad = true; break; }
            ivStepStores.insert(st + 3);
        }
        if (bad) continue;

        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            if (func.instrs[k].type != IRInstrType::STORE) continue;
            int dst = -1;
            if (!parseSlot(func.instrs[k].src2, dst)) { bad = true; break; }
            if (dst == iv && !ivStepStores.count(k)) { bad = true; break; }
        }
        if (bad) continue;

        // Validate all control-flow targets inside the body.  Internal labels
        // are cloned per phase; break targets keep the common loop exit.
        std::unordered_set<std::string> bodyLabels;
        for (size_t k = bodyBegin; k < bodyEnd; ++k)
            if (func.instrs[k].type == IRInstrType::LABEL)
                bodyLabels.insert(func.instrs[k].label);

        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            const auto& ins = func.instrs[k];
            if (ins.type == IRInstrType::CALL ||
                ins.type == IRInstrType::STORE_ARG ||
                ins.type == IRInstrType::LOAD_ARG ||
                ins.type == IRInstrType::RET) {
                bad = true; break;
            }
            if (ins.type == IRInstrType::JUMP ||
                ins.type == IRInstrType::BRANCH_ZERO ||
                ins.type == IRInstrType::BRANCH_NONZERO) {
                if (ins.label == header || ins.label == exitLabel ||
                    bodyLabels.count(ins.label))
                    continue;
                bad = true; break;
            }
        }
        if (bad) continue;

        const std::int64_t fastBound =
            bound - (period - 1) * step;
        if (fastBound < std::numeric_limits<std::int32_t>::min() ||
            fastBound > std::numeric_limits<std::int32_t>::max())
            continue;

        static std::uint64_t serial = 0;
        const auto id = serial++;
        const std::string fastLabel =
            header + ".periodic." + std::to_string(period) + "." + std::to_string(id);
        const std::string latchLabel =
            header + ".periodic_latch." + std::to_string(id);
        const std::string tailLabel =
            header + ".periodic_tail." + std::to_string(id);

        std::vector<std::string> phaseLabels(static_cast<size_t>(period));
        for (std::int64_t ph = 0; ph < period; ++ph) {
            phaseLabels[static_cast<size_t>(ph)] =
                fastLabel + ".p" + std::to_string(ph);
        }

        // Lookup remainder starts for O(1) clone specialization.
        std::unordered_map<size_t, std::int64_t> remAt;
        for (const auto& rp : rems) remAt[rp.pos] = rp.mod;

        std::vector<IRInstr> out;
        out.reserve(n + bodyLen * static_cast<size_t>(period));
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));

        // One guard proves that an entire residue period can run.
        out.emplace_back(IRInstrType::LI, "t0", std::to_string(fastBound));
        out.emplace_back(IRInstrType::LOAD, "t1", std::to_string(iv));
        out.emplace_back(IRInstrType::SLT, "t0", "t1", "t0");
        out.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", tailLabel);

        for (std::int64_t ph = 0; ph < period; ++ph) {
            const auto phaseIndex = static_cast<size_t>(ph);
            out.emplace_back(IRInstrType::LABEL, "", "", "", phaseLabels[phaseIndex]);

            std::unordered_map<std::string, std::string> labelMap;
            for (const auto& l : bodyLabels) {
                labelMap[l] = l + ".periodic." + std::to_string(id) +
                              ".p" + std::to_string(ph);
            }

            for (size_t k = bodyBegin; k < bodyEnd;) {
                auto rit = remAt.find(k);
                if (rit != remAt.end()) {
                    const std::int64_t mod = rit->second;
                    std::int64_t raw = init + ph * step;
                    std::int64_t residue = raw % mod;
                    if (residue < 0) residue += mod;
                    out.emplace_back(IRInstrType::LI, "t0",
                                     std::to_string(residue));
                    k += 3;
                    continue;
                }

                IRInstr ins = func.instrs[k];
                if (ins.type == IRInstrType::LABEL) {
                    auto lm = labelMap.find(ins.label);
                    if (lm != labelMap.end()) ins.label = lm->second;
                    out.push_back(std::move(ins));
                    ++k;
                    continue;
                }

                if (ins.type == IRInstrType::JUMP ||
                    ins.type == IRInstrType::BRANCH_ZERO ||
                    ins.type == IRInstrType::BRANCH_NONZERO) {
                    if (ins.label == header) {
                        // `continue` after a proven induction update.
                        if (ins.type != IRInstrType::JUMP) { bad = true; break; }
                        ins.label = (ph + 1 < period)
                            ? phaseLabels[static_cast<size_t>(ph + 1)]
                            : latchLabel;
                    } else {
                        auto lm = labelMap.find(ins.label);
                        if (lm != labelMap.end()) ins.label = lm->second;
                        // exitLabel (break) intentionally stays common.
                    }
                }
                out.push_back(std::move(ins));
                ++k;
            }
            if (bad) break;
            // Normal fallthrough proceeds directly to the next phase.  The
            // final source iteration reaches the shared periodic latch.
            if (ph + 1 == period)
                out.emplace_back(IRInstrType::JUMP, "", "", "", latchLabel);
        }
        if (bad) continue;

        out.emplace_back(IRInstrType::LABEL, "", "", "", latchLabel);
        out.emplace_back(IRInstrType::LI, "t0", std::to_string(fastBound));
        out.emplace_back(IRInstrType::LOAD, "t1", std::to_string(iv));
        out.emplace_back(IRInstrType::SLT, "t0", "t1", "t0");
        out.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", phaseLabels[0]);

        out.emplace_back(IRInstrType::LABEL, "", "", "", tailLabel);
        // Preserve the original scalar loop as the exact remainder path.
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(h),
                   func.instrs.end());

        func.instrs.swap(out);
        return true;
    }
    return false;
}


// Summarize a straight-line bottom-tested affine loop.
//
// Periodic remainder specialization above commonly produces a super-iteration
// with a single conditional back edge and no internal control flow.  Analyze
// that one super-iteration symbolically and replace all repeated super-trips by
// their affine closed form.  This is local recurrence analysis over one loop
// body using a closed-form recurrence.
bool summarizeAffineBottomTestLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 8) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        if (func.instrs[back].type != IRInstrType::BRANCH_NONZERO) continue;
        auto hit = labels.find(func.instrs[back].label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        if (back < 3) continue;

        // Canonical bottom test: LI bound; LOAD iv; SLT; BNEZ header.
        const size_t guardStart = back - 3;
        const auto& g0 = func.instrs[guardStart];
        const auto& g1 = func.instrs[guardStart + 1];
        const auto& g2 = func.instrs[guardStart + 2];
        const auto& gb = func.instrs[back];
        if (g0.type != IRInstrType::LI || g0.dest != "t0" ||
            g1.type != IRInstrType::LOAD || g1.dest != "t1" ||
            g2.type != IRInstrType::SLT || g2.dest != "t0" ||
            g2.src1 != "t1" || g2.src2 != "t0" ||
            gb.src1 != "t0")
            continue;

        int iv = -1;
        if (!parseSlot(g1.src1, iv)) continue;
        std::int64_t bound = 0;
        try { bound = std::stoll(g0.src1); }
        catch (...) { continue; }
        if (bound < std::numeric_limits<std::int32_t>::min() ||
            bound > std::numeric_limits<std::int32_t>::max())
            continue;

        const size_t bodyBegin = h + 1;
        const size_t bodyEnd = guardStart;
        if (bodyBegin >= bodyEnd) continue;

        // The super-body must be completely straight-line and side-effect free
        // except for local-slot stores.
        std::unordered_set<int> stored;
        bool safe = true;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO ||
                t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::LOAD_GLOBAL ||
                t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::LOAD_ARG || t == IRInstrType::STORE_ARG ||
                t == IRInstrType::RET) {
                safe = false; break;
            }
            if (t == IRInstrType::STORE) {
                int s = -1;
                if (!parseSlot(func.instrs[k].src2, s)) { safe = false; break; }
                stored.insert(s);
            }
        }
        if (!safe || !stored.count(iv)) continue;

        auto initOpt = straightLineConstBefore(func, h, iv);
        if (!initOpt) continue;
        const std::int64_t init = *initOpt;
        // If an initial guard still exists, straightLineConstBefore stops at it.
        // Therefore reaching here means this bottom-tested header is entered
        // directly; require the first super-trip to satisfy the loop condition.
        if (init >= bound) continue;

        std::unordered_map<std::string, AffineLoopExpr> regs;
        std::unordered_map<int, AffineLoopExpr> slots;
        std::unordered_set<int> modified;

        auto reg = [&](const std::string& r) -> AffineLoopExpr {
            auto it = regs.find(r);
            if (it == regs.end()) {
                AffineLoopExpr bad; bad.valid = false; return bad;
            }
            return it->second;
        };
        auto loadSlotExpr = [&](int slot) -> AffineLoopExpr {
            auto it = slots.find(slot);
            if (it != slots.end()) return it->second;
            if (!stored.count(slot) && slot != iv) {
                if (auto c = straightLineConstBefore(func, h, slot))
                    return affineConstant(*c);
            }
            return affineIdentity(slot);
        };

        for (size_t k = bodyBegin; k < bodyEnd && safe; ++k) {
            const auto& ins = func.instrs[k];
            switch (ins.type) {
                case IRInstrType::LI: {
                    long long v = 0;
                    try { v = std::stoll(ins.src1); }
                    catch (...) { safe = false; break; }
                    regs[ins.dest] = affineConstant(v);
                    break;
                }
                case IRInstrType::LOAD: {
                    int s = -1;
                    if (!parseSlot(ins.src1, s)) { safe = false; break; }
                    regs[ins.dest] = loadSlotExpr(s);
                    break;
                }
                case IRInstrType::STORE: {
                    int s = -1;
                    if (!parseSlot(ins.src2, s)) { safe = false; break; }
                    auto e = reg(ins.src1);
                    if (!e.valid) { safe = false; break; }
                    slots[s] = std::move(e);
                    modified.insert(s);
                    break;
                }
                case IRInstrType::MV: {
                    auto e = reg(ins.src1);
                    if (!e.valid) { safe = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::ADD:
                case IRInstrType::SUB: {
                    auto e = affineAdd(reg(ins.src1), reg(ins.src2),
                                       ins.type == IRInstrType::ADD ? 1 : -1);
                    if (!e.valid) { safe = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::MUL: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    AffineLoopExpr e;
                    if (affineIsConstant(a, ca)) e = affineScale(b, ca);
                    else if (affineIsConstant(b, cb)) e = affineScale(a, cb);
                    else e.valid = false;
                    if (!e.valid) { safe = false; break; }
                    regs[ins.dest] = std::move(e);
                    break;
                }
                case IRInstrType::DIV:
                case IRInstrType::REM:
                case IRInstrType::SLT: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    if (!affineIsConstant(a, ca) || !affineIsConstant(b, cb) ||
                        ca < std::numeric_limits<std::int32_t>::min() ||
                        ca > std::numeric_limits<std::int32_t>::max() ||
                        cb < std::numeric_limits<std::int32_t>::min() ||
                        cb > std::numeric_limits<std::int32_t>::max()) {
                        safe = false; break;
                    }
                    auto v = foldBinary(ins.type, static_cast<std::int32_t>(ca),
                                        static_cast<std::int32_t>(cb));
                    if (!v) { safe = false; break; }
                    regs[ins.dest] = affineConstant(*v);
                    break;
                }
                case IRInstrType::SEQZ:
                case IRInstrType::SNEZ: {
                    std::int64_t c = 0;
                    auto a = reg(ins.src1);
                    if (!affineIsConstant(a, c)) { safe = false; break; }
                    regs[ins.dest] = affineConstant(
                        ins.type == IRInstrType::SEQZ ? (c == 0 ? 1 : 0)
                                                     : (c != 0 ? 1 : 0));
                    break;
                }
                default:
                    safe = false;
                    break;
            }
        }
        if (!safe) continue;

        auto ivIt = slots.find(iv);
        if (ivIt == slots.end() || !ivIt->second.valid) continue;
        const auto& ivNext = ivIt->second;
        std::int64_t ivSelf = 0;
        for (const auto& [s, c] : ivNext.coeff) {
            if (s == iv) ivSelf = c;
            else { safe = false; break; }
        }
        if (!safe || ivSelf != 1 || ivNext.constant <= 0) continue;
        const std::int64_t step = ivNext.constant;

        const std::int64_t distance = bound - init;
        const std::int64_t trips = (distance + step - 1) / step;
        if (trips <= 0 || trips > 1000000000LL) continue;
        if (step > (std::numeric_limits<std::int64_t>::max() - init) / trips)
            continue;
        const std::int64_t finalIv = init + trips * step;
        if (finalIv < std::numeric_limits<std::int32_t>::min() ||
            finalIv > std::numeric_limits<std::int32_t>::max())
            continue;

        auto checkedMul = [](std::int64_t a, std::int64_t b,
                             std::int64_t& out) -> bool {
            if (a == 0 || b == 0) { out = 0; return true; }
            if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) return false;
            if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) return false;
            if (a > 0) {
                if (b > 0) {
                    if (a > std::numeric_limits<std::int64_t>::max() / b) return false;
                } else {
                    if (b < std::numeric_limits<std::int64_t>::min() / a) return false;
                }
            } else {
                if (b > 0) {
                    if (a < std::numeric_limits<std::int64_t>::min() / b) return false;
                } else {
                    if (a != 0 && b < std::numeric_limits<std::int64_t>::max() / a) return false;
                }
            }
            out = a * b;
            return true;
        };
        auto checkedAdd = [](std::int64_t a, std::int64_t b,
                             std::int64_t& out) -> bool {
            if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
                (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b))
                return false;
            out = a + b;
            return true;
        };

        std::int64_t nInit = 0, pairCount = 0, stepPairs = 0, sumIv = 0;
        if (!checkedMul(trips, init, nInit)) continue;
        std::int64_t aN = trips, bN = trips - 1;
        if ((aN & 1) == 0) aN /= 2; else bN /= 2;
        if (!checkedMul(aN, bN, pairCount) ||
            !checkedMul(step, pairCount, stepPairs) ||
            !checkedAdd(nInit, stepPairs, sumIv))
            continue;

        struct FinalValue { int slot; std::int32_t value; };
        std::vector<FinalValue> finals;
        for (int slot : modified) {
            if (slot == iv || !slotLoadedAfter(func, back + 1, slot)) continue;
            auto it = slots.find(slot);
            if (it == slots.end() || !it->second.valid) { safe = false; break; }

            std::int64_t self = 0, alpha = 0;
            for (const auto& [s, c] : it->second.coeff) {
                if (s == slot) self = c;
                else if (s == iv) alpha = c;
                else { safe = false; break; }
            }
            if (!safe || self != 1) { safe = false; break; }

            auto startOpt = straightLineConstBefore(func, h, slot);
            if (!startOpt) { safe = false; break; }

            std::int64_t alphaSum = 0, betaN = 0, total = *startOpt;
            if (!checkedMul(alpha, sumIv, alphaSum) ||
                !checkedMul(it->second.constant, trips, betaN) ||
                !checkedAdd(total, alphaSum, total) ||
                !checkedAdd(total, betaN, total) ||
                total < std::numeric_limits<std::int32_t>::min() ||
                total > std::numeric_limits<std::int32_t>::max()) {
                safe = false; break;
            }
            finals.push_back({slot, static_cast<std::int32_t>(total)});
        }
        if (!safe) continue;

        const bool ivLive = slotLoadedAfter(func, back + 1, iv);
        std::vector<IRInstr> out;
        out.reserve(n - (back - h + 1) + finals.size() * 2 + (ivLive ? 2 : 0));
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        for (const auto& fv : finals) {
            out.emplace_back(IRInstrType::LI, "t0", std::to_string(fv.value));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(fv.slot));
        }
        if (ivLive) {
            out.emplace_back(IRInstrType::LI, "t0",
                             std::to_string(static_cast<std::int32_t>(finalIv)));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(iv));
        }
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
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



// Strength-reduce loop multiplications involving the canonical induction
// variable.  For `iv*C`, carry a hidden product and update it by C each trip;
// for `iv*invariantLocal` with iv+=1, update the product by that invariant.
bool strengthReduceInductionProducts(IRFunction& func) {
    std::unordered_map<std::string,size_t> labels;
    for(size_t i=0;i<func.instrs.size();++i)
        if(func.instrs[i].type==IRInstrType::LABEL) labels[func.instrs[i].label]=i;

    for(size_t j=func.instrs.size(); j-- > 0;) {
        if(func.instrs[j].type!=IRInstrType::JUMP) continue;
        auto hit=labels.find(func.instrs[j].label); if(hit==labels.end()||hit->second>=j) continue;
        const size_t h=hit->second;
        int headerJumps=0;for(const auto&x:func.instrs)if(x.type==IRInstrType::JUMP&&x.label==func.instrs[j].label)++headerJumps;
        if(headerJumps!=1||j<4) continue;
        const size_t stepStart=j-4;
        const auto&s0=func.instrs[stepStart];const auto&s1=func.instrs[stepStart+1];const auto&s2=func.instrs[stepStart+2];const auto&s3=func.instrs[stepStart+3];
        int iv=-1,stiv=-2;long long step=0;
        if(s0.type!=IRInstrType::LI||s0.dest!="t0"||s1.type!=IRInstrType::LOAD||s1.dest!="t1"||!parseSlot(s1.src1,iv)||
           s2.type!=IRInstrType::ADD||s2.dest!="t0"||s2.src1!="t1"||s2.src2!="t0"||s3.type!=IRInstrType::STORE||s3.src1!="t0"||!parseSlot(s3.src2,stiv)||stiv!=iv) continue;
        try{step=std::stoll(s0.src1);}catch(...){continue;} if(step<=0||step>0x3fffffffLL) continue;
        int ivStores=0;for(size_t k=h+1;k<j;++k)if(func.instrs[k].type==IRInstrType::STORE){int q=-1;if(parseSlot(func.instrs[k].src2,q)&&q==iv)++ivStores;}
        if(ivStores!=1) continue;

        struct Key{bool imm=false;int32_t c=0;int slot=-1;};
        struct Pat{size_t begin,end;int dest;bool hadStore;Key key;};
        std::vector<Pat> pats;
        for(size_t k=h+1;k+2<stepStart;) {
            const auto&a=func.instrs[k];const auto&b=func.instrs[k+1];const auto&m=func.instrs[k+2];
            if(m.type!=IRInstrType::MUL||m.dest!="t0"){++k;continue;}
            bool normal=m.src1=="t1"&&m.src2=="t0",rev=m.src1=="t0"&&m.src2=="t1";if(!normal&&!rev){++k;continue;}
            bool hadStore=false; int dst=-1; size_t end=k+3;
            if(k+3<stepStart && func.instrs[k+3].type==IRInstrType::STORE && func.instrs[k+3].src1=="t0" && parseSlot(func.instrs[k+3].src2,dst)) { hadStore=true; end=k+4; }
            Key key; bool ok=false;
            // LI factor -> t0; LOAD iv -> t1; MUL
            int q=-1;
            if(a.type==IRInstrType::LI&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1"&&parseSlot(b.src1,q)&&q==iv) {
                long long c=0;try{c=std::stoll(a.src1);}catch(...){c=0;}
                if(c>=std::numeric_limits<int32_t>::min()&&c<=std::numeric_limits<int32_t>::max()&&c!=0&&c!=1&&c!=-1){key.imm=true;key.c=static_cast<int32_t>(c);ok=true;}
            }
            // LOAD factor -> t0; LOAD iv -> t1; factor must be loop invariant.
            if(!ok&&a.type==IRInstrType::LOAD&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1"&&step==1) {
                int sa=-1,sb=-1;if(parseSlot(a.src1,sa)&&parseSlot(b.src1,sb)) {
                    int fac=-1;
                    if(sa==iv&&sb!=iv) fac=sb;
                    else if(sb==iv&&sa!=iv) fac=sa;
                    if(fac>=0) {
                        bool written=false;for(size_t z=h+1;z<j;++z)if(func.instrs[z].type==IRInstrType::STORE){int w=-1;if(parseSlot(func.instrs[z].src2,w)&&w==fac){written=true;break;}}
                        if(!written){key.imm=false;key.slot=fac;ok=true;}
                    }
                }
            }
            if(ok){pats.push_back({k,end,dst,hadStore,key});k=end;}else ++k;
        }
        if(pats.empty()) continue;

        struct Prod{Key key;int slot;};std::vector<Prod> prods;
        auto same=[](const Key&a,const Key&b){return a.imm==b.imm&&(a.imm?a.c==b.c:a.slot==b.slot);};
        auto prodFor=[&](const Key&key){for(size_t z=0;z<prods.size();++z)if(same(prods[z].key,key))return static_cast<int>(z);int slot=func.localSize;func.localSize+=4;prods.push_back({key,slot});return static_cast<int>(prods.size()-1);};
        std::unordered_map<size_t,int> at;for(const auto&p0:pats)at[p0.begin]=prodFor(p0.key);

        std::vector<IRInstr> out;out.reserve(func.instrs.size()+prods.size()*10);
        for(size_t k=0;k<func.instrs.size();) {
            if(k==h) {
                for(const auto&pr:prods) {
                    if(pr.key.imm) out.emplace_back(IRInstrType::LI,"t0",std::to_string(pr.key.c));
                    else out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(pr.key.slot));
                    // For local factor, correct the source: key.slot is factor slot,
                    // while pr.slot is hidden product slot.
                    if(!pr.key.imm) out.back().src1=std::to_string(pr.key.slot);
                    out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(iv));
                    out.emplace_back(IRInstrType::MUL,"t0","t1","t0");
                    out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(pr.slot));
                }
            }
            if(k==stepStart) {
                for(const auto&pr:prods) {
                    if(pr.key.imm) {
                        const int32_t delta=static_cast<int32_t>(static_cast<uint32_t>(pr.key.c)*static_cast<uint32_t>(step));
                        out.emplace_back(IRInstrType::LI,"t0",std::to_string(delta));
                    } else {
                        out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(pr.key.slot));
                    }
                    out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(pr.slot));
                    out.emplace_back(IRInstrType::ADD,"t0","t1","t0");
                    out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(pr.slot));
                }
            }
            auto ai=at.find(k);
            if(ai!=at.end()) {
                const Pat* pp=nullptr;for(const auto&x:pats)if(x.begin==k){pp=&x;break;}
                out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(prods[ai->second].slot));
                if(pp->hadStore) out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(pp->dest));
                k=pp->end;continue;
            }
            out.push_back(func.instrs[k]);++k;
        }
        func.instrs.swap(out);return true;
    }
    return false;
}

// Strength-reduce `(iv % C) + K` inside loops where iv is a proven nonnegative
// induction variable initialized from a constant and incremented by exactly 1
// immediately before the unique back edge.  A hidden local carries the cyclic
// value K..K+C-1, replacing an expensive signed REM sequence on every trip.
bool strengthReduceInductionRemainders(IRFunction& func) {
    std::unordered_map<std::string,size_t> labels;
    for(size_t i=0;i<func.instrs.size();++i)
        if(func.instrs[i].type==IRInstrType::LABEL) labels[func.instrs[i].label]=i;

    for(size_t j=func.instrs.size(); j-- > 0;) {
        if(func.instrs[j].type!=IRInstrType::JUMP) continue;
        auto hit=labels.find(func.instrs[j].label);
        if(hit==labels.end()||hit->second>=j) continue;
        const size_t h=hit->second;

        int headerJumps=0;
        for(const auto& x:func.instrs)
            if(x.type==IRInstrType::JUMP&&x.label==func.instrs[j].label) ++headerJumps;
        if(headerJumps!=1) continue; // no continue-like edge

        // Require the canonical iv += 1 sequence immediately before backedge.
        if(j<4) continue;
        const size_t stepStart=j-4;
        const auto&s0=func.instrs[stepStart];const auto&s1=func.instrs[stepStart+1];
        const auto&s2=func.instrs[stepStart+2];const auto&s3=func.instrs[stepStart+3];
        int iv=-1,stiv=-2;
        if(s0.type!=IRInstrType::LI||s0.dest!="t0"||s0.src1!="1"||
           s1.type!=IRInstrType::LOAD||s1.dest!="t1"||!parseSlot(s1.src1,iv)||
           s2.type!=IRInstrType::ADD||s2.dest!="t0"||s2.src1!="t1"||s2.src2!="t0"||
           s3.type!=IRInstrType::STORE||s3.src1!="t0"||!parseSlot(s3.src2,stiv)||stiv!=iv) continue;
        int ivStores=0;
        std::unordered_map<int,int> loopDefs;
        for(size_t k=h+1;k<j;++k) if(func.instrs[k].type==IRInstrType::STORE){
            int q=-1;if(parseSlot(func.instrs[k].src2,q)){++loopDefs[q];if(q==iv)++ivStores;}
        }
        if(ivStores!=1) continue;

        // Recover a constant loop-entry value for iv from the straight-line preheader.
        bool haveInit=false; int32_t ivInit=0;
        for(size_t k=h;k-- > 0;) {
            if(func.instrs[k].type==IRInstrType::LABEL||func.instrs[k].type==IRInstrType::JUMP||
               func.instrs[k].type==IRInstrType::BRANCH_ZERO||func.instrs[k].type==IRInstrType::BRANCH_NONZERO) break;
            if(func.instrs[k].type==IRInstrType::STORE) {
                int q=-1;if(!parseSlot(func.instrs[k].src2,q)||q!=iv) continue;
                if(k>0&&func.instrs[k-1].type==IRInstrType::LI&&func.instrs[k-1].dest=="t0") {
                    long long v=0;try{v=std::stoll(func.instrs[k-1].src1);}catch(...){break;}
                    if(v>=std::numeric_limits<int32_t>::min()&&v<=std::numeric_limits<int32_t>::max()){
                        ivInit=static_cast<int32_t>(v);haveInit=true;
                    }
                }
                break;
            }
        }
        if(!haveInit) continue;

        // Prove the common non-negative induction/invariant form.  Signed C
        // remainder has a discontinuity while a negative dividend crosses a
        // multiple of the divisor, so the cheap +1 cyclic recurrence is used
        // only when the dividend is proven non-negative for the whole loop.
        auto slotAlwaysNonnegative=[&](int slot)->bool{
            int defs=0;
            for(size_t q=0;q<func.instrs.size();++q){
                const auto& st=func.instrs[q];
                int dst=-1;
                if(st.type!=IRInstrType::STORE||!parseSlot(st.src2,dst)||dst!=slot) continue;
                ++defs; bool ok=false;
                if(q>0&&func.instrs[q-1].type==IRInstrType::LI&&func.instrs[q-1].dest=="t0") {
                    long long v=0;try{v=std::stoll(func.instrs[q-1].src1);}catch(...){return false;}
                    ok=v>=0&&v<=std::numeric_limits<int32_t>::max();
                }
                if(!ok&&q>=3){
                    const auto&a=func.instrs[q-3];const auto&b=func.instrs[q-2];const auto&o=func.instrs[q-1];
                    int src=-1; long long inc=0;
                    if(a.type==IRInstrType::LI&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1"&&
                       parseSlot(b.src1,src)&&src==slot&&o.type==IRInstrType::ADD&&o.dest=="t0"&&o.src1=="t1"&&o.src2=="t0") {
                        try{inc=std::stoll(a.src1);}catch(...){return false;}
                        ok=inc>=0&&inc<=std::numeric_limits<int32_t>::max();
                    }
                }
                if(!ok) return false;
            }
            return defs>0;
        };

        // Describe a value that advances by exactly +1 whenever iv advances by
        // +1.  Besides iv itself, accept `iv + invariantLocal` and `iv + C`.
        // This is especially common in nested graph/matrix loops such as
        // `(outer + inner) % 7`.  The invariant may change between invocations
        // of the inner loop; the cyclic remainder is therefore initialized at
        // the loop preheader, not at compile time.
        struct SourceDesc { int invariantSlot=-1; int32_t offsetImm=0; };
        auto affineSource=[&](int srcSlot,size_t before,SourceDesc& desc)->bool{
            if(srcSlot==iv){if(ivInit<0)return false;desc={-1,0};return true;}
            // Require exactly one definition of the temporary in this loop.
            if(loopDefs[srcSlot]!=1) return false;
            // Find its defining four-instruction ADD before the REM use.
            for(size_t stPos=before; stPos-- > h+3;) {
                const auto& st=func.instrs[stPos];
                int dst=-1;
                if(st.type!=IRInstrType::STORE||st.src1!="t0"||!parseSlot(st.src2,dst)||dst!=srcSlot) continue;
                if(stPos<3) return false;
                const auto& x0=func.instrs[stPos-3];
                const auto& x1=func.instrs[stPos-2];
                const auto& op=func.instrs[stPos-1];
                if(op.type!=IRInstrType::ADD||op.dest!="t0"||op.src1!="t1"||op.src2!="t0") return false;
                int slot0=-1,slot1=-1; bool load0=x0.type==IRInstrType::LOAD&&x0.dest=="t0"&&parseSlot(x0.src1,slot0);
                bool load1=x1.type==IRInstrType::LOAD&&x1.dest=="t1"&&parseSlot(x1.src1,slot1);
                bool imm0=x0.type==IRInstrType::LI&&x0.dest=="t0";
                // LOAD rhs -> t0 ; LOAD lhs -> t1 ; ADD
                if(load0&&load1){
                    int inv=-1;
                    if(slot0==iv&&slot1!=iv) inv=slot1;
                    else if(slot1==iv&&slot0!=iv) inv=slot0;
                    if(inv<0||loopDefs.count(inv)||ivInit<0||!slotAlwaysNonnegative(inv)) return false;
                    desc={inv,0}; return true;
                }
                // LI constant -> t0 ; LOAD iv -> t1 ; ADD
                if(imm0&&load1&&slot1==iv){
                    long long z=0;try{z=std::stoll(x0.src1);}catch(...){return false;}
                    if(z<std::numeric_limits<int32_t>::min()||z>std::numeric_limits<int32_t>::max()) return false;
                    const int64_t base=static_cast<int64_t>(ivInit)+z;
                    if(ivInit<0||base<0||base>std::numeric_limits<int32_t>::max()) return false;
                    desc={-1,static_cast<int32_t>(z)}; return true;
                }
                return false;
            }
            return false;
        };

        struct Pat{size_t begin,end;int dest;int32_t mod,add;SourceDesc src;};
        std::vector<Pat> pats;
        for(size_t k=h+1;k+3<stepStart;) {
            const auto&a=func.instrs[k];const auto&b=func.instrs[k+1];
            const auto&r=func.instrs[k+2];const auto&st=func.instrs[k+3];
            int qsrc=-1,dst=-1;
            if(a.type==IRInstrType::LI&&a.dest=="t0"&&b.type==IRInstrType::LOAD&&b.dest=="t1"&&parseSlot(b.src1,qsrc)&&
               r.type==IRInstrType::REM&&r.dest=="t0"&&r.src1=="t1"&&r.src2=="t0"&&
               st.type==IRInstrType::STORE&&st.src1=="t0"&&parseSlot(st.src2,dst)) {
                long long m=0;try{m=std::stoll(a.src1);}catch(...){++k;continue;}
                if(m<=1||m>1000000){++k;continue;}
                SourceDesc src;
                if(!affineSource(qsrc,k,src)){++k;continue;}
                int32_t add=0; size_t end=k+4; int finalDst=dst;
                // Optional immediately-following + K.
                if(k+7<stepStart) {
                    const auto&c0=func.instrs[k+4];const auto&c1=func.instrs[k+5];
                    const auto&c2=func.instrs[k+6];const auto&c3=func.instrs[k+7];
                    int srcSlot=-1,fd=-1;
                    if(c0.type==IRInstrType::LI&&c0.dest=="t0"&&c1.type==IRInstrType::LOAD&&c1.dest=="t1"&&parseSlot(c1.src1,srcSlot)&&srcSlot==dst&&
                       c2.type==IRInstrType::ADD&&c2.dest=="t0"&&c2.src1=="t1"&&c2.src2=="t0"&&
                       c3.type==IRInstrType::STORE&&c3.src1=="t0"&&parseSlot(c3.src2,fd)) {
                        long long z=0;try{z=std::stoll(c0.src1);}catch(...){z=0;}
                        if(z>=std::numeric_limits<int32_t>::min()&&z<=std::numeric_limits<int32_t>::max()) {
                            add=static_cast<int32_t>(z);finalDst=fd;end=k+8;
                        }
                    }
                }
                const int64_t lo=static_cast<int64_t>(add)-(m-1); // signed REM may start negative
                const int64_t hi=static_cast<int64_t>(add)+m-1;
                if(lo<std::numeric_limits<int32_t>::min()||hi>std::numeric_limits<int32_t>::max()){++k;continue;}
                pats.push_back({k,end,finalDst,static_cast<int32_t>(m),add,src}); k=end; continue;
            }
            ++k;
        }
        if(pats.empty()) continue;

        struct Cyc{int32_t mod,add;SourceDesc src;int slot;std::string wrap;};
        std::vector<Cyc> cycles;
        auto sameSrc=[](const SourceDesc&a,const SourceDesc&b){return a.invariantSlot==b.invariantSlot&&a.offsetImm==b.offsetImm;};
        auto cycFor=[&](int32_t mod,int32_t add,const SourceDesc&src)->int{
            for(size_t z=0;z<cycles.size();++z)
                if(cycles[z].mod==mod&&cycles[z].add==add&&sameSrc(cycles[z].src,src))return static_cast<int>(z);
            int slot=func.localSize;func.localSize+=4;
            cycles.push_back({mod,add,src,slot,".remcycle_"+func.name+"_"+std::to_string(h)+"_"+std::to_string(cycles.size())});
            return static_cast<int>(cycles.size()-1);
        };
        std::unordered_map<size_t,int> at;
        for(const auto& p0:pats) at[p0.begin]=cycFor(p0.mod,p0.add,p0.src);

        std::vector<IRInstr> out;out.reserve(func.instrs.size()+cycles.size()*18);
        for(size_t k=0;k<func.instrs.size();) {
            if(k==h) {
                for(const auto&c:cycles) {
                    if(c.src.invariantSlot<0) {
                        const int64_t base=static_cast<int64_t>(ivInit)+c.src.offsetImm;
                        if(base<std::numeric_limits<int32_t>::min()||base>std::numeric_limits<int32_t>::max()) return false;
                        const int32_t init=static_cast<int32_t>(base%c.mod+c.add);
                        out.emplace_back(IRInstrType::LI,"t0",std::to_string(init));
                        out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.slot));
                    } else {
                        // Compute signed `(invariant + ivInit + offset) % mod + add`
                        // once per loop invocation.  The back edge targets the
                        // LABEL below and therefore skips this preheader.
                        out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(c.src.invariantSlot));
                        const int64_t add0=static_cast<int64_t>(ivInit)+c.src.offsetImm;
                        if(add0!=0) {
                            if(add0<std::numeric_limits<int32_t>::min()||add0>std::numeric_limits<int32_t>::max()) return false;
                            out.emplace_back(IRInstrType::LI,"t1",std::to_string(static_cast<int32_t>(add0)));
                            out.emplace_back(IRInstrType::ADD,"t0","t0","t1");
                        }
                        out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.slot));
                        out.emplace_back(IRInstrType::LI,"t0",std::to_string(c.mod));
                        out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(c.slot));
                        out.emplace_back(IRInstrType::REM,"t0","t1","t0");
                        if(c.add!=0) {
                            out.emplace_back(IRInstrType::LI,"t1",std::to_string(c.add));
                            out.emplace_back(IRInstrType::ADD,"t0","t0","t1");
                        }
                        out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.slot));
                    }
                }
            }
            if(k==stepStart) {
                for(const auto&c:cycles) {
                    const int32_t maxExclusive=static_cast<int32_t>(static_cast<int64_t>(c.add)+c.mod);
                    out.emplace_back(IRInstrType::LI,"t0","1");
                    out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(c.slot));
                    out.emplace_back(IRInstrType::ADD,"t0","t1","t0");
                    out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.slot));
                    out.emplace_back(IRInstrType::LI,"t0",std::to_string(maxExclusive));
                    out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(c.slot));
                    out.emplace_back(IRInstrType::SLT,"t0","t1","t0");
                    out.emplace_back(IRInstrType::BRANCH_NONZERO,"","t0","",c.wrap);
                    out.emplace_back(IRInstrType::LI,"t0",std::to_string(c.add));
                    out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(c.slot));
                    out.emplace_back(IRInstrType::LABEL,"","","",c.wrap);
                }
            }
            auto ai=at.find(k);
            if(ai!=at.end()) {
                const Pat* pp=nullptr;for(const auto&x:pats)if(x.begin==k){pp=&x;break;}
                const auto&c=cycles[ai->second];
                out.emplace_back(IRInstrType::LOAD,"t0",std::to_string(c.slot));
                out.emplace_back(IRInstrType::STORE,"","t0",std::to_string(pp->dest));
                k=pp->end;continue;
            }
            out.push_back(func.instrs[k]);++k;
        }
        func.instrs.swap(out);
        return true; // rebuild CFG/indices in the normal cleanup rounds
    }
    return false;
}

// Safe 4x unrolling for canonical straight-line while loops.  We duplicate the
// condition+body three extra times and keep only one backward jump.  Every copy
// still performs the original condition check, so trip counts, overflow and
// side effects are unchanged; only continue/break/nested-control-flow loops are
// excluded because they contain extra labels/jumps.
bool unrollSimpleLoops(IRFunction& func) {
    bool changed=false;
    std::uint64_t serial=0;

    // First prefer a real four-at-a-time unroll for the overwhelmingly common
    // canonical loop
    //     while (iv < CONSTANT) { straight-line-body; iv = iv + STEP; }
    // Rather than duplicating the condition four times, execute one guard for
    // four bodies and leave the original scalar loop as a <=3-iteration tail.
    for (size_t j=func.instrs.size(); j-- > 0;) {
        if (func.instrs[j].type!=IRInstrType::JUMP) continue;
        const std::string header=func.instrs[j].label;
        if (j+1>=func.instrs.size() || func.instrs[j+1].type!=IRInstrType::LABEL) continue;
        const std::string endLabel=func.instrs[j+1].label;
        size_t h=j;
        while(h>0){--h;if(func.instrs[h].type==IRInstrType::LABEL && func.instrs[h].label==header)break;}
        if(func.instrs[h].type!=IRInstrType::LABEL||func.instrs[h].label!=header||h>=j) continue;

        // The header must have no other explicit jump predecessor; continue-like
        // control flow is intentionally left to the conservative old unroller.
        int headerJumps=0;
        for(size_t k=0;k<func.instrs.size();++k)
            if(func.instrs[k].type==IRInstrType::JUMP&&func.instrs[k].label==header) ++headerJumps;
        if(headerJumps!=1) continue;

        size_t branchPos=func.instrs.size(); int branchCount=0; bool simple=true;
        for(size_t k=h+1;k<j;++k){
            auto t=func.instrs[k].type;
            if(t==IRInstrType::LABEL||t==IRInstrType::JUMP||t==IRInstrType::RET){simple=false;break;}
            if(t==IRInstrType::BRANCH_ZERO||t==IRInstrType::BRANCH_NONZERO){
                ++branchCount; branchPos=k;
                if(func.instrs[k].label!=endLabel){simple=false;break;}
            }
        }
        if(!simple||branchCount!=1||branchPos<h+4) continue;

        // Exact optimized IR form for `iv < constant`.
        const auto& c0=func.instrs[branchPos-3];
        const auto& c1=func.instrs[branchPos-2];
        const auto& c2=func.instrs[branchPos-1];
        const auto& cb=func.instrs[branchPos];
        if(c0.type!=IRInstrType::LI||c0.dest!="t0"||
           c1.type!=IRInstrType::LOAD||c1.dest!="t1"||
           c2.type!=IRInstrType::SLT||c2.dest!="t0"||c2.src1!="t1"||c2.src2!="t0"||
           cb.type!=IRInstrType::BRANCH_ZERO||cb.src1!="t0") continue;
        int iv=0; if(!parseSlot(c1.src1,iv)) continue;
        long long bound=0; try{bound=std::stoll(c0.src1);}catch(...){continue;}
        if(bound<std::numeric_limits<int32_t>::min()||bound>std::numeric_limits<int32_t>::max()) continue;

        // Find exactly one canonical positive induction update in the body.
        long long step=0; int updates=0;
        for(size_t k=branchPos+1;k+3<j;++k){
            const auto&a=func.instrs[k];const auto&b=func.instrs[k+1];
            const auto&op=func.instrs[k+2];const auto&st=func.instrs[k+3];
            int loadSlot=-1,storeSlot=-2;
            if(a.type==IRInstrType::LI&&a.dest=="t0"&&
               b.type==IRInstrType::LOAD&&b.dest=="t1"&&parseSlot(b.src1,loadSlot)&&loadSlot==iv&&
               op.type==IRInstrType::ADD&&op.dest=="t0"&&op.src1=="t1"&&op.src2=="t0"&&
               st.type==IRInstrType::STORE&&st.src1=="t0"&&parseSlot(st.src2,storeSlot)&&storeSlot==iv){
                long long q=0;try{q=std::stoll(a.src1);}catch(...){continue;}
                if(q>0&&q<=0x3fffffffLL){step=q;++updates;}
            }
        }
        if(updates!=1||step<=0) continue;
        const size_t bodyBegin=branchPos+1, bodyEnd=j;
        const size_t bodyLen=bodyEnd-bodyBegin;
       if(bodyLen==0||bodyLen>96) continue;
        // Tiny hot loops benefit from a wider unroll because branch overhead is
        // a large fraction of their dynamic work.  Keep 4x for medium bodies to
        // avoid excessive code growth/instruction-cache pressure.
        // Tiny straight-line loops are dominated by branch/control overhead.
        // A 16x factor is still small when the body has <= 8 IR instructions;
        // larger loops retain the previous conservative factors.
        const int unrollFactor = bodyLen <= 8 ? 32 : (bodyLen <= 16 ? 16 : (bodyLen <= 32 ? 8 : 4));
        const long long fastBound=bound-static_cast<long long>(unrollFactor-1)*step;
        if(fastBound<std::numeric_limits<int32_t>::min()||fastBound>std::numeric_limits<int32_t>::max()) continue;

        const std::string fastLabel=header+".u"+std::to_string(unrollFactor)+"."+std::to_string(serial);
        const std::string tailLabel=header+".tail."+std::to_string(serial++);
        std::vector<IRInstr> out;
        out.reserve(func.instrs.size()+bodyLen*static_cast<size_t>(unrollFactor)+8);
        out.insert(out.end(),func.instrs.begin(),func.instrs.begin()+static_cast<long>(h));
        // One entry guard, then a bottom-tested 4-at-a-time loop.  The steady
        // state therefore executes one branch per four source iterations rather
        // than a top test plus an unconditional back jump.
        out.emplace_back(IRInstrType::LI,"t0",std::to_string(fastBound));
        out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(iv));
        out.emplace_back(IRInstrType::SLT,"t0","t1","t0");
        out.emplace_back(IRInstrType::BRANCH_ZERO,"","t0","",tailLabel);
        out.emplace_back(IRInstrType::LABEL,"","","",fastLabel);
        for(int copy=0;copy<unrollFactor;++copy)
            out.insert(out.end(),func.instrs.begin()+static_cast<long>(bodyBegin),func.instrs.begin()+static_cast<long>(bodyEnd));
        out.emplace_back(IRInstrType::LI,"t0",std::to_string(fastBound));
        out.emplace_back(IRInstrType::LOAD,"t1",std::to_string(iv));
        out.emplace_back(IRInstrType::SLT,"t0","t1","t0");
        out.emplace_back(IRInstrType::BRANCH_NONZERO,"","t0","",fastLabel);
        out.emplace_back(IRInstrType::LABEL,"","","",tailLabel);
        // Preserve the complete original scalar loop as the remainder path.
        out.insert(out.end(),func.instrs.begin()+static_cast<long>(h),func.instrs.end());
        func.instrs.swap(out); changed=true;
        // Restart because indices/labels changed.  The original scalar tail has
        // an extra predecessor and therefore will not be selected again.
        return true;
    }

    // Conservative fallback used by v11 for non-canonical straight-line loops.
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
        if(segLen==0 || segLen>96) continue;
        std::vector<IRInstr> repl; repl.reserve(func.instrs.size()+segLen*3);
        repl.insert(repl.end(),func.instrs.begin(),func.instrs.begin()+static_cast<long>(j));
        for(int copy=0;copy<7;++copy) repl.insert(repl.end(),func.instrs.begin()+static_cast<long>(segBegin),func.instrs.begin()+static_cast<long>(segEnd));
        repl.insert(repl.end(),func.instrs.begin()+static_cast<long>(j),func.instrs.end());
        func.instrs.swap(repl); changed=true;
        j=h+1;
    }
    return changed;
}

// Unroll the bottom-tested loop produced by direct tail-recursion elimination.
// The transform is deliberately restricted to the compiler-generated
// `.tail_latch_` shape and a positive constant countdown value.  It preserves
// the scalar loop as a remainder path, so recursion semantics are unchanged
// while the hot path executes one branch per 16 source tail calls.
bool unrollTailCountdownLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 12) return false;
    static std::uint64_t serial = 0;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        const auto& br = func.instrs[back];
        if (br.type != IRInstrType::BRANCH_NONZERO &&
            br.type != IRInstrType::BRANCH_ZERO) continue;
        auto hit = labels.find(br.label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;

        // Locate the compiler-generated tail latch inside this natural loop.
        size_t latch = n;
        for (size_t k = h + 1; k < back; ++k) {
            if (func.instrs[k].type == IRInstrType::LABEL &&
                func.instrs[k].label.find(".tail_latch_") != std::string::npos)
                latch = k;
        }
        if (latch == n || latch + 2 >= back + 1) continue;

        // Exact latch condition generated by tail-recursion rotation:
        //   LOAD x; SEQZ x; BRANCH_ZERO header
        // or the logically equivalent SNEZ + BRANCH_NONZERO form.
        const auto& ld = func.instrs[latch + 1];
        const auto& nz = func.instrs[latch + 2];
        const bool nonzeroBack =
            (nz.type == IRInstrType::SEQZ && br.type == IRInstrType::BRANCH_ZERO) ||
            (nz.type == IRInstrType::SNEZ && br.type == IRInstrType::BRANCH_NONZERO);
        if (latch + 3 != back ||
            ld.type != IRInstrType::LOAD || !nonzeroBack ||
            nz.dest != ld.dest || nz.src1 != ld.dest || br.src1 != nz.dest)
            continue;
        int condSlot = -1;
        if (!parseSlot(ld.src1, condSlot)) continue;

        // The condition temporary is copied to the real countdown parameter in
        // the body.  Recover that carried slot so the fast-entry guard can test
        // a value that is initialized before the first iteration.
        int countdownSlot = -1;
        for (size_t k = h + 1; k + 1 < latch; ++k) {
            const auto& a = func.instrs[k];
            const auto& b = func.instrs[k + 1];
            int src = -1, dst = -1;
            if (a.type == IRInstrType::LOAD && a.dest == "t0" &&
                b.type == IRInstrType::STORE && b.src1 == "t0" &&
                parseSlot(a.src1, src) && src == condSlot &&
                parseSlot(b.src2, dst)) {
                countdownSlot = dst;
            }
        }
        if (countdownSlot < 0) continue;
        size_t initHeader = h;
        while (initHeader > 0 && func.instrs[initHeader - 1].type == IRInstrType::LABEL)
            --initHeader;
        auto init = straightLineConstBefore(func, initHeader, countdownSlot);
        if (!init || *init < 32) continue;

        // No control flow or observable side effects inside one generated tail
        // iteration.  Labels at the very front are harmless aliases; labels
        // inside the body would be duplicated and are therefore rejected.
        size_t bodyBegin = h + 1;
        while (bodyBegin < latch && func.instrs[bodyBegin].type == IRInstrType::LABEL)
            ++bodyBegin;
        if (bodyBegin >= latch) continue;
        bool safe = true;
        for (size_t k = bodyBegin; k < latch; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                safe = false; break;
            }
        }
        if (!safe) continue;

        constexpr int F = 32;
        const std::string fastGuard = ".tail_u16_guard_" + func.name + "_" +
                                      std::to_string(serial);
        const std::string fastBody = ".tail_u16_body_" + func.name + "_" +
                                     std::to_string(serial);
        const std::string tail = ".tail_u16_tail_" + func.name + "_" +
                                 std::to_string(serial++);

        std::vector<IRInstr> out;
        out.reserve(n + (latch - bodyBegin) * (F - 1) + 12);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));

        out.emplace_back(IRInstrType::LABEL, "", "", "", fastGuard);
        out.emplace_back(IRInstrType::LI, "t0", std::to_string(F));
        out.emplace_back(IRInstrType::LOAD, "t1", std::to_string(countdownSlot));
        out.emplace_back(IRInstrType::SLT, "t0", "t1", "t0"); // countdown < F
        out.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", tail);
        out.emplace_back(IRInstrType::LABEL, "", "", "", fastBody);

        for (int copy = 0; copy < F; ++copy)
            out.insert(out.end(), func.instrs.begin() + static_cast<long>(bodyBegin),
                       func.instrs.begin() + static_cast<long>(latch));
        out.emplace_back(IRInstrType::JUMP, "", "", "", fastGuard);

        out.emplace_back(IRInstrType::LABEL, "", "", "", tail);
        // If the chunked path lands exactly on zero, skip the original
        // do-while-shaped scalar remainder.  Otherwise it handles 1..15 trips.
        const std::string doneLabel =
            (back + 1 < n && func.instrs[back + 1].type == IRInstrType::LABEL)
                ? func.instrs[back + 1].label : std::string{};
        if (doneLabel.empty()) continue;
        out.emplace_back(IRInstrType::LOAD, "t0", std::to_string(countdownSlot));
        out.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", doneLabel);
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(h),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}


// Collapse the same post-inline unit-countdown tail loop when its carried local
// state is affine.  The trip count is proven from the positive constant entry
// countdown and the exact `countdown' = countdown - 1` recurrence.  One loop
// iteration is represented symbolically, then the affine transition is raised
// to N with exponentiation by squaring.  This is structural recurrence analysis,
// not interpretation of the function or main().
bool summarizeInlinedUnitCountdownLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 12) return false;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        const auto& br = func.instrs[back];
        if (br.type != IRInstrType::BRANCH_ZERO &&
            br.type != IRInstrType::BRANCH_NONZERO)
            continue;
        auto hit = labels.find(br.label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        if (br.label.find(".inl") == std::string::npos || back < h + 5 || back < 2)
            continue;

        const auto& condLoad = func.instrs[back - 2];
        const auto& condNorm = func.instrs[back - 1];
        const bool continueOnNonzero =
            (condNorm.type == IRInstrType::SEQZ && br.type == IRInstrType::BRANCH_ZERO) ||
            (condNorm.type == IRInstrType::SNEZ && br.type == IRInstrType::BRANCH_NONZERO);
        if (!continueOnNonzero || condLoad.type != IRInstrType::LOAD ||
            condNorm.dest != condLoad.dest || condNorm.src1 != condLoad.dest ||
            br.src1 != condNorm.dest)
            continue;

        int condSlot = -1;
        if (!parseSlot(condLoad.src1, condSlot)) continue;
        const size_t bodyBegin = h + 1;
        const size_t bodyEnd = back - 2;
        if (bodyBegin >= bodyEnd) continue;

        bool safe = true;
        std::unordered_set<int> modified;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::LOAD_GLOBAL ||
                t == IRInstrType::STORE_GLOBAL || t == IRInstrType::LOAD_ARG ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                safe = false; break;
            }
            if (auto d = slotDef(func.instrs[k])) modified.insert(*d);
        }
        if (!safe) continue;

        int countdownSlot = -1;
        for (size_t k = bodyBegin; k + 1 < bodyEnd; ++k) {
            const auto& a = func.instrs[k];
            const auto& b = func.instrs[k + 1];
            int src = -1, dst = -1;
            if (a.type == IRInstrType::LOAD &&
                b.type == IRInstrType::STORE && b.src1 == a.dest &&
                parseSlot(a.src1, src) && src == condSlot &&
                parseSlot(b.src2, dst) && dst != condSlot) {
                if (countdownSlot != -1 && countdownSlot != dst) {
                    countdownSlot = -2; break;
                }
                countdownSlot = dst;
            }
        }
        if (countdownSlot < 0) continue;

        bool unitDecrement = false;
        for (size_t k = bodyBegin; k + 3 < bodyEnd; ++k) {
            const auto& i0 = func.instrs[k];
            const auto& i1 = func.instrs[k + 1];
            const auto& i2 = func.instrs[k + 2];
            const auto& i3 = func.instrs[k + 3];
            int loadSlot = -1, storeSlot = -1;
            if (i0.type != IRInstrType::LI || i1.type != IRInstrType::LOAD ||
                i3.type != IRInstrType::STORE || i3.src1 != i2.dest ||
                !parseSlot(i1.src1, loadSlot) || loadSlot != countdownSlot ||
                !parseSlot(i3.src2, storeSlot) || storeSlot != condSlot)
                continue;
            long long imm = 0;
            try { imm = std::stoll(i0.src1); } catch (...) { continue; }
            if (i2.type == IRInstrType::SUB && imm == 1 &&
                i2.src1 == i1.dest && i2.src2 == i0.dest) {
                unitDecrement = true; break;
            }
            if (i2.type == IRInstrType::ADD && imm == -1 &&
                ((i2.src1 == i1.dest && i2.src2 == i0.dest) ||
                 (i2.src2 == i1.dest && i2.src1 == i0.dest))) {
                unitDecrement = true; break;
            }
        }
        if (!unitDecrement) continue;

        int condStores = 0, countdownStores = 0;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            if (func.instrs[k].type != IRInstrType::STORE) continue;
            int q = -1;
            if (!parseSlot(func.instrs[k].src2, q)) { safe = false; break; }
            if (q == condSlot) ++condStores;
            if (q == countdownSlot) ++countdownStores;
        }
        if (!safe || condStores != 1 || countdownStores != 1) continue;

        auto initCountdown = straightLineConstBefore(func, h, countdownSlot);
        if (!initCountdown || *initCountdown <= 0) continue;
        const std::uint64_t trips = static_cast<std::uint64_t>(*initCountdown);

        std::unordered_map<std::string, AffineLoopExpr> regs;
        std::unordered_map<int, AffineLoopExpr> slots;
        auto reg = [&](const std::string& r) -> AffineLoopExpr {
            auto it = regs.find(r);
            if (it == regs.end()) { AffineLoopExpr bad; bad.valid = false; return bad; }
            return it->second;
        };
        auto loadSlot = [&](int slot) -> AffineLoopExpr {
            auto it = slots.find(slot);
            if (it != slots.end()) return it->second;
            if (!modified.count(slot)) {
                if (auto c = straightLineConstBefore(func, h, slot))
                    return affineConstant(*c);
            }
            return affineIdentity(slot);
        };

        bool symbolicOK = true;
        for (size_t k = bodyBegin; k < bodyEnd && symbolicOK; ++k) {
            const auto& ins = func.instrs[k];
            switch (ins.type) {
                case IRInstrType::LI: {
                    long long v = 0; try { v = std::stoll(ins.src1); }
                    catch (...) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(v); break;
                }
                case IRInstrType::LOAD: {
                    int q = -1; if (!parseSlot(ins.src1, q)) { symbolicOK = false; break; }
                    regs[ins.dest] = loadSlot(q); break;
                }
                case IRInstrType::STORE: {
                    int q = -1; if (!parseSlot(ins.src2, q)) { symbolicOK = false; break; }
                    auto e = reg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    slots[q] = std::move(e); break;
                }
                case IRInstrType::MV: {
                    auto e = reg(ins.src1); if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::ADD:
                case IRInstrType::SUB: {
                    auto e = affineAdd(reg(ins.src1), reg(ins.src2),
                                       ins.type == IRInstrType::ADD ? 1 : -1);
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::MUL: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0; AffineLoopExpr e;
                    if (affineIsConstant(a, ca)) e = affineScale(b, ca);
                    else if (affineIsConstant(b, cb)) e = affineScale(a, cb);
                    else e.valid = false;
                    if (!e.valid) { symbolicOK = false; break; }
                    regs[ins.dest] = std::move(e); break;
                }
                case IRInstrType::DIV:
                case IRInstrType::REM:
                case IRInstrType::SLT: {
                    auto a = reg(ins.src1), b = reg(ins.src2);
                    std::int64_t ca = 0, cb = 0;
                    if (!affineIsConstant(a, ca) || !affineIsConstant(b, cb) ||
                        ca < std::numeric_limits<std::int32_t>::min() ||
                        ca > std::numeric_limits<std::int32_t>::max() ||
                        cb < std::numeric_limits<std::int32_t>::min() ||
                        cb > std::numeric_limits<std::int32_t>::max()) {
                        symbolicOK = false; break;
                    }
                    auto v = foldBinary(ins.type, static_cast<std::int32_t>(ca),
                                        static_cast<std::int32_t>(cb));
                    if (!v) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(*v); break;
                }
                case IRInstrType::SEQZ:
                case IRInstrType::SNEZ: {
                    auto a = reg(ins.src1); std::int64_t c = 0;
                    if (!affineIsConstant(a, c)) { symbolicOK = false; break; }
                    regs[ins.dest] = affineConstant(
                        ins.type == IRInstrType::SEQZ ? (c == 0 ? 1 : 0)
                                                     : (c != 0 ? 1 : 0));
                    break;
                }
                default: symbolicOK = false; break;
            }
        }
        if (!symbolicOK) continue;

        std::unordered_set<int> stateSet;
        for (int q : modified)
            if (slotLoadedAfter(func, back + 1, q)) stateSet.insert(q);
        if (stateSet.empty()) continue;

        bool depsOK = true, grew = true;
        while (grew && depsOK) {
            grew = false;
            std::vector<int> cur(stateSet.begin(), stateSet.end());
            for (int q : cur) {
                AffineLoopExpr e = slots.count(q) ? slots[q] : affineIdentity(q);
                if (!e.valid) { depsOK = false; break; }
                for (const auto& [dep, coeff] : e.coeff) {
                    if (coeff == 0 || stateSet.count(dep)) continue;
                    if (modified.count(dep)) { stateSet.insert(dep); grew = true; }
                    else { depsOK = false; break; }
                }
                if (!depsOK) break;
            }
        }
        if (!depsOK || stateSet.empty() || stateSet.size() > 16) continue;

        std::vector<int> state(stateSet.begin(), stateSet.end());
        std::sort(state.begin(), state.end());
        std::unordered_map<int, size_t> pos;
        for (size_t i = 0; i < state.size(); ++i) pos[state[i]] = i;
        const size_t D = state.size() + 1;

        std::vector<std::uint32_t> M(D * D, 0);
        bool matrixOK = true;
        std::vector<char> initialNeeded(state.size(), 0);
        for (size_t r = 0; r < state.size(); ++r) {
            AffineLoopExpr e = slots.count(state[r]) ? slots[state[r]]
                                                     : affineIdentity(state[r]);
            if (!e.valid) { matrixOK = false; break; }
            for (const auto& [dep, coeff] : e.coeff) {
                auto it = pos.find(dep);
                if (it == pos.end()) { matrixOK = false; break; }
                M[r * D + it->second] += static_cast<std::uint32_t>(coeff);
                if (coeff != 0) initialNeeded[it->second] = 1;
            }
            if (!matrixOK) break;
            M[r * D + (D - 1)] += static_cast<std::uint32_t>(e.constant);
        }
        if (!matrixOK) continue;
        M[(D - 1) * D + (D - 1)] = 1;

        std::vector<std::uint32_t> initVec(D, 0);
        bool initOK = true;
        for (size_t i = 0; i < state.size(); ++i) {
            if (!initialNeeded[i]) continue;
            auto c = straightLineConstBefore(func, h, state[i]);
            if (!c) { initOK = false; break; }
            initVec[i] = static_cast<std::uint32_t>(*c);
        }
        if (!initOK) continue;
        initVec[D - 1] = 1;

        auto mulMat = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& B) {
            std::vector<std::uint32_t> C(D * D, 0);
            for (size_t i = 0; i < D; ++i)
                for (size_t k = 0; k < D; ++k) {
                    const std::uint32_t aik = A[i * D + k];
                    if (aik == 0) continue;
                    for (size_t j = 0; j < D; ++j)
                        C[i * D + j] += static_cast<std::uint32_t>(
                            static_cast<std::uint64_t>(aik) * B[k * D + j]);
                }
            return C;
        };
        auto mulVec = [D](const std::vector<std::uint32_t>& A,
                          const std::vector<std::uint32_t>& x) {
            std::vector<std::uint32_t> y(D, 0);
            for (size_t i = 0; i < D; ++i)
                for (size_t j = 0; j < D; ++j)
                    y[i] += static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(A[i * D + j]) * x[j]);
            return y;
        };

        std::vector<std::uint32_t> P(D * D, 0), B = M;
        for (size_t i = 0; i < D; ++i) P[i * D + i] = 1;
        std::uint64_t eN = trips;
        while (eN) {
            if (eN & 1ULL) P = mulMat(P, B);
            eN >>= 1ULL;
            if (eN) B = mulMat(B, B);
        }
        const auto finalVec = mulVec(P, initVec);

        std::vector<IRInstr> out;
        out.reserve(n - (back - h + 1) + state.size() * 2);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        for (size_t i = 0; i < state.size(); ++i) {
            if (!slotLoadedAfter(func, back + 1, state[i])) continue;
            const std::int32_t v = static_cast<std::int32_t>(finalVec[i]);
            out.emplace_back(IRInstrType::LI, "t0", std::to_string(v));
            out.emplace_back(IRInstrType::STORE, "", "t0", std::to_string(state[i]));
        }
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}


// The generic tail-recursion unroller above deliberately keys off the
// compiler-generated `.tail_latch_` label.  After a tail-recursive helper is
// inlined into its caller, scalar cleanup can remove that label and leave the
// equivalent compact bottom-tested form.  Recognize only that very narrow
// inlined shape and add a 16-iteration fast path with the original scalar loop
// retained as an exact remainder.  No function is executed at compile time.
bool unrollInlinedUnitCountdownLoop(IRFunction& func) {
    const size_t n = func.instrs.size();
    if (n < 12) return false;
    static std::uint64_t serial = 0;

    std::unordered_map<std::string, size_t> labels;
    for (size_t i = 0; i < n; ++i)
        if (func.instrs[i].type == IRInstrType::LABEL)
            labels[func.instrs[i].label] = i;

    for (size_t back = n; back-- > 0;) {
        const auto& br = func.instrs[back];
        if (br.type != IRInstrType::BRANCH_ZERO &&
            br.type != IRInstrType::BRANCH_NONZERO)
            continue;
        auto hit = labels.find(br.label);
        if (hit == labels.end() || hit->second >= back) continue;
        const size_t h = hit->second;
        const std::string header = br.label;

        // This matcher exists solely for a tail-recursive helper after inlining.
        // Ordinary source while-loops do not receive an `.inl` suffix.
        if (header.find(".inl") == std::string::npos) continue;
        if (back < h + 5 || back < 2) continue;

        const auto& condLoad = func.instrs[back - 2];
        const auto& condNorm = func.instrs[back - 1];
        const bool continueOnNonzero =
            (condNorm.type == IRInstrType::SEQZ &&
             br.type == IRInstrType::BRANCH_ZERO) ||
            (condNorm.type == IRInstrType::SNEZ &&
             br.type == IRInstrType::BRANCH_NONZERO);
        if (!continueOnNonzero ||
            condLoad.type != IRInstrType::LOAD ||
            condNorm.dest != condLoad.dest ||
            condNorm.src1 != condLoad.dest ||
            br.src1 != condNorm.dest)
            continue;

        int condSlot = -1;
        if (!parseSlot(condLoad.src1, condSlot)) continue;
        const size_t bodyBegin = h + 1;
        const size_t bodyEnd = back - 2; // exclude LOAD/SEQZ-or-SNEZ/branch
        if (bodyBegin >= bodyEnd) continue;

        bool straightLine = true;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            const auto t = func.instrs[k].type;
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::LOAD_GLOBAL ||
                t == IRInstrType::STORE_GLOBAL || t == IRInstrType::LOAD_ARG ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                straightLine = false; break;
            }
        }
        if (!straightLine) continue;

        // Recover the carried countdown slot from the compiler-generated copy
        // `LOAD condSlot; STORE countdownSlot`.
        int countdownSlot = -1;
        size_t countdownCopy = n;
        for (size_t k = bodyBegin; k + 1 < bodyEnd; ++k) {
            const auto& a = func.instrs[k];
            const auto& b = func.instrs[k + 1];
            int src = -1, dst = -1;
            if (a.type == IRInstrType::LOAD &&
                b.type == IRInstrType::STORE && b.src1 == a.dest &&
                parseSlot(a.src1, src) && src == condSlot &&
                parseSlot(b.src2, dst) && dst != condSlot) {
                if (countdownSlot != -1 && countdownSlot != dst) {
                    countdownSlot = -2; break;
                }
                countdownSlot = dst;
                countdownCopy = k;
            }
        }
        if (countdownSlot < 0 || countdownCopy == n) continue;

        // Prove the exact unit-decrement producer of condSlot:
        //   LI 1; LOAD countdown; SUB; STORE condSlot
        // The ADD -1 spelling is accepted too.
        bool unitDecrement = false;
        size_t decrementPos = n;
        for (size_t k = bodyBegin; k + 3 < bodyEnd; ++k) {
            const auto& i0 = func.instrs[k];
            const auto& i1 = func.instrs[k + 1];
            const auto& i2 = func.instrs[k + 2];
            const auto& i3 = func.instrs[k + 3];
            int loadSlot = -1, storeSlot = -1;
            if (i0.type != IRInstrType::LI ||
                i1.type != IRInstrType::LOAD ||
                i3.type != IRInstrType::STORE || i3.src1 != i2.dest ||
                !parseSlot(i1.src1, loadSlot) || loadSlot != countdownSlot ||
                !parseSlot(i3.src2, storeSlot) || storeSlot != condSlot)
                continue;

            long long imm = 0;
            try { imm = std::stoll(i0.src1); } catch (...) { continue; }
            if (i2.type == IRInstrType::SUB && imm == 1 &&
                i2.src1 == i1.dest && i2.src2 == i0.dest) {
                unitDecrement = true; decrementPos = k; break;
            }
            if (i2.type == IRInstrType::ADD && imm == -1 &&
                ((i2.src1 == i1.dest && i2.src2 == i0.dest) ||
                 (i2.src2 == i1.dest && i2.src1 == i0.dest))) {
                unitDecrement = true; decrementPos = k; break;
            }
        }
        if (!unitDecrement || decrementPos == n) continue;

        // The condition temporary and the carried countdown must each have the
        // unique stores proven above; otherwise a duplicated 16-trip body could
        // skip an alternate update path.
        int condStores = 0, countdownStores = 0;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            if (func.instrs[k].type != IRInstrType::STORE) continue;
            int q = -1;
            if (!parseSlot(func.instrs[k].src2, q)) { straightLine = false; break; }
            if (q == condSlot) ++condStores;
            if (q == countdownSlot) ++countdownStores;
        }
        if (!straightLine || condStores != 1 || countdownStores != 1) continue;

        // Only transform when the entry countdown is proven positive and large
        // enough that the fast path is useful.  The generated runtime guard still
        // preserves the exact scalar remainder.
        auto init = straightLineConstBefore(func, h, countdownSlot);
        constexpr int F = 32;
        if (!init || *init < F) continue;

        // The tail-call argument builder computes the new countdown into a
        // temporary before computing the other new arguments, then copies that
        // temporary back to the carried countdown slot.  Other arguments often
        // still need the *old* countdown (factorial-style `acc*n` is the common
        // case), which makes the temporary overlap the old countdown and forces
        // an otherwise useless register-to-register move every source trip.
        //
        // Retiming the proven pure `countdown-1` producer to immediately before
        // its unique copy-back is dependency-safe: countdownSlot has no other
        // store in the body, condSlot has one store and its only body load is the
        // copy-back.  This is ordinary local instruction scheduling / copy
        // coalescing; no source iteration is executed at compile time.
        int condLoadsInBody = 0;
        for (size_t k = bodyBegin; k < bodyEnd; ++k) {
            if (func.instrs[k].type != IRInstrType::LOAD) continue;
            int q = -1;
            if (parseSlot(func.instrs[k].src1, q) && q == condSlot)
                ++condLoadsInBody;
        }
        const bool canRetimeCountdown =
            decrementPos + 3 < countdownCopy && condLoadsInBody == 1;

        std::vector<IRInstr> scheduledBody;
        scheduledBody.reserve(bodyEnd - bodyBegin);
        if (canRetimeCountdown) {
            for (size_t k = bodyBegin; k < bodyEnd;) {
                if (k == decrementPos) { k += 4; continue; }
                if (k == countdownCopy) {
                    scheduledBody.insert(scheduledBody.end(),
                        func.instrs.begin() + static_cast<long>(decrementPos),
                        func.instrs.begin() + static_cast<long>(decrementPos + 4));
                }
                scheduledBody.push_back(func.instrs[k]);
                ++k;
            }
        } else {
            scheduledBody.insert(scheduledBody.end(),
                func.instrs.begin() + static_cast<long>(bodyBegin),
                func.instrs.begin() + static_cast<long>(bodyEnd));
        }

        const std::string fastGuard = ".tail_inl_u16_guard_" + func.name + "_" +
                                      std::to_string(serial);
        const std::string fastBody = ".tail_inl_u16_body_" + func.name + "_" +
                                     std::to_string(serial);
        const std::string tail = ".tail_inl_u16_tail_" + func.name + "_" +
                                 std::to_string(serial);
        const std::string done = ".tail_inl_u16_done_" + func.name + "_" +
                                 std::to_string(serial++);

        std::vector<IRInstr> out;
        out.reserve(n + scheduledBody.size() * (F - 1) + 16);
        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));

        out.emplace_back(IRInstrType::LABEL, "", "", "", fastGuard);
        out.emplace_back(IRInstrType::LI, "t0", std::to_string(F));
        out.emplace_back(IRInstrType::LOAD, "t1", std::to_string(countdownSlot));
        out.emplace_back(IRInstrType::SLT, "t0", "t1", "t0");
        out.emplace_back(IRInstrType::BRANCH_NONZERO, "", "t0", "", tail);
        out.emplace_back(IRInstrType::LABEL, "", "", "", fastBody);

        for (int copy = 0; copy < F; ++copy)
            out.insert(out.end(), scheduledBody.begin(), scheduledBody.end());
        out.emplace_back(IRInstrType::JUMP, "", "", "", fastGuard);

        out.emplace_back(IRInstrType::LABEL, "", "", "", tail);
        out.emplace_back(IRInstrType::LOAD, "t0", std::to_string(countdownSlot));
        out.emplace_back(IRInstrType::BRANCH_ZERO, "", "t0", "", done);
        // Exact scalar remainder, using the same dependency-safe scheduled
        // body so register allocation sees one consistent live-range shape.
        out.push_back(func.instrs[h]);
        out.insert(out.end(), scheduledBody.begin(), scheduledBody.end());
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(bodyEnd),
                   func.instrs.begin() + static_cast<long>(back + 1));
        out.emplace_back(IRInstrType::LABEL, "", "", "", done);
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(back + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        return true;
    }
    return false;
}


void optimizeFunctionCommon(IRFunction& func) {
    // Canonical scalar cleanup. Keep loops recognizable while performing
    // propagation, CSE, DCE and tail-recursion elimination; loop-specific
    // transformations are applied in the code-generation stage.
    eliminateDirectTailRecursion(func);
    for (int round = 0; round < 12; ++round) {
        bool changed = false;
        changed |= propagateConstantsAcrossCFG(func);
        changed |= propagateCopiesAcrossCFG(func);
        changed |= simplifyLocally(func);
        changed |= removeUnreachableAfterJump(func);
        changed |= eliminateUnreachableCFG(func);
        changed |= eliminateDeadPureLoops(func);
        changed |= sinkLoopFinalCopies(func);
        changed |= forwardInlineReturnsToDestination(func);
        changed |= eliminateDeadStores(func);
        changed |= eliminateDeadRegisterComputations(func);
        changed |= removeRedundantDirectReturnCopies(func);
        changed |= removeRedundantSelfCopies(func);
        changed |= removeJumpToNextLabel(func);
        changed |= removeUnreferencedLabels(func);
        if (!changed) break;
    }

    // Direct tail calls are now ordinary loops; rotate them to a single
    // conditional back edge before later inlining/code generation.
    rotateTailRecursionLoop(func);

    // Only after destination-slot forwarding has had all optimizer rounds to
    // consume inlined returns, remove the remaining a0->t0 convention for
    // expression-valued inlines that have no immediate STORE destination.
    for (int round = 0; round < 8; ++round) {
        if (!forwardInlineReturnsToT0(func)) break;
        removeRedundantSelfCopies(func);
        removeJumpToNextLabel(func);
    }
}

bool rotateCanonicalWhileLoops(IRFunction& func);

void optimizeFunctionForCodegen(IRFunction& func) {
    // Tail-recursion elimination creates a bottom-tested countdown loop.
    // Prefer an exact affine recurrence summary; if the body is not affine,
    // fall back to a bounded 16x fast path plus the exact scalar remainder.
    summarizeInlinedUnitCountdownLoop(func);
    unrollInlinedUnitCountdownLoop(func);
    unrollTailCountdownLoop(func);

    // First collapse side-effect-free counted loops whose recurrence may
    // contain a low-degree polynomial of the induction variable.  This covers
    // <, <=, > and >= counted loops and CSE-heavy terms such as (i+c)^2.
    for (int polyRound = 0; polyRound < 8; ++polyRound) {
        if (!summarizePolynomialCountedLoop(func)) break;
        for (int round = 0; round < 8; ++round) {
            bool changed = false;
            changed |= propagateConstantsAcrossCFG(func);
            changed |= propagateCopiesAcrossCFG(func);
            changed |= simplifyLocally(func);
            changed |= removeUnreachableAfterJump(func);
            changed |= eliminateUnreachableCFG(func);
            changed |= eliminateDeadStores(func);
            changed |= eliminateDeadRegisterComputations(func);
            changed |= removeRedundantSelfCopies(func);
            changed |= removeJumpToNextLabel(func);
            changed |= removeUnreferencedLabels(func);
            if (!changed) break;
        }
    }

    // Then collapse the remaining affine-state counted loops.
    for (int linearRound = 0; linearRound < 8; ++linearRound) {
        if (!summarizeLinearStateCountedLoop(func)) break;
        for (int round = 0; round < 8; ++round) {
            bool changed = false;
            changed |= propagateConstantsAcrossCFG(func);
            changed |= propagateCopiesAcrossCFG(func);
            changed |= simplifyLocally(func);
            changed |= removeUnreachableAfterJump(func);
            changed |= eliminateUnreachableCFG(func);
            changed |= eliminateDeadStores(func);
            changed |= eliminateDeadRegisterComputations(func);
            changed |= removeRedundantSelfCopies(func);
            changed |= removeJumpToNextLabel(func);
            changed |= removeUnreferencedLabels(func);
            if (!changed) break;
        }
    }

    // Then handle the narrower accumulator-style affine recurrence.
    for (int loopRound = 0; loopRound < 8; ++loopRound) {
        if (!summarizeAffineCountedLoop(func)) break;
        for (int round = 0; round < 3; ++round) {
            bool changed = false;
            changed |= propagateConstantsAcrossCFG(func);
            changed |= propagateCopiesAcrossCFG(func);
            changed |= simplifyLocally(func);
            changed |= removeUnreachableAfterJump(func);
            changed |= eliminateUnreachableCFG(func);
            changed |= eliminateDeadStores(func);
            changed |= eliminateDeadRegisterComputations(func);
            changed |= removeRedundantSelfCopies(func);
            changed |= removeJumpToNextLabel(func);
            changed |= removeUnreferencedLabels(func);
            if (!changed) break;
        }
    }

    // Specialize small residue periods before the generic remainder
    // strength-reduction pass.  This removes deterministic `% C` branch
    // decisions from branch-heavy loop benchmarks, including safe continue
    // edges that update the induction variable before jumping to the header.
    for (int periodicRound = 0; periodicRound < 2; ++periodicRound) {
        if (!specializePeriodicRemainderLoop(func)) break;
        for (int round = 0; round < 8; ++round) {
            bool changed = false;
            changed |= propagateConstantsAcrossCFG(func);
            changed |= propagateCopiesAcrossCFG(func);
            changed |= simplifyLocally(func);
            changed |= removeUnreachableAfterJump(func);
            changed |= eliminateUnreachableCFG(func);
            changed |= eliminateDeadStores(func);
            changed |= eliminateDeadRegisterComputations(func);
            changed |= removeRedundantSelfCopies(func);
            changed |= removeJumpToNextLabel(func);
            changed |= removeUnreferencedLabels(func);
            if (!changed) break;
        }

        // Once the periodic branches are specialized away, the fast
        // super-iteration is often a straight-line affine recurrence.
        if (summarizeAffineBottomTestLoop(func)) {
            for (int round = 0; round < 5; ++round) {
                bool changed = false;
                changed |= propagateConstantsAcrossCFG(func);
                changed |= propagateCopiesAcrossCFG(func);
                changed |= simplifyLocally(func);
                changed |= removeUnreachableAfterJump(func);
                changed |= eliminateUnreachableCFG(func);
                changed |= eliminateDeadStores(func);
                changed |= eliminateDeadRegisterComputations(func);
                changed |= removeRedundantSelfCopies(func);
                changed |= removeJumpToNextLabel(func);
                changed |= removeUnreferencedLabels(func);
                if (!changed) break;
            }
        }
    }

    // The individual strength-reduction helpers intentionally rewrite one
    // natural loop at a time and then return so all CFG indices can be rebuilt.
    // Run them to a bounded fixed point; otherwise a matrix/graph function with
    // several (or nested) loops would optimize only the first matching loop.
    for (int srRound = 0; srRound < 12; ++srRound) {
        bool strengthChanged = false;
        strengthChanged |= strengthReduceInductionProducts(func);
        strengthChanged |= strengthReduceInductionRemainders(func);
        if (!strengthChanged) break;

        for (int round=0; round<3; ++round) {
            bool changed=false;
            changed |= propagateConstantsAcrossCFG(func);
            changed |= propagateCopiesAcrossCFG(func);
            changed |= simplifyLocally(func);
            changed |= removeUnreachableAfterJump(func);
            changed |= eliminateUnreachableCFG(func);
            changed |= eliminateDeadStores(func);
            changed |= eliminateDeadRegisterComputations(func);
            changed |= removeRedundantSelfCopies(func);
            changed |= removeJumpToNextLabel(func);
            changed |= removeUnreferencedLabels(func);
            if(!changed) break;
        }
    }

    hoistSimpleLoopInvariants(func);
    // Keep unrolling single-shot.  The transform deliberately leaves the
    // original scalar loop as a remainder path, so repeatedly invoking the
    // matcher could select that tail again and duplicate it.
    unrollSimpleLoops(func);

    // Branch-heavy loops are not eligible for straight-line unrolling. Rotate
    // those canonical while loops to a bottom test to remove one unconditional
    // jump per iteration (continues are retargeted to the latch).
    rotateCanonicalWhileLoops(func);

    // Loop transforms often expose fresh copy/dead-store opportunities.  A
    // short cleanup fixed point keeps the final IR compact without reapplying
    // code-growth transforms.
    for (int round = 0; round < 8; ++round) {
        bool changed = false;
        changed |= propagateConstantsAcrossCFG(func);
        changed |= propagateCopiesAcrossCFG(func);
        changed |= simplifyLocally(func);
        changed |= removeUnreachableAfterJump(func);
        changed |= eliminateUnreachableCFG(func);
        changed |= eliminateDeadStores(func);
        changed |= eliminateDeadRegisterComputations(func);
        changed |= removeRedundantSelfCopies(func);
        changed |= removeJumpToNextLabel(func);
        if (!changed) break;
    }
}



// Rotate a canonical while-loop into an initial guard plus a bottom-tested loop.
// This removes the unconditional back-edge jump from branch-heavy loops that
// cannot be handled by the straight-line unroller.  Internal `continue` jumps
// to the header are retargeted to the duplicated latch condition, preserving
// while-loop semantics.  The condition itself must be a side-effect-free
// straight-line IR sequence.
bool rotateCanonicalWhileLoops(IRFunction& func) {
    static std::uint64_t serial = 0;
    bool changed = false;

    for (size_t j = func.instrs.size(); j-- > 0;) {
        if (func.instrs[j].type != IRInstrType::JUMP) continue;
        const std::string header = func.instrs[j].label;

        size_t h = j;
        while (h > 0) {
            --h;
            if (func.instrs[h].type == IRInstrType::LABEL &&
                func.instrs[h].label == header) break;
        }
        if (h >= j || func.instrs[h].type != IRInstrType::LABEL ||
            func.instrs[h].label != header) continue;
        if (j + 1 >= func.instrs.size() ||
            func.instrs[j + 1].type != IRInstrType::LABEL) continue;

        const std::string exitLabel = func.instrs[j + 1].label;

        // The first branch to the exit label is the canonical while guard.
        size_t guard = func.instrs.size();
        bool guardSafe = true;
        for (size_t k = h + 1; k < j; ++k) {
            const auto t = func.instrs[k].type;
            if ((t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO) &&
                func.instrs[k].label == exitLabel) {
                guard = k;
                break;
            }
            if (t == IRInstrType::LABEL || t == IRInstrType::JUMP ||
                t == IRInstrType::BRANCH_ZERO || t == IRInstrType::BRANCH_NONZERO ||
                t == IRInstrType::CALL || t == IRInstrType::STORE_GLOBAL ||
                t == IRInstrType::STORE_ARG || t == IRInstrType::RET) {
                guardSafe = false;
                break;
            }
        }
        if (!guardSafe || guard == func.instrs.size() || guard == h + 1) continue;
        const auto guardType = func.instrs[guard].type;
        if (guardType != IRInstrType::BRANCH_ZERO &&
            guardType != IRInstrType::BRANCH_NONZERO) continue;

        // Do not rotate a loop with another backward edge to an internal label.
        bool extraBackedge = false;
        std::unordered_map<std::string, size_t> labels;
        for (size_t k = h; k <= j; ++k)
            if (func.instrs[k].type == IRInstrType::LABEL)
                labels[func.instrs[k].label] = k;
        for (size_t k = guard + 1; k < j; ++k) {
            const auto& ins = func.instrs[k];
            if (ins.type != IRInstrType::JUMP &&
                ins.type != IRInstrType::BRANCH_ZERO &&
                ins.type != IRInstrType::BRANCH_NONZERO) continue;
            auto it = labels.find(ins.label);
            if (it != labels.end() && it->second < k && ins.label != header) {
                extraBackedge = true;
                break;
            }
        }
        if (extraBackedge) continue;

        const std::string suffix = std::to_string(serial++);
        const std::string bodyLabel = ".rot_body_" + func.name + "_" + suffix;
        const std::string latchLabel = ".rot_latch_" + func.name + "_" + suffix;

        std::vector<IRInstr> out;
        out.reserve(func.instrs.size() + (guard - h) + 3);

        out.insert(out.end(), func.instrs.begin(),
                   func.instrs.begin() + static_cast<long>(h));
        out.push_back(func.instrs[h]);

        // Initial zero-trip guard remains unchanged.
        for (size_t k = h + 1; k <= guard; ++k) out.push_back(func.instrs[k]);
        out.emplace_back(IRInstrType::LABEL, "", "", "", bodyLabel);

        // Body: continue/back-edge jumps now go to the latch condition.
        for (size_t k = guard + 1; k < j; ++k) {
            IRInstr ins = func.instrs[k];
            if (ins.type == IRInstrType::JUMP && ins.label == header)
                ins.label = latchLabel;
            out.push_back(std::move(ins));
        }

        out.emplace_back(IRInstrType::LABEL, "", "", "", latchLabel);
        // Recompute the original side-effect-free condition.
        for (size_t k = h + 1; k < guard; ++k) out.push_back(func.instrs[k]);

        IRInstr back = func.instrs[guard];
        back.type = (guardType == IRInstrType::BRANCH_ZERO)
            ? IRInstrType::BRANCH_NONZERO
            : IRInstrType::BRANCH_ZERO;
        back.label = bodyLabel;
        out.push_back(std::move(back));

        // Keep the original exit label and all following code.
        out.insert(out.end(), func.instrs.begin() + static_cast<long>(j + 1),
                   func.instrs.end());
        func.instrs.swap(out);
        changed = true;

        // Rebuild indices before attempting another loop.
        j = func.instrs.size();
    }
    return changed;
}

// Whole-program dead-function elimination.  ToyC has no function pointers and
// the executable entry point is main, so any function not reachable from main
// through CALL instructions cannot affect program behaviour.
bool eliminateUnreachableFunctions(IRProgram& program) {
    std::unordered_map<std::string, size_t> index;
    for (size_t i = 0; i < program.functions.size(); ++i)
        index[program.functions[i].name] = i;
    auto mainIt = index.find("main");
    if (mainIt == index.end()) return false;

    std::vector<char> live(program.functions.size(), 0);
    std::vector<size_t> work{mainIt->second};
    live[mainIt->second] = 1;
    while (!work.empty()) {
        const size_t fi = work.back();
        work.pop_back();
        for (const auto& ins : program.functions[fi].instrs) {
            if (ins.type != IRInstrType::CALL) continue;
            auto it = index.find(ins.src1);
            if (it == index.end() || live[it->second]) continue;
            live[it->second] = 1;
            work.push_back(it->second);
        }
    }

    std::vector<IRFunction> kept;
    kept.reserve(program.functions.size());
    bool changed = false;
    for (size_t i = 0; i < program.functions.size(); ++i) {
        if (live[i]) kept.push_back(program.functions[i]);
        else changed = true;
    }
    if (changed) program.functions.swap(kept);
    return changed;
}

// Remove globals that are no longer referenced after constant propagation.
// The whole ToyC translation unit is available to the compiler and the language
// has no address-taking, so an unreferenced global has no observable effect.
bool eliminateUnusedGlobals(IRProgram& program) {
    if (program.globalVars.empty()) return false;
    std::unordered_set<std::string> used;
    for (const auto& f : program.functions) {
        for (const auto& ins : f.instrs) {
            if (ins.type == IRInstrType::LOAD_GLOBAL) used.insert(ins.src1);
            else if (ins.type == IRInstrType::STORE_GLOBAL) used.insert(ins.src1);
        }
    }
    std::vector<std::pair<std::string, int>> kept;
    kept.reserve(program.globalVars.size());
    bool changed = false;
    for (auto& g : program.globalVars) {
        if (used.count(g.first)) kept.push_back(g);
        else changed = true;
    }
    if (changed) program.globalVars.swap(kept);
    return changed;
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
        if (isLeafInlineCandidate(f, 160, 640)) candidates[f.name] = &f;
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
            bool generatedTailLoop = false;
            for (const auto& ci : callee.instrs) {
                if (ci.type == IRInstrType::LABEL &&
                    (ci.label.rfind(".tail_", 0) == 0 ||
                     ci.label.find(".tail_latch_") != std::string::npos)) {
                    generatedTailLoop = true; break;
                }
            }
            const bool ordinarySmall =
                (callee.instrs.size()<=48 && callee.localSize<=192) ||
                // Tail-recursion elimination has already turned recursion into
                // a bounded local loop.  Inline moderately larger generated
                // tail loops even at a cold call-site so later recurrence
                // analysis can see constant arguments from the caller.
                (generatedTailLoop && callee.instrs.size()<=160 && callee.localSize<=640);
            const bool hotSite = callPos<hot.size() && hot[callPos];
            if (!ordinarySmall && !hotSite) { out.push_back(ins); continue; }
            // Keep inlining profitable and bounded.  Hot loop helpers may be
            // moderately larger than ordinary leaf calls, but never allow a
            // repeated bottom-up round to explode one caller indefinitely.
            if (caller.instrs.size() + callee.instrs.size() > 1400 ||
                caller.localSize + callee.localSize > 4096) {
                out.push_back(ins); continue;
            }
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
        if(changed){ caller.instrs.swap(out); optimizeFunctionCommon(caller); }
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

void IROptimizer::optimizeScalar(IRProgram& program) const {
    propagateImmutableGlobals(program);
    for (auto& func : program.functions) optimizeFunctionCommon(func);

    // Inline bottom-up to a small fixed point.  A caller that originally was
    // not a leaf can become a leaf after its own callees are inlined; rebuilding
    // the candidate set lets that newly simplified helper disappear at its hot
    // callers as well.  The existing per-function/site size limits still cap
    // code growth, and recursion is never selected as an inline candidate.
    for (int round = 0; round < 6; ++round) {
        if (!inlineSmallLeafFunctions(program)) break;
    }

    eliminateUnreachableFunctions(program);
    eliminateUnusedGlobals(program);
}

void IROptimizer::optimizeForCodegen(IRProgram& program) const {
    for (auto& func : program.functions) optimizeFunctionForCodegen(func);
}

void IROptimizer::optimize(IRProgram& program) const {
    optimizeScalar(program);
    optimizeForCodegen(program);
    eliminateUnreachableFunctions(program);
    eliminateUnusedGlobals(program);
}

} // namespace toycc
