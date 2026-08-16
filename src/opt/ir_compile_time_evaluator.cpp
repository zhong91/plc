#include "opt/ir_compile_time_evaluator.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace toycc {
namespace {

enum class Op : std::uint8_t {
    Nop, Li, Load, Store, LoadArg, StoreArg, LoadGlobal, StoreGlobal,
    Mv, Add, Sub, Mul, Div, Rem, Slt, Seqz, Snez,
    Jump, Bz, Bnz, Call, Ret,
    // Host-evaluation superinstructions. They keep the original code index
    // layout (branch targets stay valid) but skip the fused trailing ops.
    StoreImm, CopyLocal, AddLocImm, SubLocImm, MulLocImm, DivLocImm, RemLocImm,
    AddLocLoc, SubLocLoc, MulLocLoc, DivLocLoc, RemLocLoc,
    AddMulLocImm, SubMulLocImm, AddMulLocLoc, SubMulLocLoc,
    BrLocLtImm, BrLocGeImm, BrLocGtImm, BrLocLeImm, BrLocEqImm, BrLocNeImm,
    BrLocLtLoc, BrLocGeLoc, BrLocGtLoc, BrLocLeLoc, BrLocEqLoc, BrLocNeLoc,
    FastLinearLoop, FastPolyLoop
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
    std::uint8_t skip = 0; // number of following original bytecodes fused into this op
    std::int32_t imm = 0;  // immediate / slot / global index / target pc / function index
    std::int32_t aux = 0;  // second immediate, usually destination slot or branch target
};

struct LinearUpdate { std::int16_t slot=-1; std::int32_t delta=0; std::int32_t ivCoeff=0; std::int32_t mulFactor=1; std::int16_t ivScaleSlot=-1; };
struct FastLoop {
    Op exitOp=Op::BrLocGeImm;
    std::int16_t iv=-1;
    std::int32_t bound=0;
    std::int16_t boundSlot=-1;
    std::int32_t step=0;
    std::int32_t exitPc=0;
    std::vector<LinearUpdate> updates;
};
struct PolyLoop {
    Op exitOp=Op::BrLocGeImm;
    std::int16_t iv=-1;
    std::int32_t bound=0;
    std::int16_t boundSlot=-1;
    std::int32_t exitPc=0;
    std::int32_t bodyBegin=0;
    std::int32_t bodyEnd=0; // exclusive, excludes backward JUMP
    std::vector<std::int16_t> modified;
    std::vector<std::int16_t> liveAfter;
};
struct Fn {
    std::vector<BC> code;
    std::vector<FastLoop> fastLoops;
    std::vector<PolyLoop> polyLoops;
    int localWords = 0;
    int outgoingWords = 0;
    int paramCount = 0;
    bool pure = true;
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

void fuseSuperInstructions(Fn& fn) {
    auto& c = fn.code;
    const size_t n = c.size();
    auto is = [&](size_t i, Op op) { return i < n && c[i].op == op; };

    for (size_t i = 0; i < n; ++i) {
        // LI t0,imm ; STORE t0,slot
        if (i + 1 < n && is(i, Op::Li) && c[i].d == R_T0 &&
            is(i + 1, Op::Store) && c[i + 1].a == R_T0) {
            BC y; y.op = Op::StoreImm; y.d = static_cast<std::int16_t>(c[i + 1].imm);
            y.imm = c[i].imm; y.skip = 1; c[i] = y; continue;
        }

        // LOAD t0,src ; STORE t0,dst
        if (i + 1 < n && is(i, Op::Load) && c[i].d == R_T0 &&
            is(i + 1, Op::Store) && c[i + 1].a == R_T0) {
            BC y; y.op = Op::CopyLocal; y.a = static_cast<std::int16_t>(c[i].imm);
            y.d = static_cast<std::int16_t>(c[i + 1].imm); y.skip = 1; c[i] = y; continue;
        }

        // LI rhs ; LOAD lhs ; OP t0,t1,t0 ; STORE t0,dst
        if (i + 3 < n && is(i, Op::Li) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            c[i + 2].d == R_T0 && c[i + 2].a == 1 && c[i + 2].b == R_T0 &&
            is(i + 3, Op::Store) && c[i + 3].a == R_T0) {
            Op fused = Op::Nop;
            switch (c[i + 2].op) {
                case Op::Add: fused = Op::AddLocImm; break;
                case Op::Sub: fused = Op::SubLocImm; break;
                case Op::Mul: fused = Op::MulLocImm; break;
                case Op::Div: fused = Op::DivLocImm; break;
                case Op::Rem: fused = Op::RemLocImm; break;
                default: break;
            }
            if (fused != Op::Nop) {
                BC y; y.op=fused; y.a=static_cast<std::int16_t>(c[i + 1].imm);
                y.d=static_cast<std::int16_t>(c[i + 3].imm); y.imm=c[i].imm; y.skip=3;
                c[i]=y; continue;
            }
        }

        // LOAD rhs ; LOAD lhs ; OP t0,t1,t0 ; STORE t0,dst
        if (i + 3 < n && is(i, Op::Load) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            c[i + 2].d == R_T0 && c[i + 2].a == 1 && c[i + 2].b == R_T0 &&
            is(i + 3, Op::Store) && c[i + 3].a == R_T0) {
            Op fused = Op::Nop;
            switch (c[i + 2].op) {
                case Op::Add: fused = Op::AddLocLoc; break;
                case Op::Sub: fused = Op::SubLocLoc; break;
                case Op::Mul: fused = Op::MulLocLoc; break;
                case Op::Div: fused = Op::DivLocLoc; break;
                case Op::Rem: fused = Op::RemLocLoc; break;
                default: break;
            }
            if (fused != Op::Nop) {
                BC y; y.op=fused; y.a=static_cast<std::int16_t>(c[i + 1].imm);
                y.b=static_cast<std::int16_t>(c[i].imm); y.d=static_cast<std::int16_t>(c[i + 3].imm);
                y.skip=3; c[i]=y; continue;
            }
        }

        // Fused multiply-accumulate reductions emitted by nested
        // expressions: acc = acc +/- (x * C).
        if (i + 5 < n && is(i, Op::Load) && c[i].d == R_T0 &&
            is(i + 1, Op::Li) && c[i + 1].d == 1 &&
            is(i + 2, Op::Mul) && c[i + 2].d == R_T0 &&
            ((c[i + 2].a == 1 && c[i + 2].b == R_T0) || (c[i + 2].a == R_T0 && c[i + 2].b == 1)) &&
            is(i + 3, Op::Load) && c[i + 3].d == 1 &&
            (is(i + 4, Op::Add) || is(i + 4, Op::Sub)) && c[i + 4].d == R_T0 &&
            c[i + 4].a == 1 && c[i + 4].b == R_T0 &&
            is(i + 5, Op::Store) && c[i + 5].a == R_T0) {
            BC y; y.op=is(i + 4, Op::Add)?Op::AddMulLocImm:Op::SubMulLocImm;
            y.a=static_cast<std::int16_t>(c[i].imm); y.d=static_cast<std::int16_t>(c[i + 5].imm);
            y.imm=c[i + 1].imm; y.skip=5; c[i]=y; continue;
        }
        // acc = acc +/- (x * y).
        if (i + 5 < n && is(i, Op::Load) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            is(i + 2, Op::Mul) && c[i + 2].d == R_T0 &&
            ((c[i + 2].a == 1 && c[i + 2].b == R_T0) || (c[i + 2].a == R_T0 && c[i + 2].b == 1)) &&
            is(i + 3, Op::Load) && c[i + 3].d == 1 &&
            (is(i + 4, Op::Add) || is(i + 4, Op::Sub)) && c[i + 4].d == R_T0 &&
            c[i + 4].a == 1 && c[i + 4].b == R_T0 &&
            is(i + 5, Op::Store) && c[i + 5].a == R_T0) {
            BC y; y.op=is(i + 4, Op::Add)?Op::AddMulLocLoc:Op::SubMulLocLoc;
            y.a=static_cast<std::int16_t>(c[i].imm); y.b=static_cast<std::int16_t>(c[i + 1].imm);
            y.d=static_cast<std::int16_t>(c[i + 5].imm); y.skip=5; c[i]=y; continue;
        }

        // LI rhs ; LOAD lhs ; SLT t0,t1,t0 ; [SEQZ] ; BRANCH
        if (i + 3 < n && is(i, Op::Li) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            is(i + 2, Op::Slt) && c[i + 2].d == R_T0) {
            bool reversed = c[i + 2].a == R_T0 && c[i + 2].b == 1; // imm < local
            bool normal = c[i + 2].a == 1 && c[i + 2].b == R_T0;   // local < imm
            if (!reversed && !normal) continue;
            size_t bi = i + 3;
            bool negated = false;
            if (bi < n && is(bi, Op::Seqz) && c[bi].d == R_T0 && c[bi].a == R_T0) {
                negated = true; ++bi;
            }
            if (bi >= n || (c[bi].op != Op::Bz && c[bi].op != Op::Bnz) || c[bi].a != R_T0) continue;
            // condition before branch: normal ? (local<imm) : (imm<local).
            // SEQZ negates it; Bz negates once more, Bnz keeps it.
            bool branchOnCond = (c[bi].op == Op::Bnz);
            if (negated) branchOnCond = !branchOnCond;
            Op bop = Op::Nop;
            if (normal) bop = branchOnCond ? Op::BrLocLtImm : Op::BrLocGeImm;
            else bop = branchOnCond ? Op::BrLocGtImm : Op::BrLocLeImm;
            BC y; y.op=bop; y.a=static_cast<std::int16_t>(c[i + 1].imm); y.imm=c[i].imm;
            y.aux=c[bi].imm; y.skip=static_cast<std::uint8_t>(bi-i); c[i]=y; continue;
        }

        // LOAD rhs ; LOAD lhs ; SLT t0,t1,t0 ; [SEQZ] ; BRANCH.
        // This preserves local-vs-local comparisons as a single host operation,
        // which lets nested loops use an outer local as the inner bound.
        if (i + 3 < n && is(i, Op::Load) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            is(i + 2, Op::Slt) && c[i + 2].d == R_T0) {
            bool reversed = c[i + 2].a == R_T0 && c[i + 2].b == 1;
            bool normal = c[i + 2].a == 1 && c[i + 2].b == R_T0;
            if (reversed || normal) {
                size_t bi=i+3; bool negated=false;
                if (bi<n && is(bi,Op::Seqz) && c[bi].d==R_T0 && c[bi].a==R_T0){negated=true;++bi;}
                if (bi<n && (c[bi].op==Op::Bz||c[bi].op==Op::Bnz) && c[bi].a==R_T0) {
                    bool branchOnCond=(c[bi].op==Op::Bnz); if(negated)branchOnCond=!branchOnCond;
                    Op bop=Op::Nop;
                    if(normal)bop=branchOnCond?Op::BrLocLtLoc:Op::BrLocGeLoc;
                    else bop=branchOnCond?Op::BrLocGtLoc:Op::BrLocLeLoc;
                    BC y;y.op=bop;y.a=static_cast<int16_t>(c[i+1].imm);y.b=static_cast<int16_t>(c[i].imm);
                    y.aux=c[bi].imm;y.skip=static_cast<uint8_t>(bi-i);c[i]=y;continue;
                }
            }
        }

        // LOAD rhs ; LOAD lhs ; SUB ; SEQZ/SNEZ ; BRANCH.
        if (i + 4 < n && is(i,Op::Load) && c[i].d==R_T0 &&
            is(i+1,Op::Load) && c[i+1].d==1 && is(i+2,Op::Sub) &&
            c[i+2].d==R_T0 && c[i+2].a==1 && c[i+2].b==R_T0 &&
            (is(i+3,Op::Seqz)||is(i+3,Op::Snez)) && c[i+3].d==R_T0 && c[i+3].a==R_T0 &&
            (is(i+4,Op::Bz)||is(i+4,Op::Bnz)) && c[i+4].a==R_T0) {
            bool exprEq=is(i+3,Op::Seqz), branchOnTrue=is(i+4,Op::Bnz);
            bool wantEq=branchOnTrue?exprEq:!exprEq;
            BC y;y.op=wantEq?Op::BrLocEqLoc:Op::BrLocNeLoc;
            y.a=static_cast<int16_t>(c[i+1].imm);y.b=static_cast<int16_t>(c[i].imm);y.aux=c[i+4].imm;y.skip=4;c[i]=y;continue;
        }

        // LI rhs ; LOAD lhs ; SUB t0,t1,t0 ; SEQZ/SNEZ ; BRANCH
        if (i + 4 < n && is(i, Op::Li) && c[i].d == R_T0 &&
            is(i + 1, Op::Load) && c[i + 1].d == 1 &&
            is(i + 2, Op::Sub) && c[i + 2].d == R_T0 && c[i + 2].a == 1 && c[i + 2].b == R_T0 &&
            (is(i + 3, Op::Seqz) || is(i + 3, Op::Snez)) && c[i + 3].d == R_T0 && c[i + 3].a == R_T0 &&
            (is(i + 4, Op::Bz) || is(i + 4, Op::Bnz)) && c[i + 4].a == R_T0) {
            bool exprEq = is(i + 3, Op::Seqz);
            bool branchOnTrue = is(i + 4, Op::Bnz);
            bool wantEq = branchOnTrue ? exprEq : !exprEq;
            BC y; y.op=wantEq?Op::BrLocEqImm:Op::BrLocNeImm;
            y.a=static_cast<std::int16_t>(c[i + 1].imm); y.imm=c[i].imm; y.aux=c[i + 4].imm; y.skip=4;
            c[i]=y; continue;
        }
    }
}

void fuseLinearLoops(Fn& fn) {
    auto& c=fn.code; const size_t n=c.size();
    auto nextEffective=[&](size_t& k){ while(k<n && c[k].op==Op::Nop) ++k; };
    auto slotReferencedOutsidePair=[&](int slot,size_t p0,size_t p1){
        for(size_t z=0;z<n;++z){
            if(z>=p0&&z<=p1) continue;
            const BC& q=c[z];
            auto eq=[&](int x){return x==slot;};
            switch(q.op){
                case Op::Load: if(eq(q.imm)) return true; break;
                case Op::Store: if(eq(q.imm)) return true; break;
                case Op::StoreImm: if(eq(q.d)) return true; break;
                case Op::CopyLocal: if(eq(q.a)||eq(q.d)) return true; break;
                case Op::AddLocImm: case Op::SubLocImm: case Op::MulLocImm: case Op::DivLocImm: case Op::RemLocImm:
                    if(eq(q.a)||eq(q.d)) return true; break;
                case Op::AddLocLoc: case Op::SubLocLoc: case Op::MulLocLoc: case Op::DivLocLoc: case Op::RemLocLoc:
                    if(eq(q.a)||eq(q.b)||eq(q.d)) return true; break;
                case Op::AddMulLocImm: case Op::SubMulLocImm:
                    if(eq(q.a)||eq(q.d)) return true; break;
                case Op::AddMulLocLoc: case Op::SubMulLocLoc:
                    if(eq(q.a)||eq(q.b)||eq(q.d)) return true; break;
                case Op::BrLocLtImm: case Op::BrLocGeImm: case Op::BrLocGtImm: case Op::BrLocLeImm: case Op::BrLocEqImm: case Op::BrLocNeImm:
                    if(eq(q.a)) return true; break;
                case Op::BrLocLtLoc: case Op::BrLocGeLoc: case Op::BrLocGtLoc: case Op::BrLocLeLoc: case Op::BrLocEqLoc: case Op::BrLocNeLoc:
                    if(eq(q.a)||eq(q.b)) return true; break;
                default: break;
            }
        }
        return false;
    };
    for(size_t ji=0;ji<n;++ji){
        if(c[ji].op!=Op::Jump || c[ji].imm<0 || static_cast<size_t>(c[ji].imm)>=ji) continue;
        size_t k=static_cast<size_t>(c[ji].imm); nextEffective(k); if(k>=ji) continue;
        Op exitOp=c[k].op;
        const bool localBound=(exitOp==Op::BrLocGeLoc||exitOp==Op::BrLocGtLoc||exitOp==Op::BrLocLeLoc||exitOp==Op::BrLocLtLoc);
        if(!(exitOp==Op::BrLocGeImm||exitOp==Op::BrLocGtImm||exitOp==Op::BrLocLeImm||exitOp==Op::BrLocLtImm||localBound)) continue;
        const int iv=c[k].a, bound=localBound?0:c[k].imm, boundSlot=localBound?c[k].b:-1, exitPc=c[k].aux;
        if(iv<0) continue;
        std::vector<LinearUpdate> canonical; bool ok=true; int groups=0;
        while(k<ji){
            nextEffective(k); if(k>=ji) break;
            const BC& br=c[k];
            if(br.op!=exitOp||br.a!=iv||br.aux!=exitPc||
               (localBound ? br.b!=boundSlot : br.imm!=bound)){ok=false;break;}
            ++groups; k += 1 + br.skip;
            std::vector<LinearUpdate> ups;
            while(k<ji){
                nextEffective(k); if(k>=ji) break;
                if(c[k].op==exitOp) break;
                if(c[k].op==Op::AddLocImm || c[k].op==Op::SubLocImm){
                    if(c[k].d!=c[k].a){ok=false;break;}
                    int32_t delta=c[k].op==Op::AddLocImm?c[k].imm:-c[k].imm;
                    ups.push_back({c[k].d,delta,0,1,-1}); k += 1 + c[k].skip; continue;
                }
                if(c[k].op==Op::MulLocImm){
                    if(c[k].d!=c[k].a || c[k].d==iv){ok=false;break;}
                    ups.push_back({c[k].d,0,0,c[k].imm,-1}); k += 1 + c[k].skip; continue;
                }
                if(c[k].op==Op::AddMulLocImm || c[k].op==Op::SubMulLocImm) {
                    if(c[k].a!=iv || c[k].d==iv){ok=false;break;}
                    int32_t coeff=(c[k].op==Op::AddMulLocImm?c[k].imm:-c[k].imm);
                    ups.push_back({c[k].d,0,coeff,1,-1}); k += 1 + c[k].skip; continue;
                }
                if(c[k].op==Op::AddMulLocLoc || c[k].op==Op::SubMulLocLoc) {
                    int scale=-1;
                    if(c[k].a==iv && c[k].b!=iv) scale=c[k].b;
                    else if(c[k].b==iv && c[k].a!=iv) scale=c[k].a;
                    if(scale<0 || c[k].d==iv){ok=false;break;}
                    int sign=c[k].op==Op::AddMulLocLoc?1:-1;
                    ups.push_back({c[k].d,0,sign,1,static_cast<int16_t>(scale)}); k += 1 + c[k].skip; continue;
                }

                // Reduction through a single-use multiplication:
                //   tmp = invariant * iv; acc = acc +/- tmp
                // or tmp = iv * C; acc = acc +/- tmp.
                if(c[k].op==Op::MulLocLoc || (c[k].op==Op::MulLocImm && c[k].d!=c[k].a)) {
                    const size_t mulPos=k;
                    int temp=c[k].d, scaleSlot=-1; int32_t scaleImm=1; bool matches=false;
                    if(c[k].op==Op::MulLocLoc) {
                        if(c[k].a==iv && c[k].b!=iv){scaleSlot=c[k].b;matches=true;}
                        else if(c[k].b==iv && c[k].a!=iv){scaleSlot=c[k].a;matches=true;}
                    } else if(c[k].a==iv) { scaleImm=c[k].imm; matches=true; }
                    if(matches && temp!=iv) {
                        size_t nk=k+1+c[k].skip; nextEffective(nk);
                        if(nk<ji && (c[nk].op==Op::AddLocLoc||c[nk].op==Op::SubLocLoc)) {
                            int acc=-1, sign=1;
                            if(c[nk].op==Op::AddLocLoc) {
                                if(c[nk].d==c[nk].a&&c[nk].b==temp) acc=c[nk].d;
                                else if(c[nk].d==c[nk].b&&c[nk].a==temp) acc=c[nk].d;
                            } else if(c[nk].d==c[nk].a&&c[nk].b==temp) { acc=c[nk].d; sign=-1; }
                            // The materialized product slot must exist solely for
                            // this reduction pair; otherwise fast-forwarding would
                            // fail to preserve its final observable value.
                            if(acc>=0 && acc!=iv && !slotReferencedOutsidePair(temp,mulPos,nk)) {
                                ups.push_back({static_cast<int16_t>(acc),0,sign*scaleImm,1,static_cast<int16_t>(scaleSlot)});
                                k=nk+1+c[nk].skip; continue;
                            }
                        }
                    }
                }

                if(c[k].op==Op::AddLocLoc){
                    // Common reduction: acc = acc + inductionVar. Require the
                    // destination to be the accumulator itself; the induction
                    // update is validated below and must occur after this use.
                    int acc=-1;
                    if(c[k].d==c[k].a && c[k].b==iv) acc=c[k].d;
                    else if(c[k].d==c[k].b && c[k].a==iv) acc=c[k].d;
                    if(acc<0||acc==iv){ok=false;break;}
                    ups.push_back({static_cast<int16_t>(acc),0,1,1,-1}); k += 1 + c[k].skip; continue;
                }
                if(c[k].op==Op::SubLocLoc && c[k].d==c[k].a && c[k].b==iv && c[k].d!=iv){
                    ups.push_back({c[k].d,0,-1,1,-1}); k += 1 + c[k].skip; continue;
                }
                ok=false; break;
            }
            if(!ok) break;
            if(canonical.empty()) canonical=ups; else if(ups.size()!=canonical.size()){ok=false;break;} else {
                for(size_t u=0;u<ups.size();++u) if(ups[u].slot!=canonical[u].slot||ups[u].delta!=canonical[u].delta||ups[u].ivCoeff!=canonical[u].ivCoeff||ups[u].mulFactor!=canonical[u].mulFactor||ups[u].ivScaleSlot!=canonical[u].ivScaleSlot){ok=false;break;}
            }
            if(!ok) break;
        }
        if(!ok||groups==0||canonical.empty()) continue;
        int32_t step=0; int ivUpdates=0; bool seenIvUpdate=false; bool orderOK=true;
        for(auto u:canonical) {
            if(u.slot==iv){step+=u.delta;++ivUpdates;seenIvUpdate=true;}
            else if(u.ivCoeff!=0 && seenIvUpdate) orderOK=false;
        }
        if(ivUpdates==0||step==0||!orderOK) continue;
        // A multiplicative recurrence is fast-forwarded only when that slot has
        // no second update in the same iteration; otherwise the update is an
        // affine recurrence and needs a more general transform composition.
        {
            std::unordered_map<int,int> count;
            bool multOK=true;
            for(const auto& u:canonical) ++count[u.slot];
            for(const auto& u:canonical) if(u.mulFactor!=1 && count[u.slot]!=1) { multOK=false; break; }
            for(const auto& u:canonical) if(u.ivScaleSlot>=0 && count.count(u.ivScaleSlot)) { multOK=false; break; }
            if(!multOK) continue;
        }
        bool directionOK=((exitOp==Op::BrLocGeImm||exitOp==Op::BrLocGtImm||exitOp==Op::BrLocGeLoc||exitOp==Op::BrLocGtLoc)&&step>0)||
                         ((exitOp==Op::BrLocLeImm||exitOp==Op::BrLocLtImm||exitOp==Op::BrLocLeLoc||exitOp==Op::BrLocLtLoc)&&step<0);
        if(!directionOK) continue;
        if(localBound){for(const auto& u:canonical)if(u.slot==boundSlot){directionOK=false;break;}if(!directionOK)continue;}
        FastLoop fl;fl.exitOp=exitOp;fl.iv=iv;fl.bound=bound;fl.boundSlot=static_cast<int16_t>(boundSlot);fl.step=step;fl.exitPc=exitPc;fl.updates=canonical;
        int idx=static_cast<int>(fn.fastLoops.size());fn.fastLoops.push_back(std::move(fl));
        BC y; y.op=Op::FastLinearLoop; y.imm=idx; c[static_cast<size_t>(c[ji].imm)]=y;
    }
}


void fusePolynomialLoops(Fn& fn) {
    auto& c=fn.code; const size_t n=c.size();
    auto next=[&](size_t& k){while(k<n&&c[k].op==Op::Nop)++k;};
    auto isBranch=[](Op o){return o==Op::BrLocLtImm||o==Op::BrLocGeImm||o==Op::BrLocGtImm||o==Op::BrLocLeImm||o==Op::BrLocLtLoc||o==Op::BrLocGeLoc||o==Op::BrLocGtLoc||o==Op::BrLocLeLoc;};
    auto invert=[](Op o){
        if(o==Op::BrLocLtImm)return Op::BrLocGeImm;if(o==Op::BrLocGeImm)return Op::BrLocLtImm;if(o==Op::BrLocGtImm)return Op::BrLocLeImm;if(o==Op::BrLocLeImm)return Op::BrLocGtImm;
        if(o==Op::BrLocLtLoc)return Op::BrLocGeLoc;if(o==Op::BrLocGeLoc)return Op::BrLocLtLoc;if(o==Op::BrLocGtLoc)return Op::BrLocLeLoc;return Op::BrLocGtLoc;
    };
    auto addReads=[&](const BC&q,std::unordered_set<int>& live){
        auto add=[&](int z){if(z>=0)live.insert(z);};
        switch(q.op){
            case Op::Load:add(q.imm);break;
            case Op::CopyLocal:add(q.a);break;
            case Op::AddLocImm:case Op::SubLocImm:case Op::MulLocImm:case Op::DivLocImm:case Op::RemLocImm:add(q.a);break;
            case Op::AddLocLoc:case Op::SubLocLoc:case Op::MulLocLoc:case Op::DivLocLoc:case Op::RemLocLoc:add(q.a);add(q.b);break;
            case Op::AddMulLocImm:case Op::SubMulLocImm:add(q.a);add(q.d);break;
            case Op::AddMulLocLoc:case Op::SubMulLocLoc:add(q.a);add(q.b);add(q.d);break;
            case Op::BrLocLtImm:case Op::BrLocGeImm:case Op::BrLocGtImm:case Op::BrLocLeImm:case Op::BrLocEqImm:case Op::BrLocNeImm:add(q.a);break;
            case Op::BrLocLtLoc:case Op::BrLocGeLoc:case Op::BrLocGtLoc:case Op::BrLocLeLoc:case Op::BrLocEqLoc:case Op::BrLocNeLoc:add(q.a);add(q.b);break;
            default:break;
        }
    };
    for(size_t j=0;j<n;++j){
        if(c[j].op!=Op::Jump||c[j].imm<0||static_cast<size_t>(c[j].imm)>=j)continue;
        const size_t h=static_cast<size_t>(c[j].imm);
        size_t k=h;next(k);if(k>=j||c[k].op==Op::FastLinearLoop||c[k].op==Op::FastPolyLoop||!isBranch(c[k].op))continue;
        const BC br=c[k];
        Op exitOp=br.op;int exitPc=br.aux;
        int boundSlot=(exitOp==Op::BrLocLtLoc||exitOp==Op::BrLocGeLoc||exitOp==Op::BrLocGtLoc||exitOp==Op::BrLocLeLoc)?br.b:-1;
        size_t body=k+1+br.skip;next(body);
        // Tail-recursion layout: the branch goes into the loop body, while the
        // fallthrough executes a small base-case return block and jumps beyond
        // the back edge. Invert the predicate and summarize the branch target.
        if(br.aux>static_cast<int>(k)&&br.aux<static_cast<int>(j)){
            size_t fall=k+1+br.skip, q=fall;bool exitPath=false,safeExit=true;
            while(q<static_cast<size_t>(br.aux)){
                next(q);if(q>=static_cast<size_t>(br.aux))break;const BC& z=c[q];
                if(z.op==Op::Jump){if(z.imm>static_cast<int>(j)){exitPath=true;q+=1+z.skip;break;}safeExit=false;break;}
                if(!(z.op==Op::Li||z.op==Op::Load||z.op==Op::Mv||z.op==Op::Nop)){safeExit=false;break;}
                q+=1+z.skip;
            }
            if(!safeExit||!exitPath)continue;
            exitOp=invert(br.op);exitPc=static_cast<int>(fall);body=static_cast<size_t>(br.aux);next(body);
        } else if(br.aux<=static_cast<int>(j)||br.aux<0) continue;
        if(body>=j)continue;
        bool safe=true;std::unordered_set<int> mods;
        for(size_t q=body;q<j;){
            next(q);if(q>=j)break;const BC& x=c[q];
            switch(x.op){
                case Op::Li: case Op::Load: case Op::Store: case Op::Mv:
                case Op::Add: case Op::Sub: case Op::Mul:
                case Op::StoreImm: case Op::CopyLocal:
                case Op::AddLocImm: case Op::SubLocImm: case Op::MulLocImm:
                case Op::AddLocLoc: case Op::SubLocLoc: case Op::MulLocLoc:
                case Op::AddMulLocImm: case Op::SubMulLocImm:
                case Op::AddMulLocLoc: case Op::SubMulLocLoc:
                    break;
                default:safe=false;break;
            }
            if(!safe)break;
            switch(x.op){
                case Op::Store:mods.insert(x.imm);break;
                case Op::StoreImm:case Op::CopyLocal:
                case Op::AddLocImm:case Op::SubLocImm:case Op::MulLocImm:
                case Op::AddLocLoc:case Op::SubLocLoc:case Op::MulLocLoc:
                case Op::AddMulLocImm:case Op::SubMulLocImm:
                case Op::AddMulLocLoc:case Op::SubMulLocLoc:mods.insert(x.d);break;
                default:break;
            }
            q+=1+x.skip;
        }
        if(!safe||!mods.count(br.a))continue;
        if(boundSlot>=0 && mods.count(boundSlot))continue;
        std::unordered_set<int> live;
        // Follow bytecode CFG from the loop exit rather than scanning textually:
        // tail-recursion layouts place the loop body after a base-case jump, so
        // a linear scan would incorrectly make dead argument temporaries live.
        std::vector<size_t> work{static_cast<size_t>(exitPc)};std::vector<char> seen(n,0);
        while(!work.empty()){
            size_t q=work.back();work.pop_back();if(q>=n||seen[q])continue;seen[q]=1;const BC&z=c[q];addReads(z,live);
            auto push=[&](size_t x){if(x<n&&!seen[x])work.push_back(x);};
            if(z.op==Op::Jump)push(static_cast<size_t>(z.imm));
            else if(z.op==Op::Bz||z.op==Op::Bnz){push(static_cast<size_t>(z.imm));push(q+1+z.skip);}
            else if(z.op==Op::BrLocLtImm||z.op==Op::BrLocGeImm||z.op==Op::BrLocGtImm||z.op==Op::BrLocLeImm||z.op==Op::BrLocEqImm||z.op==Op::BrLocNeImm||z.op==Op::BrLocLtLoc||z.op==Op::BrLocGeLoc||z.op==Op::BrLocGtLoc||z.op==Op::BrLocLeLoc||z.op==Op::BrLocEqLoc||z.op==Op::BrLocNeLoc){push(static_cast<size_t>(z.aux));push(q+1+z.skip);}
            else if(z.op!=Op::Ret)push(q+1+z.skip);
        }
        PolyLoop pl;pl.exitOp=exitOp;pl.iv=br.a;pl.bound=boundSlot>=0?0:br.imm;pl.boundSlot=static_cast<int16_t>(boundSlot);pl.exitPc=exitPc;
        pl.bodyBegin=static_cast<int>(body);pl.bodyEnd=static_cast<int>(j);
        for(int m:mods)if(m>=0&&m<=std::numeric_limits<int16_t>::max())pl.modified.push_back(static_cast<int16_t>(m));else{safe=false;break;}
        for(int m:live)if(m>=0&&m<=std::numeric_limits<int16_t>::max())pl.liveAfter.push_back(static_cast<int16_t>(m));
        if(!safe)continue;
        int idx=static_cast<int>(fn.polyLoops.size());fn.polyLoops.push_back(std::move(pl));BC y;y.op=Op::FastPolyLoop;y.imm=idx;c[k]=y;
    }
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
        dst.paramCount = src.paramCount;
        dst.pure = true;
        for (const auto& ins : src.instrs) {
            if (ins.type == IRInstrType::LOAD_GLOBAL || ins.type == IRInstrType::STORE_GLOBAL) {
                dst.pure = false;
                break;
            }
        }

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
        fuseSuperInstructions(dst);
        fuseLinearLoops(dst);
        fusePolynomialLoops(dst);
    }
    // A function is memoizable only if it has no global access and every
    // function it calls is likewise pure. Starting from the local property
    // and iterating to a fixed point also handles recursive SCCs safely.
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t fi = 0; fi < out.functions.size(); ++fi) {
            if (!out.functions[fi].pure) continue;
            for (const auto& bc : out.functions[fi].code) {
                if (bc.op == Op::Call && (bc.imm < 0 || bc.imm >= static_cast<int>(out.functions.size()) || !out.functions[bc.imm].pure)) {
                    out.functions[fi].pure = false;
                    changed = true;
                    break;
                }
            }
        }
    }
    return true;
}

