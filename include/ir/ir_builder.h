#pragma once
#include <memory>
#include "ast/ast.h"
#include "ir/ir.h"

namespace toycc {

// 前向声明
class SemanticChecker;

class IRBuilder {
public:
    // ★ 新增：接受 SemanticChecker 指针（默认为 nullptr，兼容测试）
    IRProgram build(const ASTNodePtr& root, const SemanticChecker* checker = nullptr);
};

}