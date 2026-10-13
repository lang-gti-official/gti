#!/usr/bin/env python3
"""Interface-only owner construction preserves guarded destruction and rollback."""

import os
import pathlib
import subprocess
import sys
import tempfile


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    rollback = fixtures / "polymorphic_constructor_rollback.gti"
    lines = rollback.read_text().splitlines()
    constructor = next(i + 1 for i, line in enumerate(lines)
                       if "int constructor_failure" in line)
    cleanup = next(i + 1 for i, line in enumerate(lines)
                   if "int cleanup_failure" in line)
    failure = (f"{rollback.name}:{constructor}: runtime error[GTI-R0002]: "
               "division_by_zero in integer_division\n")
    secondary = (f"{rollback.name}:{cleanup}: runtime error[GTI-R0014]: "
                 "failure_during_cleanup; primary [GTI-R0002] at "
                 f"{rollback.name}:{constructor}; secondary [GTI-R0002]\n")
    expected = [(0, "12\n3\n2\n9\n", ""),
                (70, "2\n9\n", failure),
                (70, "2\n", secondary)]
    context_expected = [(0, "11\n1\n99\n", ""),
                        (0, "11\n99\n1\n", ""),
                        (0, "11\n1\n99\n", ""),
                        (0, "11\n1\n12\n2\n99\n", ""),
                        (0, "1\n99\n", ""),
                        (0, "11\n1\n2\n99\n", "")]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"],
               ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-polymorphic-constructor-") as temp:
        for label, flags, native in modes:
            for source in (fixtures / "polymorphic_context_lifecycle.gti", rollback):
                binary = pathlib.Path(temp) / (source.stem + "-" + label)
                command = [str(compiler), str(source), *flags, "-o", str(binary)]
                if native:
                    command += ["--", *native]
                built = subprocess.run(command, capture_output=True, text=True,
                                       timeout=120, env=environment)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                cases = expected if source == rollback else context_expected
                for count, wanted in enumerate(cases):
                    run = subprocess.run([str(binary), *(["fail"] * count)],
                                         capture_output=True, text=True,
                                         timeout=30, env=environment)
                    assert (run.returncode, run.stdout, run.stderr) == wanted, (
                        source.name, label, count, run, wanted)
            print(f"polymorphic constructor rollback: {label} passed", flush=True)


if __name__ == "__main__":
    main()
