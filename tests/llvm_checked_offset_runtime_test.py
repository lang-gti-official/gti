#!/usr/bin/env python3
"""Relative-index arithmetic preserves unsigned failures and cleanup."""
from pathlib import Path
import subprocess
import sys
import tempfile


def expected(mode, low, high):
    events = ["7"]
    maximum = (1 << 64) - 1
    if mode == 4:
        low, high = maximum - low, maximum - high
    else:
        low, high = low * (maximum // 255), high * (maximum // 255)
    if high < low:
        return 70, "\n".join(events + ["22", "11", ""])
    distance = high - low
    offset = distance // 2
    if mode == 1:
        offset = distance // 8 * 2
    elif mode in (2, 4):
        offset += distance // 8
    elif mode == 3:
        offset = distance + distance // 2
    if offset > maximum:
        return 70, "\n".join(events + ["22", "11", ""])
    events.append("8")
    if low + offset > maximum:
        return 70, "\n".join(events + ["22", "11", ""])
    return 0, "\n".join(events + ["22", str(low + offset), "11", ""])


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).resolve().parent / "fixtures/llvm_checked_offsets.gti"
    endpoints = (0, 1, 2, 7, 8, 16, 127, 128, 240, 254, 255)
    cases = [(mode, low, high) for mode in range(5)
             for low in endpoints for high in endpoints]
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-checked-offsets-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            command = [str(compiler), str(fixture), "-o", str(binary), *flags]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in cases:
                result = subprocess.run([str(binary), *("x" * n for n in case)],
                                        capture_output=True, text=True, timeout=15)
                status, stdout = expected(*case)
                assert (result.returncode, result.stdout) == (status, stdout), (
                    name, case, (status, stdout), result.returncode, result.stdout, result.stderr)
                if status:
                    assert "runtime error[GTI-R0001]" in result.stderr, (name, case, result.stderr)
                else:
                    assert not result.stderr, (name, case, result.stderr)
                observed = (result.returncode, result.stdout, result.stderr)
                if name == "llvm-O0":
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (name, case, observed, baseline[case])
            print(f"Checked offsets: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
