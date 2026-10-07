#!/usr/bin/env python3
"""Void expression returns preserve evaluation, cleanup, and failure order."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/void_return_expression.gti"
    lines = source.read_text().splitlines()

    def site(fragment):
        return next(index + 1 for index, line in enumerate(lines) if fragment in line)

    cleanup = site("int cleanup_failure")
    call = site("int call_failure")

    def failure(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def double(primary):
        return (f"{source.name}:{cleanup}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    complete = "4\n3\n2\n9\n"
    expected = [(0, complete, ""), (70, complete, failure(call)),
                (70, complete, failure(cleanup)), (70, complete, failure(cleanup)),
                (70, "4\n3\n2\n", double(call)),
                (70, "4\n3\n2\n", double(cleanup))]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-void-return-") as temporary:
        for label, flags, native in modes:
            binary = pathlib.Path(temporary) / label
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   timeout=120, env=environment)
            assert built.returncode == 0, (label, built.stdout, built.stderr)
            for count, wanted in enumerate(expected):
                run = subprocess.run([str(binary), *(["fail"] * count)],
                                     capture_output=True, text=True,
                                     timeout=30, env=environment)
                assert (run.returncode, run.stdout, run.stderr) == wanted, (
                    label, count, run, wanted)
            print(f"void-return cleanup: {label} passed", flush=True)


if __name__ == "__main__":
    main()
