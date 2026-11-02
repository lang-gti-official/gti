#!/usr/bin/env python3
"""Private atomic old/new values, wrapping, and checked-failure cleanup."""
from pathlib import Path
import subprocess
import re
import sys
import tempfile


def expected(mode, initial, delta, count):
    maximum = (1 << 64) - 1
    value = initial if initial < 128 else maximum - (255 - initial)
    events = []
    for _ in range(count):
        old = value
        if mode in (0, 6, 7):
            value += delta
        elif mode == 1:
            value -= delta
        elif mode == 2:
            value &= delta
        elif mode == 3:
            value |= delta
        elif mode == 4:
            value ^= delta
        else:
            value = delta
        if mode == 7 and value > maximum:
            return 70, "\n".join(events + ["22", "11", ""])
        value &= maximum
        events.append(str(old))
    return 0, "\n".join(events + ["22", str(value), "11", ""])


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).resolve().parent / "fixtures/llvm_private_atomics.gti"
    cases = [(mode, initial, delta, count) for mode in range(8)
             for initial, delta in ((0, 1), (255, 1), (0, 255), (127, 128), (240, 17), (255, 0))
             for count in (0, 1, 4)]
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-private-atomics-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            command = [str(compiler), str(fixture), "-o", str(binary),
                       "--execution-profile", "concurrent", *flags]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            if name == "cpp-O3":
                # Wrapping atomic RMW is an existing LLVM-only operation.
                # Assert that boundary explicitly; C++ supplies no execution
                # oracle for this fixture. Supported C++ atomic operations are
                # covered separately by llvm_atomic_execution.
                message = "wrapping atomic read-modify-write operations require the LLVM backend"
                codes = set(re.findall(r"error\[(GTI-[A-Z][0-9]+)\]", built.stderr))
                assert built.returncode != 0 and codes == {"GTI-B0003"} and message in built.stderr, (
                    command, built.returncode, built.stdout, built.stderr)
                print("Private atomics: cpp-O3 known GTI-B0003 boundary verified", flush=True)
                continue
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
            print(f"Private atomics: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
