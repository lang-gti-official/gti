#!/usr/bin/env python3
"""A failed return continuation cleans its initialized, unpublished result."""

import os
import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/return_result_cleanup.gti"
    cleanup = next(i + 1 for i, line in enumerate(source.read_text().splitlines())
                   if "1 / this.divisor" in line)
    primary = (f"{source.name}:{cleanup}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    double = (f"{source.name}:{cleanup}: runtime error[GTI-R0014]: "
              f"failure_during_cleanup; primary [GTI-R0002] at "
              f"{source.name}:{cleanup}; secondary [GTI-R0002]\n")
    expected = [(0, "2\n1\n9\n", ""), (70, "2\n1\n9\n", primary),
                (70, "2\n1\n", double)] * 5
    expected += [(0, "2\n1\n9\n", ""), (70, "2\n1\n9\n", primary)]
    expected += [(0, "2\n1\n9\n", ""), (70, "2\n1\n9\n", primary),
                 (70, "2\n1\n", double)]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-return-result-") as temporary:
        for label, flags, native in modes:
            binary = pathlib.Path(temporary) / label
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=scaled(120), env=environment)
            assert built.returncode == 0, (label, built.stdout, built.stderr)
            for count, wanted in enumerate(expected):
                run = subprocess.run([str(binary), *(["fail"] * count)],
                                     capture_output=True, text=True,
                                     timeout=scaled(30), env=environment)
                assert (run.returncode, run.stdout, run.stderr) == wanted, (
                    label, count, run, wanted)
            print(f"return-result cleanup: {label} passed", flush=True)


if __name__ == "__main__":
    main()
