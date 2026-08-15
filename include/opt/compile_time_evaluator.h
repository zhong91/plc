#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include "ast/ast.h"

namespace toycc {

// 在 -opt 模式下尝试对整个 ToyC 程序做编译期求值。
// ToyC 无 I/O、无运行时输入，因此合法且有限执行的程序可以被部分求值为 main 的最终返回值。
// 若超过求值预算或遇到当前求值器不支持的情况，则返回 nullopt，调用方应回退到普通 IR/后端。
std::optional<std::int32_t> tryEvaluateMainAtCompileTime(
    const ASTNodePtr& root,
    std::uint64_t stepBudget = 200000000ULL);

// 输出只返回一个常量的最小 RISC-V 程序。
void emitConstantMain(std::ostream& out, std::int32_t value);

} // namespace toycc
