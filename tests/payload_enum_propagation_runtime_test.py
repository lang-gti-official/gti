#!/usr/bin/env python3
"""Prefix `?` on payload enums runs the selected block's functions.

The fixture traces payload enums in every mode: a temporary, named or moved
cleanup-free operand is consumed exactly once; a generic enum template
propagates through its block; an owning operand's payload moves out once and a
residual enum is returned unchanged; a fresh variant returned across fallible
cleanup is published once; an ordinary call borrows a fresh owning variant for
its duration; and a failing test destroys the operand once before the
enclosing scope. Example 163 runs as documented.
"""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, stderr fragment)
CASES = [
    (0, 0, [5, -1, 3, -3], ""),
    (1, 0, [1, 2, 3, -2, -1, -3, 3, 3, -3, -4, 5, -5, 5, 6, 1, -6], ""),
    (2, 0, [8, 9, -9, 9, -8], ""),
    (3, 70, [8, 7, -7, -8], "division_by_zero"),
]


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "payload_enum_propagation.gti"
    example = pathlib.Path(__file__).parent.parent / "examples" / "163-payload-enum-propagation.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-payload-enum-propagation-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"payload-enum-propagation-{name}"
            command = [str(compiler), str(source), *flags, "-o", str(binary), *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for phase, code, trace, failure in CASES:
                result = run([str(binary), *(["phase"] * phase)])
                actual = (result.returncode, [int(item) for item in result.stdout.split()])
                assert actual == (code, trace), (name, phase, actual, result.stderr)
                if failure:
                    assert failure in result.stderr, (name, phase, result.stderr)
                else:
                    assert result.stderr == "", (name, phase, result.stderr)
            demo = pathlib.Path(directory) / f"example-{name}"
            built = run([str(compiler), str(example), *flags, "-o", str(demo), *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            shown = run([str(demo)])
            assert (shown.returncode, shown.stdout) == (0, "ada:40\nmissing account 3\n"), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"payload enum propagation: {name} passed", flush=True)


if __name__ == "__main__":
    main()
