#!/usr/bin/env python3
"""Compiling one function body must scale about linearly with its length.

MIR verification, C++ emission planning and HIR full-expression bookkeeping
once rescanned the whole body for each instruction, value or place they
examined. A main() of about 1,100 formatting statements then ran for more
than 18 CPU-minutes while the same statements split into small functions
compiled in seconds.

The same block of statements is repeated in one body at two lengths, and the
compiler's own CPU time (not wall time) is compared after subtracting the cost
of a single block, which carries the fixed standard-library work. Four times
the statements must cost well under eight times the work: linear growth costs
about four times, while the former quadratic and cubic searches cost sixteen
times or far more.
"""

from __future__ import annotations

import resource
import subprocess
import sys
import tempfile
from pathlib import Path
from harness_timeouts import scaled

PRELUDE = """#include <std/format>
#include <std/string>

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
BLOCK = """  show(std::try_format("[{:>8}]", int32_t(-12345)));
  show(std::try_format("[{:#x}]", uint64_t(48879)));
  show(std::try_format("[{:^7}]", int8_t(7)));
  show(std::try_format("[{:c}]", 'a'));
  show(std::try_format("[{:.2}]", "formatting"));
  show(std::try_format("[{:8}]", std::string("gti")));
  show(std::try_format("[{:6}]", true));
  show(std::try_format("[{:.3f}]", double(3.14159265)));
  std::println(std::format("{}|{:04}", int64_t(-1), uint16_t(2)));
"""

SMALL = 45
LARGE = 4 * SMALL
RUNS = 2
# The large body is four times the small one. Linear work grows by four;
# allow twice that before calling the growth superlinear.
GROWTH_LIMIT = 8.0
# Below this much per-body CPU time the small measurement is mostly noise.
MINIMUM_BODY_SECONDS = 0.2


def program(repetitions: int) -> str:
    return PRELUDE + "\nint main() {\n" + BLOCK * repetitions + "  return 0;\n}\n"


def compile_seconds(compiler: str, source: Path, timeout: float) -> float:
    """Return the child CPU seconds of one clean --emit-cpp compilation."""
    output = source.with_suffix(".cpp")
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    try:
        result = subprocess.run(
            [compiler, str(source), "-O1", "--emit-cpp", "-o", str(output)],
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
        raise SystemExit(f"{source.name}: no C++ was emitted")
    return ((after.ru_utime - before.ru_utime) +
            (after.ru_stime - before.ru_stime))


def fastest(compiler: str, source: Path, timeout: float) -> float:
    return min(compile_seconds(compiler, source, timeout) for _ in range(RUNS))


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: single_body_compile_scaling_test.py <gti>", file=sys.stderr)
        return 2
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="gti-single-body-") as temporary:
        directory = Path(temporary)
        sources = {}
        for name, repetitions in (("base", 1), ("small", SMALL),
                                  ("large", LARGE)):
            sources[name] = directory / f"{name}.gti"
            sources[name].write_text(program(repetitions))

        base = fastest(compiler, sources["base"], timeout=scaled(300))
        small = fastest(compiler, sources["small"], timeout=scaled(300))
        # A superlinear regression is reported as a timeout instead of
        # stalling the suite for many minutes.
        large = fastest(compiler, sources["large"],
                        timeout=scaled(max(300.0, 40.0 * small)))

    small_body = max(small - base, MINIMUM_BODY_SECONDS)
    large_body = max(large - base, 0.0)
    growth = large_body / small_body
    print(f"cpu seconds: base {base:.2f}, {SMALL} blocks {small:.2f}, "
          f"{LARGE} blocks {large:.2f}; body growth {growth:.2f}x for "
          f"{LARGE // SMALL}x statements")
    if growth > GROWTH_LIMIT:
        print(f"single-body compile time grew {growth:.2f}x for "
              f"{LARGE // SMALL}x statements (limit {GROWTH_LIMIT:.0f}x)",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
