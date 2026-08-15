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

                // Local common-subexpression elimination. Only reuse values that were
                // materialized in a stack slot in this same basic block.
                std::string key = makeExprKey(ins.type, a, b);
                if (!key.empty()) {
                    auto loc = st.exprLocations.find(key);
                    if (loc != st.exprLocations.end() && st.versionOf(loc->second.slot) == loc->second.version) {
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

void optimizeFunction(IRFunction& func) {
    // A few rounds are enough because each pass only removes/simplifies instructions.
    for (int round = 0; round < 4; ++round) {
        bool changed = false;
        changed |= simplifyLocally(func);
        changed |= removeUnreachableAfterJump(func);
        changed |= eliminateDeadStores(func);
        changed |= eliminateDeadRegisterComputations(func);
        changed |= removeJumpToNextLabel(func);
        if (!changed) break;
    }
}

} // namespace

void IROptimizer::optimize(IRProgram& program) const {
    for (auto& func : program.functions) optimizeFunction(func);
}

} // namespace toycc
