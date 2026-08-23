#pragma once

// Shared bounds and test hooks for the GTI libFuzzer harnesses
// (docs/plans/compiler-bug-finding-and-triage.md, Q-FUZZ-01B).
//
// Harnesses invoke production compiler-library implementations and their
// verifiers; nothing here reimplements a compiler phase or interprets GTI
// meaning.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace gti_fuzz {

// The proposal's conservative raw-input bound. Larger inputs are skipped, not
// truncated, so libFuzzer learns the bound instead of exploring past it.
inline constexpr std::size_t kMaxInputBytes = 64 * 1024;

// The whole-pipeline target runs semantic analysis, the optimizer, the
// lowered-program builder, and both backends per iteration; it needs a much
// smaller bound to keep iteration cost sane.
inline constexpr std::size_t kMaxPipelineInputBytes = 8 * 1024;

// Deterministic injected failure so the smoke test can prove that a harness
// failure produces a replayable libFuzzer artifact without depending on a
// real compiler bug remaining unfixed. Requires both the environment gate and
// the exact magic input; ordinary fuzzing never trips it.
inline constexpr std::string_view kInjectedFailureToken =
    "//gti-fuzz-injected-failure";

inline void checkInjectedFailure(const std::uint8_t *data, std::size_t size) {
  if (std::getenv("GTI_FUZZ_INJECT_FAILURE") == nullptr) {
    return;
  }
  const std::string_view input(reinterpret_cast<const char *>(data), size);
  if (input.substr(0, kInjectedFailureToken.size()) == kInjectedFailureToken) {
    std::fprintf(stderr, "gti-fuzz: injected failure requested by test\n");
    __builtin_trap();
  }
}

[[noreturn]] inline void fail(const char *harness, const char *property) {
  std::fprintf(stderr, "gti-fuzz(%s): property violated: %s\n", harness,
               property);
  __builtin_trap();
}

} // namespace gti_fuzz
