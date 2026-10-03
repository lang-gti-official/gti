#!/usr/bin/env python3
"""Direct and pack-generated prefix movement retain exact failure cleanup."""

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
    fixture = pathlib.Path(__file__).parent / "fixtures/prefix_generated_transport.gti"
    helper = fixture.parent / "prefix_constructor_test.gti"

    def line(path, marker):
        return next(index for index, text in enumerate(path.read_text().splitlines(), 1)
                    if marker in text)

    cleanup = f"{fixture.name}:{line(fixture, '// token cleanup')}"
    cases = []
    for scenario in range(8):
        marker = "// direct capacity" if scenario % 2 == 0 else "// pack capacity"
        capacity = f"<std/prefix_constructor_test>:{line(helper, marker)}"
        if scenario < 2:
            error = f"{capacity}: runtime error[GTI-R0010]: invalid_storage_state in relocation_capacity\n"
            cases.append((70, "89\n900\n", error))
        elif scenario < 4:
            cases.append((0, "89\n900\n", ""))
        elif scenario < 6:
            error = (f"{cleanup}: runtime error[GTI-R0014]: failure_during_cleanup; "
                     f"primary [GTI-R0010] at {capacity}; secondary [GTI-R0002]\n")
            cases.append((70, "89\n", error))
        else:
            error = f"{cleanup}: runtime error[GTI-R0002]: division_by_zero in integer_division\n"
            cases.append((70, "89\n900\n", error))

    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("asan-ubsan", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    if args.mode:
        modes = [mode for mode in modes if mode[0] == args.mode]
        if not modes:
            parser.error("unknown --mode")
    with tempfile.TemporaryDirectory(prefix="gti-prefix-generated-transport-") as directory:
        root = pathlib.Path(directory)
        stdlib = root / "stdlib"
        shutil.copytree(fixture.parent.parent.parent / "stdlib", stdlib)
        shutil.copy2(helper, stdlib / "std/prefix_constructor_test.gti")
        source = root / fixture.name
        source.write_text(fixture.read_text().replace(
            '#include "prefix_constructor_test.gti"',
            '#include <std/prefix_constructor_test>'))
        environment = dict(os.environ, GTI_STDLIB_PATH=str(stdlib))
        for mode, flags, native in modes:
            binary = root / mode
            command = [str(args.compiler.resolve()), str(source), *flags, "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = run(command, env=environment)
            if built.returncode:
                raise AssertionError(f"{mode} build:\n{built.stdout}{built.stderr}")
            for count, expected in enumerate(cases):
                result = run([str(binary), *(["case"] * count)])
                actual = (result.returncode, result.stdout, result.stderr)
                if actual != expected:
                    raise AssertionError(f"{mode}/{count}: {actual!r} != {expected!r}")
            print(f"generated prefix transport: {mode} passed ({len(cases)} outcomes)", flush=True)


if __name__ == "__main__":
    main()
