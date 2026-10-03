#!/usr/bin/env python3
"""Final-slot non-scalar construction preserves preflight ownership and cleanup."""

import argparse
import os
import pathlib
import shutil
import tempfile

from vector_scalar_constructor_runtime_test import run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--mode")
    args = parser.parse_args()
    fixture = pathlib.Path(__file__).parent / "fixtures/vector_prepared_constructor_failure.gti"
    lines = fixture.read_text().splitlines()
    helper = fixture.parent / "prefix_constructor_test.gti"
    helper_lines = helper.read_text().splitlines()
    capacity_line = next(index for index, text in enumerate(helper_lines, 1)
                         if "// capacity failure" in text)
    capacity_failure = (f"<std/prefix_constructor_test>:{capacity_line}: "
                        "runtime error[GTI-R0010]: invalid_storage_state in relocation_capacity\n")

    def line(marker):
        return next(index for index, text in enumerate(lines, 1) if marker in text)

    def failure(marker, primary=None):
        location = f"{fixture.name}:{line(marker)}"
        if primary is None:
            return f"{location}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"
        return (f"{location}: runtime error[GTI-R0014]: failure_during_cleanup; "
                f"primary [GTI-R0002] at {fixture.name}:{line(primary)}; secondary [GTI-R0002]\n")

    first = "11\nowner\n"
    second = "12\nowner\n"
    success = first + second + "300\n112\n12\n111\n11\n900\n"
    cases = [
        (0, success, ""),
        (70, first + second + "12\n111\n11\n900\n", failure("// constructor failure")),
        (70, success, failure("// cleanup failure")),
        (70, first + second + "12\n111\n", failure("// cleanup failure", "// constructor failure")),
        (70, first + second + "300\n112\n12\n111\n", failure("// cleanup failure", "// cleanup failure")),
        (70, first + "12\n111\n11\n900\n", failure("// later argument failure")),
        (70, "77\n900\n", capacity_failure),
        (70, "88\n900\n", capacity_failure),
        (70, "89\n900\n", capacity_failure),
        (0, "91\n90\n900\n", ""),
        (0, "900\n", ""),
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
    with tempfile.TemporaryDirectory(prefix="gti-vector-prepared-constructor-") as directory:
        # Capability controls live in a disposable trusted library unit. The
        # application fixture still has no permission to name gti_internal.
        stdlib = pathlib.Path(directory) / "stdlib"
        shutil.copytree(fixture.parent.parent.parent / "stdlib", stdlib)
        shutil.copy2(helper, stdlib / "std/prefix_constructor_test.gti")
        application = pathlib.Path(directory) / fixture.name
        application.write_text(fixture.read_text().replace(
            '#include "prefix_constructor_test.gti"',
            '#include <std/prefix_constructor_test>'))
        environment = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
        for mode, flags, native in modes:
            binary = pathlib.Path(directory) / mode
            command = [str(args.compiler.resolve()), str(application), *flags,
                       "--keep-cpp", "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = run(command, env=environment)
            if built.returncode:
                raise AssertionError(f"{mode} build:\n{built.stdout}{built.stderr}")
            text = binary.with_suffix(".gti.cpp").read_text()
            if "mir_prefix_preflight_v1(" not in text or "mir_prefix_construct_v1(" not in text:
                raise AssertionError("missing separately verified preflight and final-slot construction")
            for count, expected in enumerate(cases):
                result = run([str(binary), *(["fail"] * count)])
                actual = (result.returncode, result.stdout, result.stderr)
                if actual != expected:
                    raise AssertionError(f"{mode}/{count}: {actual!r} != {expected!r}")
            print(f"prepared constructor: {mode} passed ({len(cases)} outcomes)", flush=True)


if __name__ == "__main__":
    main()
