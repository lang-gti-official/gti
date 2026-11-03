#!/usr/bin/env python3
"""Successful bounds facts preserve the first failure, aliasing and cleanup."""
from pathlib import Path
import subprocess
import sys
import tempfile


def expected(mode, value):
    index = {0: -(1 << 31), 1: (1 << 31) - 1}.get(value, value - 10)
    output = []
    failed = False
    valid = 0 <= index < 8
    if mode == 0:
        failed = not valid
        if valid:
            output.append(index + 1)
    elif mode == 1:
        failed = True
        if valid:
            output.append(0)
    elif mode == 2:
        failed = (index % 2 == 0 and not valid) or index == (1 << 31) - 1
        if not failed:
            output.append(index + 1)
    elif mode == 3:
        failed = not valid or index == 7
        if valid:
            output.append(index)
        if valid and index != 7:
            output.append(index)
    elif mode == 4:
        failed = True
    elif mode == 5:
        failed = not 0 <= index < 6
        if not failed:
            output.append(index + 3)
    elif mode == 6:
        failed = not valid or index == 0
        if not failed:
            output.append(index + 1)
    else:
        failed = index not in (0, 1)
        if not failed:
            output.append(0)
    output.append(22)
    return (70 if failed else 0), "".join(f"{n}\n" for n in output)


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).parent / "fixtures/optimizer_bounds_postconditions.gti"
    cases = [(mode, value) for mode in range(8) for value in range(27)]
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-bounds-postconditions-") as directory:
        for backend, level in [("llvm", "-O0"), ("llvm", "-O2"),
                               ("llvm", "-O3"), ("cpp", "-O3")]:
            binary = Path(directory) / (backend + level)
            command = [str(compiler), str(source), "--backend", backend,
                       level, "-o", str(binary)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in cases:
                result = subprocess.run([str(binary), *("x" * n for n in case)],
                                        capture_output=True, text=True, timeout=15)
                status, output = expected(*case)
                assert (result.returncode, result.stdout) == (status, output), (
                    backend, level, case, result.returncode, result.stdout, result.stderr)
                if status:
                    assert "runtime error[GTI-R" in result.stderr, result.stderr
                else:
                    assert not result.stderr, result.stderr
                observed = (result.returncode, result.stdout, result.stderr)
                if level == "-O0":
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (
                        backend, level, case, observed, baseline[case])
            print(f"Bounds postconditions: {backend} {level} passed {len(cases)} cases", flush=True)


if __name__ == "__main__":
    main()
