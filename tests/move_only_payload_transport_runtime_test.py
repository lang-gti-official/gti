#!/usr/bin/env python3
"""A move-only payload enum without cleanup moves wherever it is transferred.

A payload enum whose alternative holds a move-only class with no destructor
needs no cleanup but cannot be copied. The fixture passes such an enum by
value to free functions, a method, a constructor, a lambda and a generic
function, returns its payload from a consuming arm (the reported shape),
returns the enum from a function that can fail, observes a named enum, a
parameter and a field in place, and moves the enum through a fixed array, a
vector and a loop and its payload class through std::experimental::optional.
Tokens trace construction (id) and consumption (id + 1000), so each payload
appears once in each role. The same shapes over an owning enum trace
construction (id) and destruction (-id), so each payload is destroyed exactly
once. Phases 1 and 2 fail inside a consuming arm reached through a by-value
argument, for the cleanup-free and the owning enum; every live owner is
released once.
"""

import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report)
CASES = [
    (0, 0, [1, 1001, 2, 1002, 0, 1000, 3, 2001, 1003, 4, 2001, 5, 2005, 6,
            2001, 1006, 7, 1007, 8, 1008, 9, 1009, 10, 1, 11, 2001, 1011, 12,
            1012, 13, 1013, 25, 1025, 21, 1021, 22, 2001, -22, 1022, 23, -23,
            2001, 24, -24, 1024, -21], None),
    (1, 70, [50, 51, -50], ("ordinary", "slot")),
    (2, 70, [60, 61, -61, -60], ("ordinary", "parcel")),
]

SITES["move_only_payload_transport.gti"] = {
    "slot": "return item.read() / divisor;",
    "parcel": "return item.read() / by;",
}


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "move_only_payload_transport.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-move-only-payload-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"move-only-payload-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, report in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()],
                          result.stderr)
                wanted = (code, trace, expected_report(source, report))
                assert actual == wanted, (name, phase, actual, wanted)
            print(f"move-only payload transport: {name} passed", flush=True)


if __name__ == "__main__":
    main()
