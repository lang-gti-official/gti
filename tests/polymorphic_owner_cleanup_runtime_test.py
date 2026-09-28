#!/usr/bin/env python3
"""Erasing an owner retains complete-object cleanup and failure propagation."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/polymorphic_owner_cleanup.gti"
    lines = source.read_text().splitlines()

    def site(fragment):
        return next(index + 1 for index, line in enumerate(lines) if fragment in line)

    def failure(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def double(primary, secondary):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                f"failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    field = site("int trace_failure")
    model = site("int model_failure")
    call = site("return 7 / divisor")
    complete = "3\n2\n1\n9\n"
    expected = [(0, complete, ""), (70, complete, failure(model)),
                (70, complete, failure(field)), (70, complete, failure(call)),
                (70, "3\n2\n", double(model, field)),
                (70, "3\n", double(call, model))]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-polymorphic-owner-") as temporary:
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
            print(f"polymorphic-owner cleanup: {label} passed", flush=True)


if __name__ == "__main__":
    main()
