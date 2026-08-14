#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/token.h"

#include <memory>
#include <vector>

namespace lang {

using ParseDiagnostic = Diagnostic;

class Parser {
public:
  explicit Parser(std::vector<Token> tokens);
  ~Parser();

  Parser(const Parser &) = delete;
  Parser &operator=(const Parser &) = delete;
  Parser(Parser &&) noexcept;
  Parser &operator=(Parser &&) noexcept;

  Program parse();
  ExprPtr parseExpression();
  [[nodiscard]] bool hadError() const;
  [[nodiscard]] const std::vector<ParseDiagnostic> &errors() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
