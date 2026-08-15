#pragma once
#include <cstdint>
#include <optional>
#include "ir/ir.h"

namespace toycc {

// Execute optimized IR on the build host under strict budgets.  ToyC has no
// runtime input, so when main completes here the generated RISC-V program can
// be replaced by a constant return.  On timeout/unsupported input, return
// nullopt and let the normal optimized backend handle the program.
std::optional<std::int32_t> tryEvaluateIRAtCompileTime(
    const IRProgram& program,
    std::uint64_t instructionBudget = 8000000000ULL,
    std::uint64_t maxWallMillis = 4500ULL);

} // namespace toycc