struct MemoKey {
    int fn = -1;
    std::uint8_t count = 0;
    std::array<std::int32_t, 8> args{};
    bool operator==(const MemoKey& o) const noexcept {
        if (fn != o.fn || count != o.count) return false;
        for (std::uint8_t i = 0; i < count; ++i) if (args[i] != o.args[i]) return false;
        return true;
    }
};
struct MemoHash {
    std::size_t operator()(const MemoKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(k.fn) * 0x9e3779b97f4a7c15ULL + k.count;
        for (std::uint8_t i = 0; i < k.count; ++i) {
            std::size_t x = static_cast<std::uint32_t>(k.args[i]);
            h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        }
        return h;
    }
};


struct SymExpr {
    bool ok=true;
    std::array<std::uint32_t,3> p{}; // c0 + c1*X + c2*X^2 (mod 2^32)
    std::unordered_map<int,std::uint32_t> state;
};
static SymExpr symConst(std::int32_t v){SymExpr e;e.p[0]=static_cast<uint32_t>(v);return e;}
static SymExpr symX(){SymExpr e;e.p[1]=1;return e;}
static SymExpr symState(int s){SymExpr e;e.state[s]=1;return e;}
static SymExpr symAdd(const SymExpr&a,const SymExpr&b,bool sub=false){
    if(!a.ok||!b.ok){SymExpr z;z.ok=false;return z;}SymExpr r;
    for(int i=0;i<3;++i)r.p[i]=sub?a.p[i]-b.p[i]:a.p[i]+b.p[i];r.state=a.state;
    for(auto [k,v]:b.state){uint32_t&x=r.state[k];x=sub?x-v:x+v;if(x==0)r.state.erase(k);}return r;
}
static bool symIsConstant(const SymExpr& e) {
    return e.ok && e.state.empty() && e.p[1] == 0 && e.p[2] == 0;
}
static SymExpr symScale(const SymExpr& e, uint32_t k) {
    if (!e.ok) { SymExpr z; z.ok=false; return z; }
    SymExpr r=e;
    for (auto& x : r.p) x=static_cast<uint32_t>(static_cast<uint64_t>(x)*k);
    for (auto it=r.state.begin(); it!=r.state.end();) {
        it->second=static_cast<uint32_t>(static_cast<uint64_t>(it->second)*k);
        if (it->second==0) it=r.state.erase(it); else ++it;
    }
    return r;
}
static SymExpr symMul(const SymExpr&a,const SymExpr&b){
    if(!a.ok||!b.ok){SymExpr z;z.ok=false;return z;}
    // Multiplication by a compile-time constant preserves an affine/polynomial
    // state expression.  This is common in copy/recurrence loops such as
    // `a = 2*a + b`; the previous evaluator unnecessarily rejected it.
    if (symIsConstant(a)) return symScale(b,a.p[0]);
    if (symIsConstant(b)) return symScale(a,b.p[0]);
    if(!a.state.empty()||!b.state.empty()){SymExpr z;z.ok=false;return z;}SymExpr r;
    std::array<uint32_t,5> q{};for(int i=0;i<3;++i)for(int j=0;j<3;++j)q[i+j]+=static_cast<uint32_t>(static_cast<uint64_t>(a.p[i])*b.p[j]);
    if(q[3]||q[4]){r.ok=false;return r;}r.p[0]=q[0];r.p[1]=q[1];r.p[2]=q[2];return r;
}
static uint32_t evalPoly(const std::array<uint32_t,3>&p,int64_t x){uint32_t ux=static_cast<uint32_t>(x);uint32_t x2=static_cast<uint32_t>(static_cast<uint64_t>(ux)*ux);return p[0]+static_cast<uint32_t>(static_cast<uint64_t>(p[1])*ux)+static_cast<uint32_t>(static_cast<uint64_t>(p[2])*x2);}

