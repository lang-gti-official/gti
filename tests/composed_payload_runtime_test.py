#!/usr/bin/env python3
"""Recursive sum/array/result payload ownership and partial-failure cleanup."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run

CASES = [
    (0, 0, [1, -1, 2, 3, -2, -3, 4, 5, -5, -4, 6, -6]),
    (1, 70, [20, -20]),
    (2, 70, [30, 31, -31, -30]),
]


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "composed_payloads.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-composed-payloads-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()])
                assert actual == (code, trace), (name, phase, actual, result.stderr)
                if phase:
                    assert "GTI-R" in result.stderr, (name, phase, result.stderr)
            print(f"composed payloads: {name} passed", flush=True)


if __name__ == "__main__":
    main()
