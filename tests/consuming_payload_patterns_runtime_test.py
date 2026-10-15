#!/usr/bin/env python3
"""A switch over a consumed payload enum moves the payload into its bindings.

The fixture traces construction (id) and destruction (-id) of user-defined
owners in every mode. Phase 0 covers prvalue and std::move subjects, tag-only
and default arms (which drop the consumed enum at their entry), multi-field
and mixed alternatives, a class payload holding its own owners, a generic enum
consumed by a source-defined optional's `&&` accessor and by a generic
function, continue/break/return out of an arm, and a move-only payload without
cleanup. Phases 1-3 fail inside a binding arm, while a tag-only arm drops its
enum, and while the subject's other temporaries end; every live owner is
released exactly once. Example 158 runs as documented.
"""

import pathlib
import sys
import tempfile

from shared_failure_cleanup_runtime_test import SITES, expected_report
from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report)
CASES = [
    (0, 0, [1, 1001, -1, 1, 2, -2, 1002, 3, 4, 34, -4, -3, 1003, 5, 30, -5,
            7, -7, 1007, 8, 9, -9, -8, 17, 10, 1010, 2010, -10, 11, 12, -12,
            1011, 13, 1013, 14, -13, -11, 21, -21, 22, -22, 23, -23, 44, 30,
            31, -31, 1030, -30, 42], None),
    (1, 70, [50, 51, -51, -50], ("ordinary", "divide")),
    (2, 70, [60, 61, -61, -60], ("ordinary", "fragile")),
    (3, 70, [70, 71, 72, -71, -72, -70], ("ordinary", "fragile")),
]

SITES["consuming_payload_patterns.gti"] = {
    "divide": "return left.read() / divisor;",
    "fragile": "int32_t checked = 100 / this.divisor;",
}

EXAMPLE_OUTPUT = "hello\nmode\nfast\nready\n3 lines, 1 ignored\n"


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    tests = pathlib.Path(__file__).parent
    source = tests / "fixtures" / "consuming_payload_patterns.gti"
    example = tests.parent / "examples" / "158-consuming-matches.gti"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-consuming-patterns-") as directory:
        for name, flags, native in modes:
            binary = pathlib.Path(directory) / f"consuming-patterns-{name}"
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
            assert (shown.returncode, shown.stdout) == (0, EXAMPLE_OUTPUT), \
                (name, shown.returncode, shown.stdout, shown.stderr)
            print(f"consuming payload patterns: {name} passed", flush=True)


if __name__ == "__main__":
    main()
