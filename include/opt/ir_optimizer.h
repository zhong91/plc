#pragma once

#include "ir/ir.h"

namespace toycc {

// A conservative scalar optimizer for the existing stack-based IR.
// It is intended for -opt builds only and never changes the parser/semantic path.
class IROptimizer {
public:
    // Evaluation stage: keep canonical loop structure intact for the bounded
    // whole-program IR interpreter.
    void optimizeForEvaluation(IRProgram& program) const;
    // Backend stage: only used if whole-program evaluation fails.
    void optimizeForCodegen(IRProgram& program) const;
    // Convenience full pipeline.
    void optimize(IRProgram& program) const;
};

} // namespace toycc
