#!/usr/bin/env python3
"""Native support scopes retain earliest records and restore across handoff."""

from pathlib import Path
import os
import re
import signal
import subprocess
import sys
import tempfile


PROBE = r"""
#include <gti/runtime_thread.hpp>
#include <thread>
int main() {
  using namespace ::gti_internal::backend;
  ::gti_failure_record_v2 primary{}, secondary{};
  const auto *artifact = &__gti_failure_artifact_descriptor_v1;
  // Worker failure handoff may unwind through this boundary. Outlining must
  // never turn the helper into an exception firewall or a terminal first-failure check.
  static_assert(!noexcept(mir_check_incoming_cleanup_failure_v1(nullptr, nullptr)));
  static_assert(!noexcept(mir_terminate_cleanup_failure_v2(
      nullptr, nullptr, nullptr, nullptr)));
  if (mir_incoming_failure_cleanup_v1() != nullptr) return 1;
  mir_check_incoming_cleanup_failure_v1(nullptr, nullptr);
  {
    mir_failure_cleanup_scope_v1 outer(&primary, artifact);
    {
      mir_failure_cleanup_scope_v1 inner(&secondary, artifact);
      const auto *context = mir_incoming_failure_cleanup_v1();
      if (context == nullptr || context->record != &primary ||
          context->artifact != artifact) return 2;
    }
    if (mir_incoming_failure_cleanup_v1()->record != &primary) return 3;
    bool isolated = false;
    std::thread worker([&] {
      if (mir_incoming_failure_cleanup_v1() != nullptr) return;
      { mir_failure_cleanup_scope_v1 own(&secondary, artifact); }
      isolated = mir_incoming_failure_cleanup_v1() == nullptr;
    });
    worker.join();
    if (!isolated || mir_incoming_failure_cleanup_v1()->record != &primary)
      return 4;
  }
  if (mir_incoming_failure_cleanup_v1() != nullptr) return 5;
  try {
    mir_failure_cleanup_scope_v1 handoff(&primary, artifact);
    throw ::gti_runtime_detail::worker_failure_handoff{};
  } catch (const ::gti_runtime_detail::worker_failure_handoff &) {}
  if (mir_incoming_failure_cleanup_v1() != nullptr) return 6;
  {
    mir_cleanup_sequence_v1 sequence(&primary, false, artifact);
    if (!sequence.step(false)) return 7;
    if (mir_incoming_failure_cleanup_v1()->record != &primary) return 8;
    mir_failure_cleanup_scope_v1 nested(sequence.record(), artifact);
    if (mir_incoming_failure_cleanup_v1()->record != &primary) return 9;
    if (!sequence.step(true) || sequence.success()) return 10;
  }
  return mir_incoming_failure_cleanup_v1() == nullptr ? 0 : 11;
}
"""


def run(command):
    result = subprocess.run(list(map(str, command)), capture_output=True,
                            text=True, timeout=180)
    if result.returncode != 0:
        raise AssertionError((command, result.returncode, result.stdout, result.stderr))
    return result


def main():
    compiler, cxx, runtime, thread_probe = sys.argv[1:]
    root = Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-cleanup-context-") as directory:
        work = Path(directory)
        source = work / "empty.gti"
        source.write_text("int main() { return 0; }\n")
        for standard in ("c++20", "c++23"):
            generated = work / f"{standard}.cpp"
            run([compiler, source, "--std", standard, "--emit-cpp", "-o", generated])
            text, replaced = re.subn(r"^int main\(", "int __gti_scope_original_main(",
                                     generated.read_text(), flags=re.MULTILINE)
            assert replaced == 1, "probe must replace exactly the native entry point"
            cold_helper = ("#if defined(__GNUC__) || defined(__clang__)\n"
                           "__attribute__((__cold__, __noinline__))\n"
                           "#endif\n"
                           "inline void mir_check_incoming_cleanup_failure_v1(")
            assert text.count(cold_helper) == 1, "failure-only check must have its bounded cold hint"
            terminal_helper = ("[[noreturn]]\n"
                               "#if defined(__GNUC__) || defined(__clang__)\n"
                               "__attribute__((__cold__, __noinline__))\n"
                               "#elif defined(_MSC_VER)\n"
                               "__declspec(noinline)\n"
                               "#endif\n"
                               "inline void mir_terminate_cleanup_failure_v2(")
            assert text.count(terminal_helper) == 1, "terminal envelope must stay in its own native frame"
            generated.write_text(text + PROBE)
            for level in ("-O0", "-O3"):
                binary = work / f"{standard}-{level}"
                run([cxx, f"-std={standard}", level,
                     "-fno-fast-math", "-ffp-contract=off",
                     "-D__gti_strict_ieee754=1", generated,
                     "-I", root / "runtime/include",
                     "-I", root / "vendor/expected_lite/include",
                     runtime, "-pthread", "-o", binary])
                result = run([binary])
                assert not result.stdout and not result.stderr, result
    run([thread_probe, "valid"])
    run([thread_probe, "stored"])
    for mutation in ("code", "site", "message", "descriptor", "third", "body"):
        result = subprocess.run([thread_probe, mutation], capture_output=True,
                                text=True, timeout=30)
        expected = (3, 0xC0000409, 0xC0000409 - (1 << 32)) if os.name == "nt" else (
            -signal.SIGABRT,)
        assert result.returncode in expected, (mutation, result)
    print("Cleanup context: four native scope restoration/isolation cases and "
          "exact/forged managed envelope cases passed")


if __name__ == "__main__":
    main()
