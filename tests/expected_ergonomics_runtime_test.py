#!/usr/bin/env python3
"""Owned expected mapping, chaining, and panic extraction share MIR cleanup."""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command, environment):
    return subprocess.run(command, text=True, capture_output=True,
                          env=environment, timeout=scaled(120))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    fixture = pathlib.Path(__file__).parent / "fixtures" / "expected_ergonomics.gti"
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = [(standard, level, ["-fno-elide-constructors"])
             for standard in ("20", "23")
             for level in ("0", "1", "3")]
    modes.append(("23", "1", ["-fsanitize=address,undefined",
                               "-fno-omit-frame-pointer"]))
    if arguments.quick:
        modes = [("23", "0", ["-fno-elide-constructors"])]
    lines = fixture.read_text().splitlines()

    def location(fragment):
        line = next(index + 1 for index, text in enumerate(lines)
                    if fragment in text)
        return f"expected_ergonomics.gti:{line}"

    cases = [
        (0, 0, "1\n2\n10\n3\n110\n1110\n1110\n99\n", ()),
        (1, 70, "1\n20\n99\n", ("GTI-R0017", "required application value",
                                       location('.and_then(next).expect('))),
        (2, 70, "1\n2\n10\n99\n", ("GTI-R0002", "division_by_zero",
                                           location("1 / (argc - 3)"))),
        (3, 70, "1\n20\n", ("GTI-R0014", "GTI-R0017", "GTI-R0002")),
        (4, 70, "1\n4\n10\n99\n", ("GTI-R0002", "division_by_zero",
                                           location("return 1 / divisor"))),
        (5, 0, "5\n6\n7\n5\n30\n30\n99\n", ()),
        (6, 70, "1\n20\n80\n99\n", ("GTI-R0017", "skipped mapper")),
        (7, 0, "1\n8\n10\n10\n99\n", ()),
        (8, 70, "5\n20\n99\n", ("GTI-R0017", "failed void work")),
        (9, 70, "1\n10\n99\n", ("GTI-R0017", "terminal mapper")),
        (10, 0, "1\n20\n20\n99\n", ()),
        (11, 70, "3\n40\n1040\n99\n", ("GTI-R0002", "division_by_zero")),
        (12, 0, "collect\nvalidate\nalpha\nbeta\n99\n", ()),
        (13, 70, "99\n", ("GTI-R0017", "required words",
                           location('.and_then(validate_words).expect('))),
        (14, 0, "7\n17\n99\n", ()),
        (15, 70, "99\n", ("GTI-R0009", "invalid_expected_access")),
        (16, 0, "map packet\nvalidate packet\nmessage\n99\n", ()),
        (17, 70, "99\n", ("GTI-R0017", "required packet",
                           location('.and_then(validate_packet).expect('))),
        (18, 0, "51\n99\n", ()),
        (19, 70, "70\n99\n", ("GTI-R0009", "invalid_expected_access")),
        (20, 0, "71\n99\n", ()),
        (21, 70, "99\n", ("GTI-R0009", "invalid_expected_access")),
        (22, 0, "4\n9\n82\n81\n99\n", ()),
        (23, 70, "4\n82\n81\n99\n", ("GTI-R0002", "division_by_zero",
                                          location("return 1 / divisor"))),
        (24, 0, "4\n9\n83\n82\n81\n99\n", ()),
        (25, 70, "4\n83\n82\n81\n99\n", ("GTI-R0002", "division_by_zero",
                                              location("return 1 / divisor"))),
        (26, 0, "4\n9\n83\n82\n81\n99\n", ()),
        (27, 70, "4\n83\n82\n81\n99\n", ("GTI-R0002", "division_by_zero",
                                              location("return 1 / divisor"))),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-expected-ergonomics-") as temp:
        root = pathlib.Path(temp)
        for standard, level, native in modes:
            executable = root / f"expected-cpp{standard}-O{level}"
            command = [str(compiler), str(fixture), "--std", f"c++{standard}",
                       f"-O{level}", "-o", str(executable), "--", *native]
            built = run(command, environment)
            if built.returncode:
                sys.stderr.write(f"Build failed: {command!r}\n{built.stdout}{built.stderr}")
                return 1
            for count, status, stdout, fragments in cases:
                executed = run([str(executable), *(["case"] * count)], environment)
                if (executed.returncode != status or executed.stdout != stdout or
                        (not fragments and executed.stderr) or
                        any(fragment not in executed.stderr for fragment in fragments)):
                    sys.stderr.write(
                        f"cpp{standard}/O{level}, case {count}: expected "
                        f"{status}, {stdout!r}, {fragments!r}; got "
                        f"{executed.returncode}, {executed.stdout!r}, {executed.stderr!r}\n")
                    return 1
            print(f"expected ergonomics: C++{standard}/O{level}, {len(cases)} outcomes",
                  flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
