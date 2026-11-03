#!/usr/bin/env python3
"""Unexposed recurrence state and success-only recursive scalar publication."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    source = fixtures / "optimizer_recurrence_capture.gti"
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-recurrence-capture-") as directory:
        native = Path(directory) / "native.o"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        "-O2", "-c", str(fixtures / "optimizer_recurrence_capture_native.cpp"),
                        "-o", str(native)], check=True, capture_output=True, timeout=60)
        for backend, level in [("llvm", "-O0"), ("llvm", "-O2"),
                               ("llvm", "-O3"), ("cpp", "-O3")]:
            binary = Path(directory) / (backend + level)
            command = [str(compiler), str(source), "--backend", backend, level,
                       "-o", str(binary)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            command += ["--", str(native)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for mode in range(8):
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=dict(os.environ, GTI_RECURRENCE_CASE=str(mode)),
                                        timeout=15)
                observed = result.returncode, result.stdout, result.stderr
                if mode in (0, 4):
                    assert observed == (0, f"{214748364700 if mode == 0 else 100}\n99\n", ""), (
                        backend, level, mode, observed)
                elif mode == 5:
                    assert observed == (0, "55\n" * 63 + "256\n99\n", ""), (
                        backend, level, mode, observed)
                elif mode == 6:
                    assert observed[:2] == (70, "55\n" * 7 + "99\n"), (
                        backend, level, mode, observed)
                    assert "integer_overflow" in result.stderr, (backend, level, mode, observed)
                elif mode == 7:
                    assert observed == (0, "15\n99\n", ""), (backend, level, mode, observed)
                else:
                    assert observed[:2] == (70, "99\n"), (backend, level, mode, observed)
                    assert "integer_overflow" in result.stderr if mode >= 2 else (
                        "conversion" in result.stderr), (backend, level, mode, observed)
                if level == "-O0":
                    baseline[mode] = observed
                else:
                    assert observed == baseline[mode], (backend, level, mode, observed,
                                                       baseline[mode])
            print(f"Recurrence and scalar publication: {backend} {level} passed 8 cases", flush=True)


if __name__ == "__main__":
    main()
