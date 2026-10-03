#!/usr/bin/env python3
"""Final-slot scalar-input construction preserves exact failure and cleanup."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def run(command, *, env=None):
    environment = dict(os.environ if env is None else env)
    environment["ASAN_OPTIONS"] = "halt_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=120, env=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode")
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixture = pathlib.Path(__file__).parent / "fixtures/vector_scalar_constructor_failure.gti"
    lines = fixture.read_text().splitlines()

    def line(marker):
        return next(index for index, text in enumerate(lines, 1) if marker in text)

    def failure(marker, primary=None):
        location = f"{fixture.name}:{line(marker)}"
        if primary is None:
            return f"{location}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"
        return (f"{location}: runtime error[GTI-R0014]: failure_during_cleanup; "
                f"primary [GTI-R0002] at {fixture.name}:{line(primary)}; "
                "secondary [GTI-R0002]\n")

    first = "401\n402\n1\n403\n404\n"
    success = first + "2\n300\n200\n12\n100\n11\n900\n"
    cases = [
        (0, success, ""),
        (70, first + "2\n12\n100\n11\n900\n", failure("// constructor failure")),
        (70, success, failure("// cleanup failure")),
        (70, first + "2\n12\n100\n", failure("// cleanup failure", "// constructor failure")),
        (70, first + "2\n300\n200\n12\n100\n", failure("// cleanup failure", "// cleanup failure")),
        (70, first + "100\n11\n900\n", failure("// later argument failure")),
    ]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("asan-ubsan", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if args.mode:
        modes = [mode for mode in modes if mode[0] == args.mode]
        if not modes:
            parser.error("unknown --mode")
    with tempfile.TemporaryDirectory(prefix="gti-vector-scalar-constructor-") as temporary:
        for mode, flags, native in modes:
            binary = pathlib.Path(temporary) / mode
            command = [str(compiler), str(fixture), *flags, "--keep-cpp", "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = run(command)
            if built.returncode:
                raise AssertionError(f"{mode} build:\n{built.stdout}{built.stderr}")
            generated = binary.with_suffix(".gti.cpp").read_text()
            if "mir_prefix_construct_v1(" not in generated or "&__gti_prefix_constructed" not in generated:
                raise AssertionError("missing checked final-slot constructor witness")
            for count, expected in enumerate(cases):
                result = run([str(binary), *(["fail"] * count)])
                actual = (result.returncode, result.stdout, result.stderr)
                if actual != expected:
                    raise AssertionError(f"{mode}/{count}: {actual!r} != {expected!r}")
            print(f"scalar constructor: {mode} passed ({len(cases)} outcomes)", flush=True)


if __name__ == "__main__":
    main()
