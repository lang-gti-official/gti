#!/usr/bin/env python3
"""Checked owning assignment validates, cleans, then commits through MIR."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/checked_owner_replacement.gti"
    lines = source.read_text().splitlines()
    site = lambda text: next(i for i, line in enumerate(lines, 1) if text in line)
    old = site("old_failure = 17 / zero")
    staged = site("staged_failure = 19 / zero")

    def bounds(text):
        return (f"{source.name}:{site(text)}: runtime error[GTI-R0007]: "
                "index_out_of_bounds in fixed_array\n")

    ordinary = (f"{source.name}:{old}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    emergency = (f"{source.name}:{staged}: runtime error[GTI-R0014]: "
                 f"failure_during_cleanup; primary [GTI-R0002] at {source.name}:{old}; "
                 "secondary [GTI-R0002]\n")
    success = (0, "1\n2\n3\n9\n", "")
    first_failure = (70, "1\n3\n2\n9\n", ordinary)
    cases = [
        success,
        first_failure,
        (70, "3\n2\n1\n9\n", bounds("nodes[index] = std::make_unique")),
        success,
        first_failure,
        (70, "1\n3\n", emergency),
        (70, "3\n2\n1\n9\n", bounds("nodes[index] = std::move")),
        success,
        first_failure,
        (70, "3\n2\n1\n9\n", bounds("values[index] = Node")),
        success,
        first_failure,
        (70, "1\n3\n", emergency),
        (70, "3\n2\n1\n9\n", bounds("values[index] = std::move")),
        (0, "9\n", ""),
        (70, "9\n", bounds("scalars[index] = std::move")),
        (0, "3\n9\n", ""),
        # The row selects a subarray before the later column or RHS runs.
        # A terminal column failure still occurs at the replacement commit.
        (70, "9\n", bounds("grid[row][column] =")),
        (70, "3\n9\n", bounds("grid[row][column] =")),
    ]
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-checked-replacement-") as directory:
        rejected = run([str(compiler), str(source.with_name(
            "checked_owner_replacement_copy_rejected.gti")), "--emit-mir",
            "-o", str(pathlib.Path(directory) / "rejected.mir")])
        assert rejected.returncode != 0 and "[GTI-S2003]" in rejected.stderr, (
            rejected.returncode, rejected.stdout, rejected.stderr)
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            built = run([str(compiler), str(source), *flags, "--keep-cpp",
                         "-o", str(binary), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated
            assert "__gti_mir_replacement_success_" in generated
            for phase, expected in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, result.stdout, result.stderr)
                assert actual == expected, (name, phase, actual, expected)
            print(f"checked replacement: {name}: {len(cases)} passed", flush=True)


if __name__ == "__main__":
    main()
