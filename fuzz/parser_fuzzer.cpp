// libFuzzer harness for the production GTI lexer and parser
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// Properties enforced per iteration:
//   - no crash, sanitizer report, assertion, or hang on arbitrary bytes;
//   - every reported diagnostic span stays within its source unit; and
//   - the complete parse result is destroyed before the iteration returns,
//     so recovery must never leave a Program whose destruction touches
//     expired token or source storage.
//
// Invalid UTF-8 and arbitrary bytes are first-class inputs: the lexer must
// fail safely, not cleanly.

#include "fuzz_support.h"

#include "gti/lexer.h"
#include "gti/parser.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace {

void checkSpans(const std::vector<lang::Diagnostic> &diagnostics,
                std::size_t sourceSize, const char *harness) {
  for (const lang::Diagnostic &diagnostic : diagnostics) {
    if (diagnostic.primary.start > diagnostic.primary.end ||
        diagnostic.primary.end > sourceSize) {
      gti_fuzz::fail(harness, "diagnostic span escapes its source unit");
    }
  }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  std::string source(reinterpret_cast<const char *>(data), size);
  lang::Lexer lexer;
  std::vector<lang::Token> tokens = lexer.scan(std::move(source), "fuzz.gti");
  checkSpans(lexer.errors(), size, "parser");

  {
    lang::Parser parser(std::move(tokens));
    lang::Program program = parser.parse();
    checkSpans(parser.errors(), size, "parser");
    // program and parser are destroyed here, while the lexer's source
    // storage is still alive; the reverse order is the lifetime bug class
    // this target exists to catch, and the sanitizers observe it.
  }
  return 0;
}
