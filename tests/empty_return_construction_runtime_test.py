#!/usr/bin/env python3
"""Bare returns use ordinary constructor/default-input failure and cleanup."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/empty_return_construction.gti"
    lines = source.read_text().splitlines()

    def failure(fragment):
        line = next(index + 1 for index, text in enumerate(lines) if fragment in text)
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    expected = [(0, "2\n3\n9\n", ""),
                (70, "3\n2\n9\n", failure("value(10 / divisor)")),
                (70, "2\n9\n", failure("int argument_failure"))]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-empty-return-") as temporary:
        for label, flags, native in modes:
            binary = pathlib.Path(temporary) / label
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, env=environment)
            assert built.returncode == 0, (label, built.stdout, built.stderr)
            for count, wanted in enumerate(expected):
                executed = subprocess.run([str(binary), *(["fail"] * count)],
                                          capture_output=True, text=True,
                                          timeout=30, env=environment)
                assert (executed.returncode, executed.stdout, executed.stderr) == wanted, (
                    label, count, executed, wanted)
            print(f"empty-return cleanup: {label} passed", flush=True)


if __name__ == "__main__":
    main()
