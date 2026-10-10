#pragma once

#include "gti/ast.h"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace lang {

// Deep copies of body-level AST. Semantic analysis records facts by node
// identity, so a construct whose body is analysed more than once with
// different meaning (`constexpr for`) gives every analysis its own copy.
//
// Tokens, spans, and type references are copied as they are, so diagnostics
// on a copy point at the source the user wrote. Declaration kinds that only
// make sense at namespace or class scope (functions, classes, enums,
// concepts, aliases, namespaces, extern blocks) are not cloned: `cloneStmt`
// returns nullptr for them and the caller reports the limitation.
struct AstCloneResult {
  StmtPtr statement;
  // Maps original nodes to their copies for constructs that hold raw
  // cross-references (range-for bindings, constexpr-for element bindings).
  std::unordered_map<const Stmt *, const Stmt *> statements;
  std::unordered_map<const Expr *, const Expr *> expressions;
};

// `expansion`, when nonzero, is stamped on every copied token (Token::expansion)
// so the copy's declarations get identities of their own.
[[nodiscard]] AstCloneResult cloneStmt(const Stmt &statement,
                                       std::uint32_t expansion = 0);
[[nodiscard]] ExprPtr cloneExpr(const Expr &expression,
                                std::uint32_t expansion = 0);

} // namespace lang
