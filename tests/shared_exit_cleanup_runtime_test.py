#!/usr/bin/env python3
"""Shared MIR normal-exit cleanup keeps every exit's exact observable behaviour.

Each phase of the fixture leaves a body through a different exit while a
different set of user-defined resources is live. Every mode must release
exactly the live resources, newest first and once each, and return the exit's
own value or error; a destructor failing during that cleanup must release the
remaining resources on the failure path and report an ordinary failure at the
destructor. The expected traces follow from the fixture's source alone
(construction prints the id, destruction its negation), so they are an oracle
independent of the lowering. The structure of the shared exits themselves is
pinned by mir_shared_failure_cleanup on the same fixture.
"""

import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report): a report is None or an ordinary
# ("ordinary", site) failure.
CASES = [
    (0, 0, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1, 2003], None),
    (1, 0, [1, -1, 1011], None),
    (2, 0, [1, -1, 2001], None),
    (3, 0, [1, 2, 100, -100, -2, -1, 2004], None),
    (4, 0, [1, 2, 105, -105, -2, -1, 1005], None),
    (5, 0, [1, 2, 105, -105, 3, -3, -2, -1, 1055], None),
    (6, 0, [1, 2, 105, -105, 3, -3, -2, -1, 2002], None),
    (7, 0, [1, 2, 105, -105, 3, 100, -100, -3, -2, -1, 2004], None),
    (8, 0, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1, 1015], None),
    (9, 0, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1, 2003], None),
    (50, 0, [10, 20, 30, -30, -20, -10, 900], None),
    (51, 0, [10, -10, 900], None),
    (52, 0, [10, 20, -20, -10, 900], None),
    (53, 0, [10, 20, 30, -30, -20, -10, 900], None),
    # A destructor fails while an exit's shared cleanup runs. The failure's
    # own cleanup releases the resources that were still live, and the
    # program reports the destructor's failure instead of the exit's value.
    (101, 70, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1], ("ordinary", "destructor")),
    (102, 70, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1], ("ordinary", "destructor")),
    (104, 70, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1], ("ordinary", "destructor")),
    (111, 70, [1, -1], ("ordinary", "destructor")),
    (121, 70, [1, -1], ("ordinary", "destructor")),
    (132, 70, [1, 2, 100, -100, -2, -1], ("ordinary", "destructor")),
    (141, 70, [1, 2, 105, -105, -2, -1], ("ordinary", "destructor")),
    (152, 70, [1, 2, 105, -105, 3, -3, -2, -1], ("ordinary", "destructor")),
    (163, 70, [1, 2, 105, -105, 3, -3, -2, -1], ("ordinary", "destructor")),
    (173, 70, [1, 2, 105, -105, 3, 100, -100, -3, -2, -1], ("ordinary", "destructor")),
    (184, 70, [1, 2, 105, -105, 3, 101, -101, -3, 4, -4, -2, -1], ("ordinary", "destructor")),
]

SITES["shared_exit_cleanup.gti"] = {
    "destructor": "int32_t cleanup_failure = 7 / zero;",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "shared_exit_cleanup.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-shared-exits-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"shared-exits-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            print(f"shared exit cleanup: {name} passed ({len(CASES)} phases)", flush=True)


if __name__ == "__main__":
    main()
