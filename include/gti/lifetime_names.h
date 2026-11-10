#pragma once

#include "gti/token.h"

#include <optional>
#include <vector>

namespace lang {

// Lexical resolution of the lifetime names a source unit writes (RFC 0004).
//
// Lifetimes are erased at the syntax boundary: no semantic type carries a
// region. The parser owns their lexical scopes (function, class, alias,
// concept, conformance and requirement binders, class argument patterns, and
// `for<lifetime ...>` quantified types), so it records which binder each
// written region name denotes. Semantics reports the names no binder covers,
// and editor tooling derives lifetime symbols and occurrences from this table.
struct LifetimeNameUse {
  Token name;
  Token binder;
};

struct LifetimeNameCompletion {
  Token token;
  // Binders visible at the completion token, innermost first, one per
  // spelling.
  std::vector<Token> visible;
};

struct LifetimeNames {
  // Every written binder name, in source order.
  std::vector<Token> binders;
  // Region names resolved to the binder that declares them.
  std::vector<LifetimeNameUse> uses;
  // Region names no enclosing binder declares.
  std::vector<Token> unknown;
  std::optional<LifetimeNameCompletion> completion;

  [[nodiscard]] bool empty() const {
    return binders.empty() && uses.empty() && unknown.empty() && !completion;
  }
};

} // namespace lang
