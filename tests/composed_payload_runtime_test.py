#!/usr/bin/env python3
"""Recursive sum/array/result payload ownership and partial-failure cleanup."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run
from shared_failure_cleanup_runtime_test import SITES, expected_report

CASES = [
    (0, 0, [1, -1, 2, 3, -2, -3, 4, 5, -5, -4, 6, -6, 7, -7, 8, -8], None),
    (1, 70, [20, -20], ("ordinary", "construction")),
    (2, 70, [30, 31, -31, -30], ("ordinary", "destructor")),
    (3, 70, [40, 41, -41, -40], ("emergency", "destructor", "destructor")),
    (4, 70, [50, -50], ("emergency", "partial", "destructor")),
]

SITES["composed_payloads.gti"] = {
    "construction": "Packet<Resource>::item(Resource(20)), 1 / zero);",
    "partial": "Packet<Resource>::item(Resource(50, zero)), 1 / zero);",
    "destructor": "int checked = 1 / this.divisor;",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "composed_payloads.gti"
    example = pathlib.Path(__file__).parent.parent / "examples" / "164-payload-composition.gti"
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
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            assert (shown.returncode, shown.stdout, shown.stderr) == (0, "ready\n42\n", ""), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"composed payloads: {name} passed", flush=True)


if __name__ == "__main__":
    main()
