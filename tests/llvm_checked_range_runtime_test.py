#!/usr/bin/env python3
"""Checked scan specialization preserves values, effects and first failures."""
from pathlib import Path
import subprocess
import sys
import tempfile


def expected(mode, start, stop, length, early):
    index, total, events = start, 0, []
    for _ in range(100):
        if mode == 0:
            index += 1
        events.append(str(index))
        if index >= length:
            return 70, "\n".join(events + ["22", "11", ""]), "GTI-R0007"
        total += index + 1
        if mode:
            if index == 0:
                return 70, "\n".join(events + ["22", "11", ""]), "GTI-R0001"
            total += index
            index -= 1
        if index == early or (index <= stop if mode else index == stop):
            return 0, "\n".join(events + ["22", str(total), "11", ""]), ""
    raise AssertionError("unbounded test case")


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).resolve().parent / "fixtures/llvm_checked_ranges.gti"
    cases = [(mode, start, stop, length, early)
             for mode in (0, 1) for length in (0, 1, 2, 7, 16)
             for start, stop in ((0, 0), (0, 3), (1, 0), (3, 1), (6, 6), (9, 2))
             for early in (2, 20)]
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-checked-ranges-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            command = [str(compiler), str(fixture), "-o", str(binary), *flags]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in cases:
                result = subprocess.run([str(binary), *("x" * n for n in case)],
                                        capture_output=True, text=True, timeout=15)
                status, stdout, code = expected(*case)
                assert result.returncode == status and result.stdout == stdout, (
                    name, case, (status, stdout), result.returncode, result.stdout, result.stderr)
                if code:
                    assert f"runtime error[{code}]" in result.stderr, (name, case, result.stderr)
                else:
                    assert not result.stderr, (name, case, result.stderr)
                observed = (result.returncode, result.stdout, result.stderr)
                if name == "llvm-O0":
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (name, case, observed, baseline[case])
            print(f"Checked ranges: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
