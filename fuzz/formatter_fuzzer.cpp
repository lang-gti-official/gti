// libFuzzer harness for the production GTI formatter
// (Q-FUZZ-01B, docs/plans/compiler-bug-finding-and-triage.md).
//
// Properties enforced per iteration:
//   - formatting arbitrary bytes never crashes, reports under a sanitizer,
//     or hangs; and
//   - formatting is idempotent: format(format(x)) == format(x) byte for
//     byte. The formatter has no failure channel — it always produces
//     output — so idempotence is asserted unconditionally.
//
// The harness deliberately does not grow a second parser to compare syntax
// trees; semantic preservation of formatting is checked by the bounded audit
// over typed generated programs (Q-FUZZ-01D), not in-process here.

#include "fuzz_support.h"

#include "gti/formatter.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  if (size > gti_fuzz::kMaxInputBytes) {
    return 0;
  }
  gti_fuzz::checkInjectedFailure(data, size);

  const std::string_view source(reinterpret_cast<const char *>(data), size);
  const lang::Formatter formatter;
  const std::string once = formatter.format(source);
  const std::string twice = formatter.format(once);
  if (once != twice) {
    gti_fuzz::fail("formatter", "formatting is not idempotent");
  }
  return 0;
}
