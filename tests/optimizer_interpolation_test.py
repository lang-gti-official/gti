#!/usr/bin/env python3
"""Bounded interpolation keeps prior failures, signed division and aliasing."""
from itertools import product
from pathlib import Path
import subprocess
import sys
import tempfile


MINIMUM = -(1 << 31)
MAXIMUM = (1 << 31) - 1
ENDPOINTS = [MINIMUM, MINIMUM + 1, -8, -1, 0, 1, 8, MAXIMUM]
DIVISORS = [-2, -1, 0, 1, 2, 3, 7, MAXIMUM]


def expected(first, last, divisor, mode):
    delta = last - first
    if not MINIMUM <= delta <= MAXIMUM:
        return 70, "22\n", "integer_overflow"
    if divisor == 0:
        return 70, "22\n", "division_by_zero"
    quotient = abs(delta) // abs(divisor)
    if (delta < 0) != (divisor < 0):
        quotient = -quotient
    if not MINIMUM <= quotient <= MAXIMUM:
        return 70, "22\n", "integer_overflow"
    result = (last if mode == 2 else first) + quotient
    if not MINIMUM <= result <= MAXIMUM:
        return 70, "22\n", "integer_overflow"
    return 0, f"{result}\n22\n", ""


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).parent / "fixtures/optimizer_interpolation.gti"
    cases = list(product(range(8), range(8), range(8), range(3)))
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-interpolation-") as directory:
        for backend, level in [("llvm", "-O0"), ("llvm", "-O2"),
                               ("llvm", "-O3"), ("cpp", "-O3")]:
            binary = Path(directory) / (backend + level)
            command = [str(compiler), str(fixture), "--backend", backend,
                       level, "-o", str(binary)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in cases:
                a, b, k, mode = case
                status, output, failure = expected(ENDPOINTS[a], ENDPOINTS[b],
                                                   DIVISORS[k], mode)
                result = subprocess.run([str(binary), *("x" * n for n in case)],
                                        capture_output=True, text=True, timeout=15)
                observed = result.returncode, result.stdout, result.stderr
                assert observed[:2] == (status, output), (backend, level, case, observed)
                assert failure in result.stderr if failure else not result.stderr, (
                    backend, level, case, observed)
                if level == "-O0":
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (backend, level, case, observed,
                                                       baseline[case])
            print(f"Interpolation: {backend} {level} passed {len(cases)} cases", flush=True)


if __name__ == "__main__":
    main()
