// libFuzzer harness for the production GTI formatter
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// Properties enforced per iteration:
//   - formatting arbitrary bytes never crashes, reports under a sanitizer,
//     or hangs; and
//   - formatting is idempotent: format(format(x)) == format(x) byte for
//     byte; and
//   - when the production lexer accepts the original input, formatting
//     preserves the significant token kind and normalized spelling sequence.
//     Comments are lexer trivia and therefore intentionally absent from this
//     sequence; formatter idempotence still covers their concrete output.
//     Lexically recovered input keeps the crash/idempotence/range properties,
//     but does not claim a token-preservation oracle.
//
// The harness deliberately does not grow a second parser to compare syntax
// trees; semantic preservation of formatting is checked by the bounded audit
// over typed generated programs (Q-FUZZ-01D), not in-process here.

#include "fuzz_support.h"

#include "gti/formatter.h"
#include "gti/lexer.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct SignificantToken {
  lang::TokenKind kind = lang::TokenKind::END_OF_FILE;
  std::string text;

  friend bool operator==(const SignificantToken &,
                         const SignificantToken &) = default;
};

struct LexicalSnapshot {
  std::vector<SignificantToken> tokens;
  bool valid = false;
};

std::string normalizedText(const lang::Token &token) {
  using enum lang::TokenKind;
  switch (token.kind) {
  case INT8:
    return "int8_t";
  case INT16:
    return "int16_t";
  case INT32:
    return "int32_t";
  case INT64:
    return "int64_t";
  case UINT8:
    return "uint8_t";
  case UINT16:
    return "uint16_t";
  case UINT32:
    return "uint32_t";
  case UINT64:
    return "uint64_t";
  default:
    return token.lexeme;
  }
}

void checkSpan(const lang::SourceSpan &span, std::size_t sourceSize) {
  if (span.start > span.end || span.end > sourceSize || span.line < 1) {
    gti_fuzz::fail("formatter", "lexer diagnostic range is invalid");
  }
}

LexicalSnapshot lex(std::string source, std::string sourceName) {
  const std::size_t sourceSize = source.size();
  lang::Lexer lexer;
  const std::vector<lang::Token> tokens =
      lexer.scan(std::move(source), std::move(sourceName));
  for (const lang::Diagnostic &diagnostic : lexer.errors()) {
    checkSpan(diagnostic.primary, sourceSize);
    for (const lang::RelatedDiagnostic &related : diagnostic.related) {
      checkSpan(related.span, sourceSize);
    }
    for (const lang::FixIt &fix : diagnostic.fixes) {
      checkSpan(fix.span, sourceSize);
    }
  }

  LexicalSnapshot result;
  result.valid = !lexer.hadError();
  result.tokens.reserve(tokens.size());
  for (const lang::Token &token : tokens) {
    if (token.kind != lang::TokenKind::END_OF_FILE) {
      result.tokens.push_back(
          {.kind = token.kind, .text = normalizedText(token)});
    }
  }
  return result;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  const std::string_view source(reinterpret_cast<const char *>(data), size);
  const lang::Formatter formatter;
  const std::string once = formatter.format(source);
  std::string twice = formatter.format(once);
  if (gti_fuzz::injectProperty(data, size, "formatter-idempotence")) {
    twice += "injected";
  }
  if (once != twice) {
    gti_fuzz::fail("formatter", "formatting is not idempotent");
  }

  const LexicalSnapshot original = lex(std::string(source), "fuzz.gti");
  LexicalSnapshot formatted = lex(once, "formatted.gti");
  if (gti_fuzz::injectProperty(data, size, "formatter-token")) {
    formatted.tokens.push_back(
        {.kind = lang::TokenKind::IDENTIFIER, .text = "injected"});
  }
  if (original.valid && !formatted.valid) {
    gti_fuzz::fail("formatter", "formatting made lexer-clean input invalid");
  }
  if (original.valid && original.tokens != formatted.tokens) {
    gti_fuzz::fail("formatter", "significant lexer token kind/text changed");
  }
  return 0;
}
