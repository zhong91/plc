#pragma once

#include "ir/ir.h"

namespace toycc {

// A conservative scalar optimizer for the existing stack-based IR.
// It is intended for -opt builds only and never changes the parser/semantic path.
class IROptimizer {
public:
    void optimize(IRProgram& program) const;
};

} // namespace toycc
