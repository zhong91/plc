#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "ast/ast.h"

namespace toycc {

class SemanticError : public std::runtime_error {
public:
  explicit SemanticError(const std::string& message)
      : std::runtime_error(message) {}
};

class SemanticChecker {
public:
  SemanticChecker();
  ~SemanticChecker();

  SemanticChecker(const SemanticChecker&) = delete;
  SemanticChecker& operator=(const SemanticChecker&) = delete;

  SemanticChecker(SemanticChecker&&) noexcept;
  SemanticChecker& operator=(SemanticChecker&&) noexcept;

  // Checks the complete ToyC AST. The AST is not modified.
  // Throws SemanticError on the first semantic error.
  void check(const ASTNodePtr& root) const;

  // Query interfaces for later compiler stages.
  bool exists(const std::string& name) const;
  bool isGlobal(const std::string& name) const;
  bool isConstant(const std::string& name) const;
  std::optional<std::int32_t> getConstValue(const std::string& name) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace toycc