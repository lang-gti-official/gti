#!/usr/bin/env python3
"""Exercise binary32 decimal text output end to end."""

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


SOURCE = """\
#include <std/string>

int main() {
  // Exact halves and quarters are exact in binary32.
  std::println(std::to_string(1.5));
  std::println(std::to_string(-3.25));

  // 2.675 is stored as 2.6749999523162841796875: the seventh exact digit is
  // nine, so half-up rounding must carry into "2.675000".
  std::println(std::to_string(2.675));

  // 0.1 is stored slightly above one tenth.
  std::println(std::to_string(0.1));

  // Zero and the 2^24 boundary where fractional bits disappear.
  std::println(std::to_string(0.0));
  std::println(std::to_string(16777216.0));

  // Beyond int64 range: the digits come from exact decimal doubling.
  float huge = 1000000000000000000000000000000.0;
  std::println(std::to_string(huge));

  // Below the sixth fractional digit.
  std::println(std::to_string(0.0000001));

  // Non-finite values.
  float infinite = huge * huge;
  std::println(std::to_string(infinite));
  std::println(std::to_string(-infinite));
  std::println(std::to_string(infinite - infinite));

  // print/println overloads share the same contract.
  std::print(1.5);
  std::print(" ");
  std::println(-2.25);
  return 0;
}
"""

# Every expected line is `printf("%f", (float)x)` from C, so the contract is
# pinned against an independent implementation rather than against itself.
EXPECTED = """\
1.500000
-3.250000
2.675000
0.100000
0.000000
16777216.000000
1000000015047466219876688855040.000000
0.000000
inf
-inf
nan
1.500000 -2.250000
"""


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: float_text_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-float-text-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "float-text.gti"
        source.write_text(SOURCE, encoding="utf8")

        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"float-text-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(source),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(built)

                executed = run([str(executable)])
                if executed.returncode != 0:
                    return fail(executed)
                if executed.stdout != EXPECTED:
                    sys.stderr.write(
                        f"binary32 decimal text mismatch for {standard} "
                        f"{optimization}:\nexpected:\n{EXPECTED}\nactual:\n"
                        f"{executed.stdout}"
                    )
                    return 1

    print("binary32 decimal text verified against the C %f reference")
    return 0


if __name__ == "__main__":
    sys.exit(main())
