// Source-facing spelling of AST names, type references, and semantic types
// for semantic diagnostics. These are pure output helpers: they read the
// AST or the semantic model and produce the exact text the analyzer's
// diagnostics quote, and they carry no analysis state.
#pragma once

#include "gti/ast.h"
#include "gti/semantic_analyzer.h"

#include <string>
#include <string_view>
#include <vector>

namespace lang::semantic_spelling {

// `a::b::name` from an explicit scope vector, using the first
// `segmentCount` segments.
[[nodiscard]] std::string qualifiedName(const std::vector<std::string> &scope,
                                        std::size_t segmentCount,
                                        std::string_view name);

[[nodiscard]] std::string qualifiedName(const std::vector<std::string> &scope,
                                        std::string_view name);

// `a::b::c` from a source name path.
[[nodiscard]] std::string pathSpelling(const NamePath &path);

// The source spelling of a type reference exactly as diagnostics quote it:
// qualifiers, template arguments, pointers, array extents, and references.
[[nodiscard]] std::string typeRefSpelling(const TypeRef &type);

// The user-facing name of a call target expression, or "function" when the
// callee has no quotable name.
[[nodiscard]] std::string callableSpelling(const ExprPtr &callee);

// The declaration keyword of a class kind.
[[nodiscard]] std::string_view classKindSpelling(ClassKind kind);

// The diagnostic spelling of a resolved semantic type.
[[nodiscard]] std::string typeSpelling(const SemanticModel &semantics,
                                       const SemanticType &type);

} // namespace lang::semantic_spelling
