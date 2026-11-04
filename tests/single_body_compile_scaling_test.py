#!/usr/bin/env python3
"""Compiling one function body must scale about linearly with its length.

MIR verification, backend emission planning and HIR full-expression
bookkeeping once rescanned the whole body for each instruction, value or place
they examined. A main() of about 1,100 formatting statements then ran for more
than 18 CPU-minutes while the same statements split into small functions
compiled in seconds.

The same block of statements is repeated in one body at two lengths, and the
compiler's own CPU time (not wall time) is compared after subtracting the cost
of a single block, which carries the fixed standard-library and native link
work. The shipped LLVM backend builds each program (tests/backend_modes.py);
the frozen C++ backend cannot emit the formatting calls' scalar temporary
reference arguments. Four times
the statements must cost well under eight times the work: linear growth costs
about four times, while the former quadratic and cubic searches cost sixteen
times or far more. A separate 90/360-block MIR-only profile caps growth at
six times, exposing compiler-phase scaling without native LLVM/linker costs.
Both profiles use paired baseline subtraction and the median of three samples.
"""

from __future__ import annotations

import resource
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

from backend_modes import llvm_modes

PRELUDE = """import std.format;
import std.string;

void show(expected<std::string, std::format_errc> result) {
  if (result.has_value()) {
    std::println(result.value().view());
  } else {
    std::println("error");
  }
}
"""

# One block covers integers of several widths, chars, text, bools and floats,
# with both the fallible and the infallible formatting entry points.
BLOCK = """  show(std::try_runtime_format("[{:>8}]", int32_t(-12345)));
  show(std::try_runtime_format("[{:#x}]", uint64_t(48879)));
  show(std::try_runtime_format("[{:^7}]", int8_t(7)));
  show(std::try_runtime_format("[{:c}]", 'a'));
  show(std::try_runtime_format("[{:.2}]", "formatting"));
  show(std::try_runtime_format("[{:8}]", std::string("gti")));
  show(std::try_runtime_format("[{:6}]", true));
  show(std::try_runtime_format("[{:.3f}]", double(3.14159265)));
  std::println(std::runtime_format("{}|{:04}", int64_t(-1), uint16_t(2)));
"""

SMALL = 45
LARGE = 4 * SMALL
RUNS = 3
# The large body is four times the small one. Linear work grows by four;
# allow twice that before calling the growth superlinear.
GROWTH_LIMIT = 8.0
# Below this much per-body CPU time the small measurement is mostly noise.
MINIMUM_BODY_SECONDS = 0.2
# The production LLVM build at the GTI optimization level the C++ emission
# measurement used.
BUILD_FLAGS = llvm_modes(("O1",))[0][1]


def program(repetitions: int) -> str:
    return PRELUDE + "\nint main() {\n" + BLOCK * repetitions + "  return 0;\n}\n"


def compile_seconds(compiler: str, source: Path, timeout: float,
                    flags: list[str] = BUILD_FLAGS, *, mir: bool = False) -> float:
    """Return child CPU seconds for one clean native build or MIR emission."""
    output = source.with_suffix(".mir" if mir else ".exe")
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    try:
        result = subprocess.run(
            [compiler, str(source), *flags, "-o", str(output)],
            capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise SystemExit(
            f"{source.name}: compilation did not finish within {timeout:.0f}s "
            "wall time; single-body compile time has regressed superlinearly")
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    if result.returncode != 0:
        raise SystemExit(
            f"{source.name}: compiler exited {result.returncode}\n{result.stderr}")
    if not output.is_file() or output.stat().st_size == 0:
        raise SystemExit(f"{source.name}: no output was produced")
    return ((after.ru_utime - before.ru_utime) +
            (after.ru_stime - before.ru_stime))


def profile(compiler: str, *, mir: bool) -> float:
    # The native check includes LLVM and linking. The larger MIR-only case
    # prevents fixed native-toolchain costs from hiding verifier regressions.
    small_size, limit = (90, 6.0) if mir else (SMALL, GROWTH_LIMIT)
    large_size = 4 * small_size
    flags = ["-O1", "--emit-mir"] if mir else BUILD_FLAGS
    label = "MIR" if mir else "native LLVM"
    with tempfile.TemporaryDirectory(prefix="gti-single-body-") as temporary:
        directory = Path(temporary)
        sources = {}
        for name, repetitions in (("base", 1), ("small", small_size),
                                  ("large", large_size)):
            sources[name] = directory / f"{name}.gti"
            sources[name].write_text(program(repetitions))

        def measure(name: str, timeout: float = 300) -> float:
            return compile_seconds(compiler, sources[name], timeout,
                                   flags, mir=mir)

        for name in sources:
            measure(name)
        growth_samples = []
        for sample in range(RUNS):
            base = measure("base")
            small = measure("small")
            large = measure("large", max(300.0, 40.0 * small))
            small_body = max(small - base, MINIMUM_BODY_SECONDS)
            growth_samples.append(max(large - base, 0.0) / small_body)
            print(f"{label} sample {sample + 1}: cpu seconds base {base:.2f}, "
                  f"{small_size} blocks {small:.2f}, {large_size} blocks {large:.2f}; "
                  f"body growth {growth_samples[-1]:.2f}x", flush=True)

    growth = statistics.median(growth_samples)
    print(f"{label} median body growth {growth:.2f}x for 4x statements")
    if growth > limit:
        raise SystemExit(f"{label} single-body compile time grew {growth:.2f}x "
                         f"for 4x statements (limit {limit:.0f}x)")
    return growth


def main() -> int:
    if len(sys.argv) not in (2, 3) or (len(sys.argv) == 3 and
                                      sys.argv[2] != "--mir-only"):
        print("usage: single_body_compile_scaling_test.py <gti> [--mir-only]",
              file=sys.stderr)
        return 2
    compiler = sys.argv[1]
    if len(sys.argv) == 2:
        profile(compiler, mir=False)
    profile(compiler, mir=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