// All closed-form loop summaries ultimately update ToyC int values, whose
// arithmetic is modeled modulo 2^32 throughout this evaluator.  Keeping only
// the low 32 bits lets us stay in standard C++20 and remain portable to MSVC;
// no compiler-specific 128-bit integer extension is required.
static uint32_t mul32(uint32_t a,uint32_t b){
    return static_cast<uint32_t>(static_cast<uint64_t>(a)*static_cast<uint64_t>(b));
}
static uint32_t triangular32(uint64_t n){
    // n*(n-1)/2, cancel the exact factor 2 before multiplying.
    uint64_t a=n,b=n-1;
    if((a&1u)==0)a/=2;else b/=2;
    return mul32(static_cast<uint32_t>(a),static_cast<uint32_t>(b));
}
static uint32_t squareSum32(uint64_t n){
    // n*(n-1)*(2*n-1)/6 = sum_{k=0}^{n-1} k^2.
    // For ToyC induction variables n is at most the 32-bit iteration range,
    // so 2*n-1 is safely representable by uint64_t.  Divide exact factors
    // 2 and 3 first, then multiply only the low 32 bits.
    if(n==0)return 0;
    uint64_t a=n,b=n-1,c=2*n-1;
    if((a&1u)==0)a/=2;else b/=2;
    if(a%3==0)a/=3;else if(b%3==0)b/=3;else c/=3;
    return mul32(mul32(static_cast<uint32_t>(a),static_cast<uint32_t>(b)),
                 static_cast<uint32_t>(c));
}
static uint32_t progressionSum32(int64_t x0,int64_t step,uint64_t trips){
    return mul32(static_cast<uint32_t>(trips),static_cast<uint32_t>(x0))
         + mul32(static_cast<uint32_t>(step),triangular32(trips));
}
static uint32_t progressionSquareSum32(int64_t x0,int64_t step,uint64_t trips){
    const uint32_t ux0=static_cast<uint32_t>(x0);
    const uint32_t us=static_cast<uint32_t>(step);
    const uint32_t tri=triangular32(trips);
    const uint32_t sq=squareSum32(trips);
    uint32_t r=mul32(static_cast<uint32_t>(trips),mul32(ux0,ux0));
    r+=mul32(mul32(2u,mul32(ux0,us)),tri);
    r+=mul32(mul32(us,us),sq);
    return r;
}
static uint32_t sumPoly(const std::array<uint32_t,3>&p,int64_t x0,int64_t step,uint64_t trips){
    const uint32_t sx=progressionSum32(x0,step,trips);
    const uint32_t sx2=progressionSquareSum32(x0,step,trips);
    return mul32(p[0],static_cast<uint32_t>(trips))
         + mul32(p[1],sx)
         + mul32(p[2],sx2);
}


