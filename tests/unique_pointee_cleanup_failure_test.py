#!/usr/bin/env python3
"""First and secondary failures in MIR-owned unique-pointee cleanup."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unique_pointee_cleanup_failure_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/unique_pointee_cleanup_failure.gti"
    inherited = source.with_name("inherited_constructor_cleanup_failure.gti")
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    secondary = site("secondary = 11 / zero")
    primary = site("return 7 / zero")
    field = site("checked(13 / zero)")
    inherited_lines = inherited.read_text().splitlines()
    inherited_primary = next(i for i, line in enumerate(inherited_lines, 1)
                             if "checked(7 / zero)" in line)
    inherited_secondary = next(i for i, line in enumerate(inherited_lines, 1)
                               if "secondary = 11 / zero" in line)

    def ordinary(line):
        return (f"{source.name}:{line}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")

    def emergency(line):
        return (f"{source.name}:{secondary}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0002] at "
                f"{source.name}:{line}; secondary [GTI-R0002]\n")

    cases = [(0, "10\n9\n", ""),
             (70, "10\n9\n", ordinary(secondary)),
             (70, "10\n9\n", ordinary(primary)),
             (70, "10\n", emergency(primary)),
             (70, "10\n", emergency(field)),
             (0, "9\n", "")]
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-pointee-cleanup-") as directory:
        root = pathlib.Path(directory)
        for name, flags, native in modes:
            binary = root / name
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "destroy_unique_aggregate_with_failure<" in generated
            assert "mir_unique_owner_drop_with_failure_v1(" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, phase, result, expected)
            inherited_binary = root / f"{name}-base"
            built = run([str(compiler), str(inherited), *flags, "--keep-cpp",
                         "-o", str(inherited_binary), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(inherited_binary)])
            expected = (70, "base-cleanup\n",
                        f"{inherited.name}:{inherited_secondary}: runtime error[GTI-R0014]: "
                        "failure_during_cleanup; primary [GTI-R0002] at "
                        f"{inherited.name}:{inherited_primary}; secondary [GTI-R0002]\n")
            assert (result.returncode, result.stdout, result.stderr) == expected, (
                name, result, expected)
            print(f"unique-pointee cleanup: {name} passed", flush=True)


if __name__ == "__main__":
    main()
