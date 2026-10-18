#!/usr/bin/env python3
"""One return conversion matches explicit construction, ownership and failure."""
import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

CASES = [
    (0, 0, [7, 99, -99, 7, -7, 7, 99, -99, 7, -7,
            109, 9, -109, 9, -9, 109, 9, -109, 9, -9, 11, -11, 11, 75], None),
    (1, 70, [88, 8, -8, -88], ("ordinary", "failing")),
    (2, 70, [88, 8, -8, -88], ("ordinary", "failing")),
]
SITES["implicit_return_construction.gti"] = {
    "failing": ": kept(std::move(value)), ratio(20 / divisor) {}",
}

def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "implicit_return_construction.gti"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-implicit-return-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / name
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(value) for value in result.stdout.split()],
                          result.stderr)
                expected = (code, trace, expected_report(source, report))
                assert actual == expected, (name, phase, actual, expected)
            print(f"implicit return: {name} passed ({len(CASES)} phases)", flush=True)

if __name__ == "__main__":
    main()