// Apply an affine state transition modulo 2^32 by binary exponentiation.
// Each row describes next_state = M * [state..., 1].  This lets the host
// evaluator collapse copy chains and mutually-recursive scalar recurrences
// (e.g. Fibonacci-like loops) in O(k^3 log N) instead of interpreting N trips.
static bool applyAffineTransition32(
    const std::vector<int16_t>& slots,
    const std::vector<SymExpr>& next,
    int ivSlot,
    uint64_t trips,
    std::vector<std::int32_t>& locals) {
    const size_t m=slots.size();
    if(m==0 || m>32 || next.size()!=m) return false;
    std::unordered_map<int,size_t> index;
    index.reserve(m*2);
    for(size_t i=0;i<m;++i) index[slots[i]]=i;
    auto ivIt=index.find(ivSlot); if(ivIt==index.end()) return false;
    const size_t D=m+1;
    std::vector<uint32_t> mat(D*D,0), vec(D,0);
    auto at=[&](std::vector<uint32_t>& a,size_t r,size_t c)->uint32_t&{return a[r*D+c];};
    for(size_t r=0;r<m;++r){
        const SymExpr& e=next[r];
        if(!e.ok || e.p[2]!=0) return false;
        at(mat,r,D-1)+=e.p[0];
        at(mat,r,ivIt->second)+=e.p[1];
        for(const auto& [slot,coeff]:e.state){
            auto it=index.find(slot); if(it==index.end()) return false;
            at(mat,r,it->second)+=coeff;
        }
    }
    at(mat,D-1,D-1)=1;
    for(size_t i=0;i<m;++i){
        int slot=slots[i]; if(slot<0||slot>=static_cast<int>(locals.size())) return false;
        vec[i]=static_cast<uint32_t>(locals[slot]);
    }
    vec[D-1]=1;
    auto mulMat=[&](const std::vector<uint32_t>& A,const std::vector<uint32_t>& B){
        std::vector<uint32_t> C(D*D,0);
        for(size_t i=0;i<D;++i) for(size_t k=0;k<D;++k){
            uint32_t aik=A[i*D+k]; if(aik==0) continue;
            for(size_t j=0;j<D;++j){uint32_t b=B[k*D+j];if(b)C[i*D+j]+=static_cast<uint32_t>(static_cast<uint64_t>(aik)*b);}
        }
        return C;
    };
    auto mulVec=[&](const std::vector<uint32_t>& A,const std::vector<uint32_t>& x){
        std::vector<uint32_t> y(D,0);
        for(size_t i=0;i<D;++i){uint32_t acc=0;for(size_t j=0;j<D;++j)if(A[i*D+j]&&x[j])acc+=static_cast<uint32_t>(static_cast<uint64_t>(A[i*D+j])*x[j]);y[i]=acc;}
        return y;
    };
    std::vector<uint32_t> base=std::move(mat);
    uint64_t e=trips;
    while(e){if(e&1)vec=mulVec(base,vec);e>>=1;if(e)base=mulMat(base,base);}
    for(size_t i=0;i<m;++i) locals[slots[i]]=static_cast<int32_t>(vec[i]);
    return true;
}

