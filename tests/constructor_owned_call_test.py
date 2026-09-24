#!/usr/bin/env python3
"""Constructor call handoff retains preparation, callee, and rollback cleanup."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: constructor_owned_call_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/constructor_owned_call.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    argument = site("consume(Resource(2)")
    callee = site("callee_failure =")
    constructor = site("constructor_failure =")
    secondary = site("secondary =")

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    cases = [
        (0, "2\n3\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(argument)),
        (70, "2\n1\n9\n", ordinary(callee)),
        (70, "2\n3\n1\n9\n", ordinary(constructor)),
        (70, "2\n",
         f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
         "failure_during_cleanup; primary [GTI-R0002] at "
         f"{source.name}:{callee}; secondary [GTI-R0002]\n"),
        (70, "2\n",
         f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
         "failure_during_cleanup; primary [GTI-R0002] at "
         f"{source.name}:{argument}; secondary [GTI-R0002]\n"),
    ]
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-constructor-owned-call-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, phase, actual, expected)
            print(f"constructor owned call: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
