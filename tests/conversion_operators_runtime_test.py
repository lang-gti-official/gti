#!/usr/bin/env python3
"""A conversion executes as a call of its selected conversion operator.

`Target(value)`, and a declaration through an `implicit` operator, call the
operator with the converted value as the receiver. The fixture traces each
conversion in every mode: the fresh result's construction and destruction, a
temporary receiver released at the end of its full expression (a field
default's before the constructor body), a consuming receiver whose state moves
out exactly once, a mutable receiver's update, and the failure cleanup of a
receiver when the operator fails. The expected traces
follow from the fixture's source alone (construction prints the id,
destruction its negation), so they are independent of the lowering.
"""

import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report)
CASES = [
    (0, 0, [1, 101, 101, 203, -101, -101, -1,
            2, 102, -2, 102, -102,
            3, 4, 7, -4, -3,
            6, 106, -6, 107, 106, -106,
            28], None),
    (1, 70, [7, -7], ("ordinary", "failing")),
    (2, 70, [8, -8], ("ordinary", "handle")),
    (3, 70, [], ("ordinary", "ratio")),
    (4, 30, [], None),
]

SITES["conversion_operators.gti"] = {
    "failing": "implicit operator Tracked() { return Tracked(10 / this.divisor); }",
    "handle": "int scaled = 20 / this.divisor;",
    "ratio": "operator int() { return 30 / this.divisor; }",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "conversion_operators.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-conversion-operators-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"conversion-operators-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            print(f"conversion operators: {name} passed ({len(CASES)} phases)", flush=True)


if __name__ == "__main__":
    main()
