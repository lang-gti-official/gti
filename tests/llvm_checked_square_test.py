#!/usr/bin/env python3
"""Successful square bounds preserve multiplication/addition failures and cleanup."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    expected = {
        0: 46340**2 + 7, 1: 46340**2 + 7,
        4: 3 * 26754**2 + 7, 5: 3 * 26754**2 + 7,
        8: 3037000499**2 + 7, 9: 3037000499**2 + 7,
        12: 4294967295**2 + 7, 15: -1431655758,
        17: 65535**2 + 7, 19: 7, 20: 8, 21: 10, 22: 7,
    }
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-checked-square-") as directory:
        native = Path(directory) / "native.o"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        "-O2", "-c", str(fixtures / "llvm_checked_square_native.cpp"),
                        "-o", str(native)], check=True, capture_output=True, timeout=60)
        for backend, level in [("llvm", "-O0"), ("llvm", "-O2"),
                               ("llvm", "-O3"), ("cpp", "-O3")]:
            binary = Path(directory) / (backend + level)
            command = [str(compiler), str(fixtures / "llvm_checked_square.gti"),
                       "--backend", backend, level, "-o", str(binary)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            command += ["--", str(native)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for mode in range(23):
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=dict(os.environ, GTI_SQUARE_CASE=str(mode)),
                                        timeout=15)
                observed = result.returncode, result.stdout, result.stderr
                if mode in expected:
                    assert observed == (0, f"{expected[mode]}\n99\n", ""), (
                        backend, level, mode, observed)
                else:
                    assert observed[:2] == (70, "99\n"), (backend, level, mode, observed)
                    assert "integer_overflow" in result.stderr, (backend, level, mode, observed)
                if level == "-O0":
                    baseline[mode] = observed
                else:
                    assert observed == baseline[mode], (backend, level, mode, observed,
                                                       baseline[mode])
            print(f"Checked square: {backend} {level} passed 23 cases", flush=True)


if __name__ == "__main__":
    main()
