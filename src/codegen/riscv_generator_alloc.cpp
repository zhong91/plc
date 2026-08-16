#include "codegen/riscv_generator.h"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace toycc {
namespace {

bool hasAdditionalLoadOfSlotAlloc(const std::vector<IRInstr>& instrs,
                                  const std::string& slot,
                                  size_t immediateLoadIndex) {
    std::unordered_map<std::string, size_t> labels;
    for (size_t k = 0; k < instrs.size(); ++k)
        if (instrs[k].type == IRInstrType::LABEL) labels[instrs[k].label] = k;

    std::vector<unsigned char> seen(instrs.size(), 0);
    std::vector<size_t> work;
    if (immediateLoadIndex + 1 < instrs.size()) work.push_back(immediateLoadIndex + 1);
    while (!work.empty()) {
        const size_t j = work.back(); work.pop_back();
        if (j >= instrs.size() || seen[j]) continue;
        seen[j] = 1;
        const auto& in = instrs[j];
        if (in.type == IRInstrType::LOAD && in.src1 == slot) return true;
        if (in.type == IRInstrType::STORE && in.src2 == slot) continue;
        if (in.type == IRInstrType::RET) continue;
        if (in.type == IRInstrType::JUMP) {
            auto it = labels.find(in.label);
            if (it != labels.end()) work.push_back(it->second);
            continue;
        }
        if (in.type == IRInstrType::BRANCH_ZERO || in.type == IRInstrType::BRANCH_NONZERO) {
            auto it = labels.find(in.label);
            if (it != labels.end()) work.push_back(it->second);
            if (j + 1 < instrs.size()) work.push_back(j + 1);
            continue;
        }
        if (j + 1 < instrs.size()) work.push_back(j + 1);
    }
    return false;
}

} // namespace

