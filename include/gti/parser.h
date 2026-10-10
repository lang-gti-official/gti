#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/token.h"

#include <memory>
#include <string_view>
#include <vector>

namespace lang {

using ParseDiagnostic = Diagnostic;

class Parser {
public:
  // `source` is the scanned unit's text. It is optional and only lets a
  // diagnostic offer an edit that keeps text between tokens, such as comments.
  // The parser borrows it for the duration of parse().
  explicit Parser(std::vector<Token> tokens, std::string_view source = {});
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
