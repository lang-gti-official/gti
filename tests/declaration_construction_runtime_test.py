#!/usr/bin/env python3
"""Declaration construction behaves exactly like its direct initializer.

`T name = initializer;` with an initializer of another type selects the
`implicit` constructor `T name{initializer}` would. The fixture traces each declaration
beside its direct-initializer twin in every mode: construction order,
destruction order, ownership transfer of a by-value argument and the failure
cleanup of an argument the constructor had already stored. The expected traces
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
    (0, 0, [1, 2, 3, -2, -1, 103, 104, 7, -104, -103, 5, 6, 31, -6, -5, 73], None),
    (1, 70, [7, -7], ("ordinary", "failing")),
    (2, 70, [8, -8], ("ordinary", "failing")),
    (3, 70, [], ("ordinary", "ratio")),
]

SITES["declaration_construction.gti"] = {
    "failing": "kept(std::move(value)), ratio(20 / divisor) {}",
    "ratio": "implicit Ratio(int divisor) : value(30 / divisor) {}",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "declaration_construction.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-declaration-construction-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"declaration-construction-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            print(f"declaration construction: {name} passed ({len(CASES)} phases)", flush=True)


if __name__ == "__main__":
    main()
