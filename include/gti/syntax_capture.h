#pragma once

#include "gti/diagnostic.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

// Immutable, source-owned lexical input. Offsets remain in the original unit;
// text owns the exact interval, including comments and whitespace. Captures
// never infer a client grammar from their contents.
struct SyntaxCapture {
  std::vector<Token> tokens;
  std::string text;
  std::size_t start = 0;
  std::vector<SourceSpan> origins;
  // Offsets in text, independent of diagnostic/source positions. Builders may
  // splice tokens from multiple source units without rebasing their authority.
  std::vector<std::pair<std::size_t, std::size_t>> textRanges;
};

struct BalancedSyntax {
  std::size_t end = 0; // inclusive on success; offending token on failure
  bool complete = false;
};

[[nodiscard]] BalancedSyntax balancedSyntax(std::span<const Token> tokens,
                                            std::size_t start);
[[nodiscard]] SyntaxCapture captureSyntax(std::span<const Token> tokens,
                                          std::string_view source);
[[nodiscard]] bool syntaxDirective(TokenKind kind);

} // namespace lang
