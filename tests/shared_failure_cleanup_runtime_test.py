#!/usr/bin/env python3
"""Shared MIR failure cleanup keeps every failure's exact observable behaviour.

Each phase fails at a different point while a different set of user-defined
resources is live. Every mode must release exactly the live resources, newest
first and once each, and report the original failure; a destructor failing
during that cleanup must report both records. The expected traces were
recorded from the compiler before cleanup sharing, so these cases are a
differential check of the shared lowering.
"""

import pathlib
import subprocess
import sys
import tempfile

from unique_class_allocation_runtime_test import run

# (phase, exit code, stdout integers, report): a report is None, an ordinary
# ("ordinary", site) failure or an ("emergency", primary, secondary) one.
LOCALS = [
        (0, 0, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 50, -50, -900, -21, -20, -6, -1, 1000], None),
        (1, 70, [1, -1], ('ordinary', 'step')),
        (2, 70, [1, 2, -2, -1], ('ordinary', 'step')),
        (3, 70, [1, 2, 3, -3, -2, -1], ('ordinary', 'step')),
        (4, 70, [1, 2, 3, -3, -2, -1], ('ordinary', 'step')),
        (5, 70, [1, 2, 3, -3, -2, -1], ('ordinary', 'step')),
        (6, 70, [1, 2, 3, -3, 10, -10, -2, -1], ('ordinary', 'step')),
        (7, 70, [1, 2, 3, -3, 10, -10, 11, -11, -2, -1], ('ordinary', 'step')),
        (8, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -6, -2, -1], ('ordinary', 'step')),
        (9, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -6, -2, -1], ('ordinary', 'step')),
        (10, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, -6, -1], ('ordinary', 'step')),
        (11, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, -6, -1], ('ordinary', 'step')),
        (12, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, -20, -6, -1], ('ordinary', 'holder')),
        (13, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, -900, -21, -20, -6, -1], ('ordinary', 'step')),
        (14, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40, -900, -21, -20, -6, -1], ('ordinary', 'step')),
        (15, 0, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40, -900, -21, -20, -6, -1, 1041], None),
        (16, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40, 50, -50, -900, -21, -20, -6, -1], ('ordinary', 'step')),
        (17, 0, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40, 50, -50, -900, -21, -20, -6, -1, 1051], None),
        (100, 70, [1, 2, -2, -1], ('emergency', 'step', 'destructor')),
        (101, 70, [1, 2, 3, -3], ('emergency', 'step', 'destructor')),
        (102, 70, [1, 2, 3, -3, -2], ('emergency', 'step', 'destructor')),
        (103, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -6], ('emergency', 'step', 'destructor')),
        (104, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2], ('emergency', 'step', 'destructor')),
        (105, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, -900, -21, -20], ('emergency', 'step', 'destructor')),
        (106, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40], ('emergency', 'step', 'destructor')),
        (107, 70, [1, 2, 3, -3, 10, -10, 11, -11, 6, -2, 20, 21, 40, -40, 50, -50], ('emergency', 'step', 'destructor')),
    ]

THREAD = [
        (0, 0, [1, 10, 11, 20, -20, 21, -21, 12, -12, -11, -10, 2, -2, -1], None),
        (1, 70, [1, 10, -10, -1], ('ordinary', 'step')),
        (2, 70, [1, 10, 11, -11, -10, -1], ('ordinary', 'step')),
        (3, 70, [1, 10, 11, 20, -20, -11, -10, -1], ('ordinary', 'step')),
        (4, 70, [1, 10, 11, 20, -20, 21, -21, -11, -10, -1], ('ordinary', 'step')),
        (5, 70, [1, 10, 11, 20, -20, 21, -21, 12, -12, -11, -10, -1], ('ordinary', 'step')),
        (6, 70, [1, 10, 11, 20, -20, 21, -21, 12, -12, -11, -10, 2, -2, -1], ('ordinary', 'step')),
    ]

SITES = {
    "shared_failure_cleanup.gti": {
        "step": "int32_t failure = 13 / zero;",
        "destructor": "int32_t cleanup_failure = 7 / zero;",
        "holder": "checked(11 / (fail_at == base ? zero : 1))",
    },
    "shared_failure_cleanup_thread.gti": {
        "step": "int32_t failure = 13 / zero;",
    },
}


def expected_report(path, report):
    if report is None:
        return ""
    lines = path.read_text().splitlines()
    line = lambda key: next(i for i, text in enumerate(lines, 1)
                            if SITES[path.name][key] in text)
    if report[0] == "ordinary":
        return (f"{path.name}:{line(report[1])}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    return (f"{path.name}:{line(report[2])}: runtime error[GTI-R0014]: "
            "failure_during_cleanup; primary [GTI-R0002] at "
            f"{path.name}:{line(report[1])}; secondary [GTI-R0002]\n")


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    programs = [
        (fixtures / "shared_failure_cleanup.gti", [], LOCALS),
        (fixtures / "shared_failure_cleanup_thread.gti",
         ["--execution-profile", "concurrent"], THREAD),
    ]
    with tempfile.TemporaryDirectory(prefix="gti-shared-cleanup-") as directory:
        for source, profile, cases in programs:
            mir = pathlib.Path(directory) / (source.stem + ".mir")
            emitted = run([str(compiler), str(source), *profile, "-O0",
                           "--emit-mir", "-o", str(mir)])
            assert emitted.returncode == 0, (emitted.stdout, emitted.stderr)
            assert " kind=continuation " in mir.read_text(), (
                f"{source.name} no longer shares failure cleanup")
            for name, flags, native in modes:
                binary = pathlib.Path(directory) / f"{source.stem}-{name}"
                command = [str(compiler), str(source), *profile, *flags,
                           "-o", str(binary), *native]
                built = run(command)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                for phase, code, trace, report in cases:
                    result = run([str(binary), *(["phase"] * phase)])
                    actual = (result.returncode, [int(item) for item in result.stdout.split()],
                              result.stderr)
                    wanted = (code, trace, expected_report(source, report))
                    assert actual == wanted, (source.name, name, phase, actual, wanted)
                print(f"shared failure cleanup: {source.name} {name} passed "
                      f"({len(cases)} phases)", flush=True)


if __name__ == "__main__":
    main()
