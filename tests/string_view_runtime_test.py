#!/usr/bin/env python3
"""Source-defined std::string_view: behavior, bounds failures, and cost."""

import os
import pathlib
import subprocess
import sys
import tempfile
import time

EXPECTED_OUTPUT = "string_view ok\n"
EXAMPLE_OUTPUT = ("world\ngreeting\ncompile\nfound care\n3\nupdated\n"
                  "mode -> release\n")
FAILURE_DETAIL = "index_out_of_bounds in string_view"


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    fixtures = repository / "tests/fixtures"
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))

    def run(command, **extra):
        return subprocess.run(command, env=environment, capture_output=True,
                              text=True, timeout=180, **extra)

    with tempfile.TemporaryDirectory(prefix="gti-string-view-") as directory:
        root = pathlib.Path(directory)
        for level in ("O0", "O3"):
            example = root / f"example-{level}"
            built = run([str(compiler),
                         str(repository / "examples/130-string-views.gti"),
                         f"-{level}", "-o", str(example)])
            assert built.returncode == 0, built.stderr
            executed = run([str(example)])
            assert (executed.returncode, executed.stdout, executed.stderr) == \
                (0, EXAMPLE_OUTPUT, ""), executed

            binary = root / f"operations-{level}"
            built = run([str(compiler), str(fixtures / "string_view_operations.gti"),
                         f"-{level}", "-o", str(binary)])
            assert built.returncode == 0, built.stderr
            executed = run([str(binary)])
            assert (executed.returncode, executed.stdout, executed.stderr) == \
                (0, EXPECTED_OUTPUT, ""), executed

            bounds = root / f"bounds-{level}"
            built = run([str(compiler), str(fixtures / "string_view_bounds.gti"),
                         f"-{level}", "-o", str(bounds)])
            assert built.returncode == 0, built.stderr
            for count in range(7):
                executed = run([str(bounds), *(["x"] * count)])
                assert executed.returncode == 70 and executed.stdout == "99\n" and \
                    "runtime error[GTI-R0007]" in executed.stderr and \
                    FAILURE_DETAIL in executed.stderr, (level, count, executed)

        # The source-defined search compiles to the same kind of loop as the
        # owning string's source search; it must not be materially slower.
        benchmark = root / "benchmark.gti"
        benchmark.write_text(
            '#include <std/string>\n#include <std/vector>\n'
            'int main(int argc, std::vector<std::string> argv) {\n'
            '  mut std::string text = std::string();\n'
            '  for (mut int index = 0; index < 4096; index++) { text.push_back(char(uint8_t(97 + index % 23))); }\n'
            '  text.append("needle");\n'
            '  mut std::size_t found = 0;\n'
            '  for (mut int round = 0; round < 3000; round++) {\n'
            '    if (argc > 1) { found += text.view().find("needle"); }\n'
            '    else { found += text.find("needle"); }\n'
            '  }\n'
            '  return found == std::size_t(3000) * std::size_t(4096) ? 0 : 1;\n'
            '}\n')
        binary = root / "benchmark"
        built = run([str(compiler), str(benchmark), "-O3", "-o", str(binary)])
        assert built.returncode == 0, built.stderr

        def measure(arguments):
            best = None
            for _ in range(5):
                start = time.perf_counter()
                executed = run([str(binary), *arguments])
                elapsed = time.perf_counter() - start
                assert executed.returncode == 0, executed
                best = elapsed if best is None else min(best, elapsed)
            return best

        owning = measure([])
        viewed = measure(["view"])
        print(f"string::find {owning:.4f}s, string_view::find {viewed:.4f}s")
        assert viewed <= owning * 2.0 + 0.05, (owning, viewed)
    print("string_view runtime behavior, bounds, and cost passed")


if __name__ == "__main__":
    main()
