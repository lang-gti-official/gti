#pragma once

#include "gti/language_queries.h"

#include <string_view>

namespace lang {

// Static syntax templates selected exclusively from parser-owned boundaries.
// This does not perform semantic lookup or introduce executable language rules.
[[nodiscard]] std::vector<CompletionCandidate>
completionSnippets(const SyntaxCompletionContext &context,
                   std::string_view source);

} // namespace lang
