#!/usr/bin/env python3
"""Ordered multi-owner cleanup with first/secondary failure preservation."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: aggregate_cleanup_failure_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/aggregate_cleanup_failure.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    first = site("first_failure = 11 / zero")
    second = site("second_failure = 13 / zero")
    constructor = site("checked(17 /")
    field = site("checked(19 / zero)")
    body = site("return 23 / zero")
    inherited = source.with_name("inherited_multi_owner_cleanup_failure.gti")
    inherited_lines = inherited.read_text().splitlines()
    inherited_primary = next(i for i, line in enumerate(inherited_lines, 1)
                             if "checked(7 / zero)" in line)
    inherited_secondary = next(i for i, line in enumerate(inherited_lines, 1)
                               if "secondary = 11 / zero" in line)

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(primary, secondary):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{primary}; secondary [GTI-R0002]\n")

    cases = [
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(second)),
        (70, "2\n1\n9\n", ordinary(constructor)),
        (70, "2\n", emergency(constructor, second)),
        (70, "2\n1\n", emergency(constructor, first)),
        (70, "2\n", emergency(body, second)),
        (70, "2\n1\n", emergency(second, first)),
        (0, "2\n1\n9\n", ""),
        (70, "2\n1\n9\n", ordinary(field)),
        (70, "2\n", emergency(field, second)),
        (0, "2\n1\n9\n", ""),
        (0, "9\n", ""),
    ]
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-aggregate-cleanup-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "mir_cleanup_sequence_v1 sequence" in generated
            assert "mir_unique_aggregate_owner_drop_with_failure_v1<" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, phase, actual, expected)
            original = pathlib.Path(directory) / f"{name}-original"
            built = run([str(compiler), str(inherited), *flags, "--keep-cpp",
                         "-o", str(original), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(original)])
            expected = (70, "cleanup\n",
                        f"{inherited.name}:{inherited_secondary}: runtime error[GTI-R0014]: "
                        "failure_during_cleanup; primary [GTI-R0002] at "
                        f"{inherited.name}:{inherited_primary}; secondary [GTI-R0002]\n")
            assert (result.returncode, result.stdout, result.stderr) == expected, (
                name, result, expected)
            print(f"aggregate cleanup: {name} passed ({len(cases) + 1} cases)", flush=True)


if __name__ == "__main__":
    main()