std::vector<std::pair<int, std::string>>
RiscvGenerator::choosePromotedSlots(const IRFunction& func) const {
    const size_t n = func.instrs.size();
    std::unordered_map<std::string, size_t> labelIndex;
    bool hasCall = false;
    for (size_t i = 0; i < n; ++i) {
        if (func.instrs[i].type == IRInstrType::LABEL) {
            labelIndex[func.instrs[i].label] = i;
        } else if (func.instrs[i].type == IRInstrType::CALL) {
            hasCall = true;
        }
    }

    // Spill temporaries that are guaranteed to disappear in a later peephole
    // should not consume scarce promoted registers.
    std::unordered_map<int, bool> eliminatedSpills;
    for (size_t i = 0; i + 3 < n; ++i) {
        const auto& st = func.instrs[i];
        const auto& rhs = func.instrs[i + 1];
        const auto& ld = func.instrs[i + 2];
        const auto& op = func.instrs[i + 3];
        if (st.type != IRInstrType::STORE || st.src1 != "t0") continue;
        if (ld.type != IRInstrType::LOAD || ld.dest != "t1" || ld.src1 != st.src2) continue;
        if (!isCoreBinaryOp(op.type) || op.dest != "t0") continue;
        if (!((op.src1 == "t1" && op.src2 == "t0") ||
              (op.src1 == "t0" && op.src2 == "t1"))) continue;
        const bool simpleRhs = rhs.dest == "t0" &&
            (rhs.type == IRInstrType::LI || rhs.type == IRInstrType::LOAD ||
             rhs.type == IRInstrType::LOAD_ARG || rhs.type == IRInstrType::LOAD_GLOBAL);
        if (simpleRhs && !hasAdditionalLoadOfSlotAlloc(func.instrs, st.src2, i + 2)) {
            eliminatedSpills[std::stoi(st.src2)] = true;
        }
    }

    // Weight instructions inside natural loops.  Nested loops receive repeated
    // boosts so their state is preferred over setup-only locals.
    std::vector<int> loopWeight(n, 1);
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = func.instrs[i];
        if (ins.type != IRInstrType::JUMP &&
            ins.type != IRInstrType::BRANCH_ZERO &&
            ins.type != IRInstrType::BRANCH_NONZERO) continue;
        auto it = labelIndex.find(ins.label);
        if (it == labelIndex.end() || it->second >= i) continue;
        for (size_t j = it->second; j <= i; ++j) loopWeight[j] += 32;
    }

    std::unordered_map<int, long long> score;
    std::unordered_map<int, int> rawCount;
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = func.instrs[i];
        int off = -1;
        if (ins.type == IRInstrType::LOAD) off = std::stoi(ins.src1);
        else if (ins.type == IRInstrType::STORE) off = std::stoi(ins.src2);
        if (off >= 0 && !eliminatedSpills.count(off)) {
            score[off] += loopWeight[i];
            rawCount[off]++;
        }
    }

    struct Candidate { int offset; long long score; int count; };
    std::vector<Candidate> candidates;
    for (const auto& [off, sc] : score) {
        if (rawCount[off] >= 2) candidates.push_back({off, sc, rawCount[off]});
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.count != b.count) return a.count > b.count;
        return a.offset < b.offset;
    });

    // Build the exact instruction CFG and slot liveness.  Unlike the old
    // "one logical slot owns one register for the whole function" policy, this
    // lets disjoint live ranges reuse the same physical register.
    std::vector<std::vector<size_t>> succ(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& ins = func.instrs[i];
        if (ins.type == IRInstrType::RET) continue;
        if (ins.type == IRInstrType::JUMP) {
            auto it = labelIndex.find(ins.label);
            if (it != labelIndex.end()) succ[i].push_back(it->second);
        } else if (ins.type == IRInstrType::BRANCH_ZERO ||
                   ins.type == IRInstrType::BRANCH_NONZERO) {
            auto it = labelIndex.find(ins.label);
            if (it != labelIndex.end()) succ[i].push_back(it->second);
            if (i + 1 < n) succ[i].push_back(i + 1);
        } else if (i + 1 < n) {
            succ[i].push_back(i + 1);
        }
    }

    std::vector<std::unordered_set<int>> liveIn(n), liveOut(n);
    bool livenessChanged = true;
    int rounds = 0;
    while (livenessChanged && rounds++ < static_cast<int>(n) + 16) {
        livenessChanged = false;
        for (size_t ii = n; ii-- > 0;) {
            std::unordered_set<int> no;
            for (size_t s : succ[ii]) no.insert(liveIn[s].begin(), liveIn[s].end());
            std::unordered_set<int> ni = no;
            const auto& ins = func.instrs[ii];
            if (ins.type == IRInstrType::STORE) ni.erase(std::stoi(ins.src2));
            if (ins.type == IRInstrType::LOAD) ni.insert(std::stoi(ins.src1));
            if (no != liveOut[ii] || ni != liveIn[ii]) {
                liveOut[ii] = std::move(no);
                liveIn[ii] = std::move(ni);
                livenessChanged = true;
            }
        }
    }

    std::unordered_set<int> liveAcrossCall;
    for (size_t i = 0; i < n; ++i) {
        if (func.instrs[i].type != IRInstrType::CALL) continue;
        liveAcrossCall.insert(liveOut[i].begin(), liveOut[i].end());
    }

    // Interference graph: a newly defined slot conflicts with every value that
    // must remain live afterwards.  This is enough for these initialized local
    // slots and is less conservative than making every block-live set a clique.
    std::unordered_map<int, std::unordered_set<int>> interferes;
    auto addEdge = [&](int a, int b) {
        if (a == b) return;
        interferes[a].insert(b);
        interferes[b].insert(a);
    };
    for (size_t i = 0; i < n; ++i) {
        if (func.instrs[i].type != IRInstrType::STORE) continue;
        int d = std::stoi(func.instrs[i].src2);
        for (int s : liveOut[i]) addEdge(d, s);
    }

    // Coalescing hint: LOAD src; STORE dst is a pure copy.  Prefer assigning
    // dst the same register as src when the liveness graph permits it.
    std::unordered_map<int, std::unordered_map<int, long long>> copyAffinity;
    for (size_t i = 0; i + 1 < n; ++i) {
        const auto& a = func.instrs[i];
        const auto& b = func.instrs[i + 1];
        if (a.type == IRInstrType::LOAD && a.dest == "t0" &&
            b.type == IRInstrType::STORE && b.src1 == "t0") {
            int s = std::stoi(a.src1), d = std::stoi(b.src2);
            long long w = static_cast<long long>(std::max(loopWeight[i], loopWeight[i + 1]));
            copyAffinity[d][s] += w;
            copyAffinity[s][d] += w;
        }
    }

    std::vector<std::pair<int, std::string>> result;
    std::unordered_map<int, std::string> slotReg;
    std::unordered_map<std::string, std::vector<int>> regSlots;

    auto conflictsWithReg = [&](int slot, const std::string& reg) {
        auto rs = regSlots.find(reg);
        if (rs == regSlots.end()) return false;
        auto it = interferes.find(slot);
        if (it == interferes.end()) return false;
        for (int other : rs->second) if (it->second.count(other)) return true;
        return false;
    };
    auto assign = [&](int slot, const std::string& reg) {
        slotReg[slot] = reg;
        regSlots[reg].push_back(slot);
        result.push_back({slot, reg});
    };

    std::vector<std::string> zeroCostRegs;
    if (!hasCall) {
        zeroCostRegs = {"t2", "t3", "t4", "t5"};
        // a1..a7 are usable only if the IR never mentions them explicitly.
        for (int ai = 1; ai < 8; ++ai) {
            std::string reg = "a" + std::to_string(ai);
            bool mentioned = false;
            for (const auto& ins : func.instrs) {
                if (ins.dest == reg || ins.src1 == reg || ins.src2 == reg) {
                    mentioned = true; break;
                }
            }
            if (!mentioned) zeroCostRegs.push_back(reg);
        }
    } else {
        zeroCostRegs = {"t2", "t3", "t4", "t5"};
    }

    std::vector<std::string> savedRegs = {
        "s0", "s1", "s2", "s3", "s4", "s5",
        "s6", "s7", "s8", "s9", "s10", "s11"
    };

    auto allowedZeroCost = [&](int slot, const std::string& reg) {
        if (!hasCall) return true;
        // t2..t5 are caller-saved.  A slot may use them only if its value is
        // dead at every CALL.  (Non-leaf a-register allocation is intentionally
        // avoided because argument setup uses fixed a0..a7 names in the IR.)
        return reg.size() == 2 && reg[0] == 't' && !liveAcrossCall.count(slot);
    };

    // Greedy weighted coloring.  First try a coalescing-friendly register used
    // by a copy-related neighbor, then any zero-save-cost color, then an already
    // opened saved register, and finally open a new saved register only when the
    // static/hotness score can amortize its save+restore.
    for (const auto& c : candidates) {
        if (slotReg.count(c.offset)) continue;

        std::vector<std::pair<long long, std::string>> preferred;
        auto aff = copyAffinity.find(c.offset);
        if (aff != copyAffinity.end()) {
            for (const auto& [other, w] : aff->second) {
                auto sr = slotReg.find(other);
                if (sr != slotReg.end()) preferred.push_back({w, sr->second});
            }
            std::sort(preferred.begin(), preferred.end(),
                      [](const auto& a, const auto& b) { return a.first > b.first; });
        }

        bool placed = false;
        for (const auto& [w, reg] : preferred) {
            (void)w;
            const bool isSaved = !reg.empty() && reg[0] == 's';
            if (!isSaved && !allowedZeroCost(c.offset, reg)) continue;
            if (!conflictsWithReg(c.offset, reg)) {
                assign(c.offset, reg); placed = true; break;
            }
        }
        if (placed) continue;

        for (const auto& reg : zeroCostRegs) {
            if (!allowedZeroCost(c.offset, reg)) continue;
            if (!conflictsWithReg(c.offset, reg)) {
                assign(c.offset, reg); placed = true; break;
            }
        }
        if (placed) continue;

        // Reusing an already-open saved register has no additional frame cost,
        // so allow even relatively cold slots if they have at least two accesses.
        for (const auto& reg : savedRegs) {
            if (!regSlots.count(reg)) continue;
            if (!conflictsWithReg(c.offset, reg)) {
                assign(c.offset, reg); placed = true; break;
            }
        }
        if (placed) continue;

        if (c.score < 66 && c.count < 6) continue;
        for (const auto& reg : savedRegs) {
            if (regSlots.count(reg)) continue;
            assign(c.offset, reg); placed = true; break;
        }
    }

    return result;
}


} // namespace toycc
