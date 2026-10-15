#!/usr/bin/env python3
"""A converted return executes as a call of its selected conversion.

`return value;` with a value of another type initializes the owned result
through the implicit constructor or implicit conversion operator that
`R result = value;` would select. The fixture traces each shape in every mode:
a temporary, a whole move-only local and a by-value parameter moved into a
by-value constructor or a consuming operator, a local borrowed by a reference
constructor or a read-only operator and released at lexical cleanup, a
temporary receiver released before the function's locals, a generic instance
and a lambda. Three phases fail inside the selected conversion and release
every live value exactly once, and one compares returns folded at compile time
with the same calls at run time. The expected traces follow from the
fixture's source alone (construction prints the id, destruction its negation,
each conversion a marker), so they are independent of the lowering.
"""

import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report)
CASES = [
    (0, 0, [1, 101, 1, -1,
            2, 102, 2, -2,
            3, 103, 3, -3,
            4, 204, -4, 204,
            5, 305, 105, 5, -5,
            6, 56, 406, 16, 116, -56, -6, 16, -16,
            7, 8, 408, 18, 118, -8, -7, 18, -18,
            9, 509, -9,
            10, 110, 10, -10], None),
    (1, 70, [20, 21, -21, -20], ("ordinary", "ticket")),
    (2, 70, [30, 31, -31, -30], ("ordinary", "source")),
    (3, 70, [40, 41, -41, -40], ("ordinary", "handle")),
    (4, 0, [200, 200], None),
]

SITES["implicit_returns.gti"] = {
    "ticket": "kept(std::move(value)), ratio(20 / divisor) {}",
    "source": "implicit operator Ticket() { return Ticket(Tracked(10 / this.divisor)); }",
    "handle": "int scaled = 30 / this.divisor;",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "implicit_returns.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-implicit-returns-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"implicit-returns-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            print(f"implicit returns: {name} passed ({len(CASES)} phases)", flush=True)


if __name__ == "__main__":
    main()
