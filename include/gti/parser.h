#pragma once

#include "gti/ast.h"
#include "gti/diagnostic.h"
#include "gti/lifetime_names.h"
#include "gti/token.h"

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace lang {

using ParseDiagnostic = Diagnostic;

// Editor-only syntax facts. These do not resolve names or change the AST;
// the real parser records a marker only at a declaration/statement boundary.
enum class SyntaxCompletionKind { Declaration, ClassMember, Statement };

struct SyntaxCompletionContext {
  SyntaxCompletionKind kind = SyntaxCompletionKind::Declaration;
  Token token;
  std::optional<Token> owner;
  std::optional<ClassKind> ownerKind;
};

// Rebuild parser-owned range bindings from their written operands. Both fresh
// parsing and imported bodies use the same lowering and binding identities.
StmtPtr makeParsedRangeFor(std::size_t ordinal, Token keyword,
                           Mutability bindingMutability, TypeRef bindingType,
                           Token bindingName, Token colon, ExprPtr range,
                           StmtPtr body, StmtPtr initializer);
StmtPtr makeParsedConstexprFor(Token constexprKeyword, Token keyword,
                               Mutability mutability, TypeRef bindingType,
                               Token name, Token colon, ExprPtr sequence,
                               StmtPtr body);

class Parser {
public:
  // `source` is the scanned unit's text. It is optional and only lets a
  // diagnostic offer an edit that keeps text between tokens, such as comments.
  // The parser borrows it for the duration of parse().
  explicit Parser(std::vector<Token> tokens, std::string_view source = {});
  // Owned, possibly spliced syntax retains trivia independently of source
  // spans.
  explicit Parser(std::shared_ptr<const SyntaxCapture> syntax);
  ~Parser();

  Parser(const Parser &) = delete;
  Parser &operator=(const Parser &) = delete;
  Parser(Parser &&) noexcept;
  Parser &operator=(Parser &&) noexcept;

  void setErrorLimit(std::size_t limit);
  Program parse();
  ExprPtr parseExpression();
  std::unique_ptr<BlockStmt> parseFunctionBody();
  [[nodiscard]] bool hadError() const;
  [[nodiscard]] const std::vector<ParseDiagnostic> &errors() const;
  [[nodiscard]] const std::optional<SyntaxCompletionContext> &
  completionContext() const;
  // Lexical resolution of the region names the parsed unit writes.
  [[nodiscard]] const LifetimeNames &lifetimeNames() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl;
};

} // namespace lang
