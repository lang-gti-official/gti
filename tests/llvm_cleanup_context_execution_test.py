#!/usr/bin/env python3
"""Cross-call cleanup context with ordinary source-defined classes."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from backend_execution import Endpoint, execute


def main():
    gti, root = (Path(value).resolve() for value in sys.argv[1:3])
    fixture = root / "tests/fixtures/llvm_inherited_cleanup_context.gti"
    lines = fixture.read_text().splitlines()

    def site(marker):
        return next(i for i, line in enumerate(lines, 1) if marker in line)

    secondary, primary, later = (site(marker) for marker in
                                ("int secondary =", "int primary =", "int after_success ="))

    def ordinary(line):
        return (f"{fixture.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    emergency = (f"{fixture.name}:{secondary}: runtime error[GTI-R0014]: "
                 "failure_during_cleanup; primary [GTI-R0002] at "
                 f"{fixture.name}:{primary}; secondary [GTI-R0002]\n")
    cases = ((0, "DLO", ""), (70, "DLO", ordinary(secondary)),
             (70, "D", emergency), (70, "DLO", ordinary(later)))
    endpoints = [Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", f"O{level}") for level in range(4))]
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-cleanup-context-") as temporary:
        scratch = Path(temporary)
        for index, endpoint in enumerate(endpoints):
            directory = scratch / str(index)
            result = execute(gti, root, directory, fixture, endpoint, env)
            if result.error or result.observable != cases[0]:
                raise AssertionError(f"normal cleanup {endpoint}: {result}")
            for count, expected in enumerate(cases[1:], 1):
                observed = subprocess.run(
                    [str(directory / "program"), *(["phase"] * count)],
                    cwd=root, env=env, text=True, capture_output=True,
                    check=False, timeout=15)
                actual = observed.returncode, observed.stdout, observed.stderr
                if actual != expected:
                    raise AssertionError(f"cleanup phase {count} {endpoint}: {actual} != {expected}")
            print(f"cleanup context {endpoint}: normal, first, inherited and restored passed",
                  flush=True)

        # Exercise the full inline panic buffer across calls and cleanup drains,
        # including bytes beyond the first cache line and embedded zero bytes.
        message = "abcdefghijklmnopqrstuvwxyz0123456789" * 12
        panic = scratch / "copied-panic.gti"
        panic.write_text('''#include <std/panic>
class Marker {
  char code;
public:
  Marker(char code) : code(code) {}
  ~Marker() { std::print(this.code); }
};
void leaf() {
  Marker local{'L'};
  std::panic("''' + message + r'''\0tail");
}
void middle() { Marker local{'M'}; leaf(); }
int main() { Marker local{'O'}; middle(); return 0; }
''')
        expected = (70, "LMO", 'copied-panic.gti:10: runtime error[GTI-R0017]: panic: "' +
                    message + r'\x00tail' + '"\n')
        for index, endpoint in enumerate(endpoints):
            result = execute(gti, root, scratch / f"panic-{index}", panic, endpoint, env)
            if result.error or result.observable != expected:
                raise AssertionError(f"copied panic {endpoint}: {result} != {expected}")
            print(f"copied panic {endpoint}: full message, source and cleanup order passed", flush=True)


if __name__ == "__main__":
    main()
