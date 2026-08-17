#pragma once

#include "ir/ir.h"

namespace toycc {

// A conservative scalar optimizer for the existing stack-based IR.
// It is intended for -opt builds only and never changes the parser/semantic path.
class IROptimizer {
public:
    // Scalar/interprocedural cleanup stage: propagation, CSE, DCE,
    // tail-recursion elimination, function inlining and whole-program DCE.
    void optimizeScalar(IRProgram& program) const;
    // Loop and code-generation-oriented optimization stage.
    void optimizeForCodegen(IRProgram& program) const;
    // Convenience full pipeline.
    void optimize(IRProgram& program) const;
};

} // namespace toycc