struct Frame {
    int fn = -1;
    std::size_t pc = 0;
    std::array<std::int32_t, REG_COUNT> r{};
    std::vector<std::int32_t> locals;
    std::vector<std::int32_t> incoming;
    std::vector<std::int32_t> outgoing;
    bool memoValid = false;
    MemoKey memoKey{};
};

} // namespace

std::optional<std::int32_t> tryEvaluateIRAtCompileTime(
    const IRProgram& program, std::uint64_t instructionBudget, std::uint64_t maxWallMillis) {
    Compiled cp;
    if (!compileProgram(program, cp)) return std::nullopt;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(maxWallMillis);
    std::uint64_t steps = 0;
    std::unordered_map<MemoKey, std::int32_t, MemoHash> memo;
    memo.reserve(4096);
    constexpr std::size_t MAX_MEMO_ENTRIES = 1u << 20;
    std::vector<Frame> stack;
    stack.reserve(64);
    auto pushFrame = [&](int fi, const std::array<std::int32_t, REG_COUNT>& regs,
                         const std::vector<std::int32_t>& incoming, const MemoKey* memoKey = nullptr) {
        Frame f;
        f.fn=fi; f.r=regs; f.incoming=incoming;
        f.locals.assign(cp.functions[fi].localWords, 0);
        f.outgoing.assign(cp.functions[fi].outgoingWords, 0);
        if (memoKey) { f.memoValid = true; f.memoKey = *memoKey; }
        stack.push_back(std::move(f));
    };
    std::array<std::int32_t, REG_COUNT> zeroRegs{};
    pushFrame(cp.mainIndex, zeroRegs, {});

    while (!stack.empty()) {
        Frame& f=stack.back();
        const Fn& fn=cp.functions[f.fn];
        if (f.pc >= fn.code.size()) {
            std::int32_t ret=f.r[R_A0];
            const bool cache = f.memoValid;
            const MemoKey key = f.memoKey;
            stack.pop_back();
            if (cache && memo.size() < MAX_MEMO_ENTRIES) memo.emplace(key, ret);
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
            case Op::StoreImm:
                if(x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                f.locals[x.d]=x.imm; f.pc += x.skip; break;
            case Op::CopyLocal:
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                f.locals[x.d]=f.locals[x.a]; f.pc += x.skip; break;
            case Op::AddLocImm: case Op::SubLocImm: case Op::MulLocImm:
            case Op::DivLocImm: case Op::RemLocImm: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                int32_t a=f.locals[x.a], r=0;
                if(x.op==Op::AddLocImm) r=static_cast<int32_t>(static_cast<uint32_t>(a)+static_cast<uint32_t>(x.imm));
                else if(x.op==Op::SubLocImm) r=static_cast<int32_t>(static_cast<uint32_t>(a)-static_cast<uint32_t>(x.imm));
                else if(x.op==Op::MulLocImm) r=static_cast<int32_t>(static_cast<uint32_t>(a)*static_cast<uint32_t>(x.imm));
                else if(x.op==Op::DivLocImm) { if(!checkedDiv(a,x.imm,r)) return std::nullopt; }
                else { if(x.imm==0)return std::nullopt; if(a==std::numeric_limits<int32_t>::min()&&x.imm==-1) r=0; else r=a%x.imm; }
                f.locals[x.d]=r; f.pc += x.skip; break;
            }
            case Op::AddLocLoc: case Op::SubLocLoc: case Op::MulLocLoc:
            case Op::DivLocLoc: case Op::RemLocLoc: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.b<0||x.b>=static_cast<int>(f.locals.size())||x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                int32_t a=f.locals[x.a], b=f.locals[x.b], r=0;
                if(x.op==Op::AddLocLoc) r=static_cast<int32_t>(static_cast<uint32_t>(a)+static_cast<uint32_t>(b));
                else if(x.op==Op::SubLocLoc) r=static_cast<int32_t>(static_cast<uint32_t>(a)-static_cast<uint32_t>(b));
                else if(x.op==Op::MulLocLoc) r=static_cast<int32_t>(static_cast<uint32_t>(a)*static_cast<uint32_t>(b));
                else if(x.op==Op::DivLocLoc) { if(!checkedDiv(a,b,r)) return std::nullopt; }
                else { if(b==0)return std::nullopt; if(a==std::numeric_limits<int32_t>::min()&&b==-1) r=0; else r=a%b; }
                f.locals[x.d]=r; f.pc += x.skip; break;
            }
            case Op::AddMulLocImm: case Op::SubMulLocImm: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                uint32_t prod=static_cast<uint32_t>(static_cast<uint64_t>(static_cast<uint32_t>(f.locals[x.a]))*static_cast<uint32_t>(x.imm));
                uint32_t acc=static_cast<uint32_t>(f.locals[x.d]);
                f.locals[x.d]=static_cast<int32_t>(x.op==Op::AddMulLocImm?acc+prod:acc-prod); f.pc+=x.skip; break;
            }
            case Op::AddMulLocLoc: case Op::SubMulLocLoc: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.b<0||x.b>=static_cast<int>(f.locals.size())||x.d<0||x.d>=static_cast<int>(f.locals.size())) return std::nullopt;
                uint32_t prod=static_cast<uint32_t>(static_cast<uint64_t>(static_cast<uint32_t>(f.locals[x.a]))*static_cast<uint32_t>(f.locals[x.b]));
                uint32_t acc=static_cast<uint32_t>(f.locals[x.d]);
                f.locals[x.d]=static_cast<int32_t>(x.op==Op::AddMulLocLoc?acc+prod:acc-prod); f.pc+=x.skip; break;
            }
            case Op::BrLocLtImm: case Op::BrLocGeImm: case Op::BrLocGtImm:
            case Op::BrLocLeImm: case Op::BrLocEqImm: case Op::BrLocNeImm: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())) return std::nullopt;
                int32_t a=f.locals[x.a]; bool take=false;
                if(x.op==Op::BrLocLtImm) take=a<x.imm;
                else if(x.op==Op::BrLocGeImm) take=a>=x.imm;
                else if(x.op==Op::BrLocGtImm) take=a>x.imm;
                else if(x.op==Op::BrLocLeImm) take=a<=x.imm;
                else if(x.op==Op::BrLocEqImm) take=a==x.imm;
                else take=a!=x.imm;
                if(take) f.pc=static_cast<size_t>(x.aux); else f.pc += x.skip;
                break;
            }
            case Op::BrLocLtLoc: case Op::BrLocGeLoc: case Op::BrLocGtLoc:
            case Op::BrLocLeLoc: case Op::BrLocEqLoc: case Op::BrLocNeLoc: {
                if(x.a<0||x.a>=static_cast<int>(f.locals.size())||x.b<0||x.b>=static_cast<int>(f.locals.size())) return std::nullopt;
                int32_t a=f.locals[x.a], b=f.locals[x.b]; bool take=false;
                if(x.op==Op::BrLocLtLoc) take=a<b;
                else if(x.op==Op::BrLocGeLoc) take=a>=b;
                else if(x.op==Op::BrLocGtLoc) take=a>b;
                else if(x.op==Op::BrLocLeLoc) take=a<=b;
                else if(x.op==Op::BrLocEqLoc) take=a==b;
                else take=a!=b;
                if(take) f.pc=static_cast<size_t>(x.aux); else f.pc += x.skip;
                break;
            }
            case Op::FastLinearLoop: {
                if(x.imm<0||x.imm>=static_cast<int>(fn.fastLoops.size())) return std::nullopt;
                const auto& L=fn.fastLoops[x.imm];
                if(L.iv<0||L.iv>=static_cast<int>(f.locals.size())) return std::nullopt;
                if(L.boundSlot>=0 && L.boundSlot>=static_cast<int>(f.locals.size())) return std::nullopt;
                int64_t cur=f.locals[L.iv], bound=L.boundSlot>=0?f.locals[L.boundSlot]:L.bound, step=L.step; uint64_t trips=0;
                if(L.exitOp==Op::BrLocGeImm||L.exitOp==Op::BrLocGeLoc){ if(cur<bound) trips=static_cast<uint64_t>((bound-cur+step-1)/step); }
                else if(L.exitOp==Op::BrLocGtImm||L.exitOp==Op::BrLocGtLoc){ if(cur<=bound) trips=static_cast<uint64_t>((bound-cur)/step+1); }
                else if(L.exitOp==Op::BrLocLeImm||L.exitOp==Op::BrLocLeLoc){ int64_t d=-step; if(cur>bound) trips=static_cast<uint64_t>((cur-bound+d-1)/d); }
                else if(L.exitOp==Op::BrLocLtImm||L.exitOp==Op::BrLocLtLoc){ int64_t d=-step; if(cur>=bound) trips=static_cast<uint64_t>((cur-bound)/d+1); }
                const int64_t iv0=cur;
                const uint32_t sumIv=progressionSum32(iv0,step,trips);
                auto pow32=[](uint32_t base,uint64_t e){uint32_t r=1;while(e){if(e&1)r=static_cast<uint32_t>(static_cast<uint64_t>(r)*base);base=static_cast<uint32_t>(static_cast<uint64_t>(base)*base);e>>=1;}return r;};
                for(const auto& u:L.updates){
                    if(u.slot<0||u.slot>=static_cast<int>(f.locals.size())) return std::nullopt;
                    uint32_t v=static_cast<uint32_t>(f.locals[u.slot]);
                    if(u.mulFactor!=1) v=static_cast<uint32_t>(static_cast<uint64_t>(v)*pow32(static_cast<uint32_t>(u.mulFactor),trips));
                    uint32_t coeff=static_cast<uint32_t>(u.ivCoeff);
                    if(u.ivScaleSlot>=0){
                        if(u.ivScaleSlot>=static_cast<int>(f.locals.size())) return std::nullopt;
                        coeff=mul32(coeff,static_cast<uint32_t>(f.locals[u.ivScaleSlot]));
                    }
                    uint32_t add=mul32(static_cast<uint32_t>(u.delta),static_cast<uint32_t>(trips))
                               + mul32(coeff,sumIv);
                    f.locals[u.slot]=static_cast<int32_t>(v+add);
                }
                f.pc=static_cast<size_t>(L.exitPc); break;
            }
            case Op::FastPolyLoop: {
                if(x.imm<0||x.imm>=static_cast<int>(fn.polyLoops.size())) return std::nullopt;
                const auto& L=fn.polyLoops[x.imm];
                if(L.iv<0||L.iv>=static_cast<int>(f.locals.size())) return std::nullopt;
                if(L.boundSlot>=0 && L.boundSlot>=static_cast<int>(f.locals.size())) return std::nullopt;
                const int32_t loopBound=L.boundSlot>=0?f.locals[L.boundSlot]:L.bound;
                auto branchTake=[&](int32_t v){
                    if(L.exitOp==Op::BrLocLtImm||L.exitOp==Op::BrLocLtLoc)return v<loopBound;
                    if(L.exitOp==Op::BrLocGeImm||L.exitOp==Op::BrLocGeLoc)return v>=loopBound;
                    if(L.exitOp==Op::BrLocGtImm||L.exitOp==Op::BrLocGtLoc)return v>loopBound;
                    return v<=loopBound;
                };
                if(branchTake(f.locals[L.iv])){f.pc=static_cast<size_t>(L.exitPc);break;}
                std::unordered_set<int> modified(L.modified.begin(),L.modified.end());
                std::vector<SymExpr> loc(f.locals.size());
                for(size_t z=0;z<loc.size();++z){if(static_cast<int>(z)==L.iv)loc[z]=symX();else if(modified.count(static_cast<int>(z)))loc[z]=symState(static_cast<int>(z));else loc[z]=symConst(f.locals[z]);}
                std::array<SymExpr,REG_COUNT> rr;for(auto& e:rr)e.ok=false;
                bool ok=true;
                for(size_t pc=static_cast<size_t>(L.bodyBegin);pc<static_cast<size_t>(L.bodyEnd)&&ok;){
                    const BC q=fn.code[pc];
                    auto validSlot=[&](int z){return z>=0&&z<static_cast<int>(loc.size());};
                    switch(q.op){
                        case Op::Nop: break;
                        case Op::Li: rr[q.d]=symConst(q.imm); break;
                        case Op::Load: if(!validSlot(q.imm)){ok=false;break;}rr[q.d]=loc[q.imm]; break;
                        case Op::Store: if(!validSlot(q.imm)||q.a<0||q.a>=REG_COUNT||!rr[q.a].ok){ok=false;break;}loc[q.imm]=rr[q.a]; break;
                        case Op::Mv: if(q.a<0||q.a>=REG_COUNT||!rr[q.a].ok){ok=false;break;}rr[q.d]=rr[q.a]; break;
                        case Op::Add: case Op::Sub: case Op::Mul:
                            if(q.a<0||q.b<0||q.a>=REG_COUNT||q.b>=REG_COUNT||!rr[q.a].ok||!rr[q.b].ok){ok=false;break;}
                            rr[q.d]=q.op==Op::Add?symAdd(rr[q.a],rr[q.b]):q.op==Op::Sub?symAdd(rr[q.a],rr[q.b],true):symMul(rr[q.a],rr[q.b]);if(!rr[q.d].ok)ok=false;break;
                        case Op::StoreImm: if(!validSlot(q.d)){ok=false;break;}loc[q.d]=symConst(q.imm); break;
                        case Op::CopyLocal: if(!validSlot(q.a)||!validSlot(q.d)){ok=false;break;}loc[q.d]=loc[q.a]; break;
                        case Op::AddLocImm: case Op::SubLocImm: case Op::MulLocImm:
                            if(!validSlot(q.a)||!validSlot(q.d)){ok=false;break;}
                            {auto imm=symConst(q.imm);loc[q.d]=q.op==Op::AddLocImm?symAdd(loc[q.a],imm):q.op==Op::SubLocImm?symAdd(loc[q.a],imm,true):symMul(loc[q.a],imm);if(!loc[q.d].ok)ok=false;}break;
                        case Op::AddLocLoc: case Op::SubLocLoc: case Op::MulLocLoc:
                            if(!validSlot(q.a)||!validSlot(q.b)||!validSlot(q.d)){ok=false;break;}
                            loc[q.d]=q.op==Op::AddLocLoc?symAdd(loc[q.a],loc[q.b]):q.op==Op::SubLocLoc?symAdd(loc[q.a],loc[q.b],true):symMul(loc[q.a],loc[q.b]);if(!loc[q.d].ok)ok=false;break;
                        case Op::AddMulLocImm: case Op::SubMulLocImm:
                            if(!validSlot(q.a)||!validSlot(q.d)){ok=false;break;}
                            {auto prod=symMul(loc[q.a],symConst(q.imm));loc[q.d]=q.op==Op::AddMulLocImm?symAdd(loc[q.d],prod):symAdd(loc[q.d],prod,true);if(!loc[q.d].ok)ok=false;}break;
                        case Op::AddMulLocLoc: case Op::SubMulLocLoc:
                            if(!validSlot(q.a)||!validSlot(q.b)||!validSlot(q.d)){ok=false;break;}
                            {auto prod=symMul(loc[q.a],loc[q.b]);loc[q.d]=q.op==Op::AddMulLocLoc?symAdd(loc[q.d],prod):symAdd(loc[q.d],prod,true);if(!loc[q.d].ok)ok=false;}break;
                        default: ok=false; break;
                    }
                    pc+=1+q.skip;
                }
                int64_t step=0;
                if(ok){const SymExpr& ie=loc[L.iv];if(!ie.state.empty()||ie.p[2]!=0||ie.p[1]!=1)ok=false;else step=static_cast<int32_t>(ie.p[0]);if(step==0)ok=false;}
                bool dir=ok&&(((L.exitOp==Op::BrLocGeImm||L.exitOp==Op::BrLocGtImm||L.exitOp==Op::BrLocGeLoc||L.exitOp==Op::BrLocGtLoc)&&step>0)||((L.exitOp==Op::BrLocLeImm||L.exitOp==Op::BrLocLtImm||L.exitOp==Op::BrLocLeLoc||L.exitOp==Op::BrLocLtLoc)&&step<0));
                if(!dir){
                    // Fall back to the original branch semantics; body bytecode is unchanged.
                    f.pc=static_cast<size_t>(L.bodyBegin);break;
                }
                int64_t cur=f.locals[L.iv],bound=loopBound;uint64_t trips=0;
                if(L.exitOp==Op::BrLocGeImm||L.exitOp==Op::BrLocGeLoc){if(cur<bound)trips=(bound-cur+step-1)/step;}
                else if(L.exitOp==Op::BrLocGtImm||L.exitOp==Op::BrLocGtLoc){if(cur<=bound)trips=(bound-cur)/step+1;}
                else if(L.exitOp==Op::BrLocLeImm||L.exitOp==Op::BrLocLeLoc){int64_t d=-step;if(cur>bound)trips=(cur-bound+d-1)/d;}
                else {int64_t d=-step;if(cur>=bound)trips=(cur-bound)/d+1;}
                if(trips==0){f.pc=static_cast<size_t>(L.exitPc);break;}
                const int64_t lastX=cur+static_cast<int64_t>(trips-1)*step;
                // First try the general affine transition.  Unlike the older
                // per-slot formulas, it permits values to move between locals
                // from one iteration to the next (copy chains, rotations,
                // Fibonacci-like recurrences, linear state machines).  X is the
                // loop-header induction variable, so p1 maps to the iv column.
                std::vector<SymExpr> affineNext; affineNext.reserve(L.modified.size());
                for(int slot:L.modified) affineNext.push_back(loc[slot]);
                if(applyAffineTransition32(L.modified,affineNext,L.iv,trips,f.locals)){
                    f.pc=static_cast<size_t>(L.exitPc);break;
                }

                std::unordered_set<int> liveAfter(L.liveAfter.begin(),L.liveAfter.end());
                for(int slot:L.modified){if(slot==L.iv||!liveAfter.count(slot))continue;const SymExpr&e=loc[slot];
                    if(!e.ok){ok=false;break;}
                    if(e.state.empty()) f.locals[slot]=static_cast<int32_t>(evalPoly(e.p,lastX));
                    else if(e.state.size()==1&&e.state.count(slot)&&e.state.at(slot)==1) f.locals[slot]=static_cast<int32_t>(static_cast<uint32_t>(f.locals[slot])+sumPoly(e.p,cur,step,trips));
                    else {ok=false;break;}
                }
                if(!ok){f.pc=static_cast<size_t>(L.bodyBegin);break;}
                f.locals[L.iv]=static_cast<int32_t>(static_cast<uint32_t>(cur)+static_cast<uint32_t>(static_cast<uint64_t>(static_cast<uint32_t>(step))*trips));
                f.pc=static_cast<size_t>(L.exitPc);break;
            }
            case Op::Jump: f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Bz: if(f.r[x.a]==0) f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Bnz: if(f.r[x.a]!=0) f.pc=static_cast<std::size_t>(x.imm); break;
            case Op::Call: {
                if (x.imm < 0 || x.imm >= static_cast<int>(cp.functions.size())) return std::nullopt;
                const Fn& callee = cp.functions[x.imm];
                MemoKey key; bool useMemo = callee.pure && callee.paramCount >= 0 && callee.paramCount <= 8;
                if (useMemo) {
                    key.fn = x.imm; key.count = static_cast<std::uint8_t>(callee.paramCount);
                    for (int k = 0; k < callee.paramCount; ++k) key.args[k] = f.r[R_A0 + k];
                    auto hit = memo.find(key);
                    if (hit != memo.end()) { f.r[R_A0] = hit->second; break; }
                }
                std::array<std::int32_t, REG_COUNT> regs{};
                for(int k=0;k<8;++k) regs[R_A0+k]=f.r[R_A0+k];
                std::vector<std::int32_t> extra=f.outgoing;
                pushFrame(x.imm,regs,extra,useMemo?&key:nullptr);
                break;
            }
            case Op::Ret: {
                std::int32_t ret=f.r[R_A0];
                const bool cache=f.memoValid; const MemoKey key=f.memoKey;
                stack.pop_back();
                if(cache && memo.size()<MAX_MEMO_ENTRIES) memo.emplace(key,ret);
                if(stack.empty()) return ret; stack.back().r[R_A0]=ret; break;
            }
        }
    }
    return std::nullopt;
}

} // namespace toycc
