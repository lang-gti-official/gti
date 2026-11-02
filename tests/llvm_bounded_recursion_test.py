#!/usr/bin/env python3
"""Bounded recursive entries preserve unrestricted overflow and cleanup."""
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


def expected(depth, value):
    if depth == 0:
        return value % 251
    left = value * 17 + 3
    right = value * 13 + 7
    if max(left, right) >= 2**64:
        return None
    return expected(depth - 1, left % 1000003) + expected(depth - 1, right % 1000003)


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).parent / "fixtures"
    values = (0, 1, 17, 250, 251, 1000002, 1000003, (2**64 - 1) // 17 - 1,
              (2**64 - 1) // 17, (2**64 - 1) // 17 + 1, 2**64 - 1)
    cases = [(depth, value) for depth in (0, 1, 5) for value in values]
    observations = {}
    with tempfile.TemporaryDirectory(prefix="gti-bounded-recursion-") as directory:
        native = Path(directory) / "native.o"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        "-O2", "-c", str(fixtures / "llvm_bounded_recursion_native.cpp"),
                        "-o", str(native)], check=True, capture_output=True, timeout=60)
        for backend, level in (("llvm", "-O0"), ("llvm", "-O2"),
                               ("llvm", "-O3"), ("cpp", "-O3")):
            binary = Path(directory) / (backend + level)
            command = [str(compiler), str(fixtures / "llvm_bounded_recursion.gti"),
                       "--backend", backend, level, "-o", str(binary)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            command += ["--", str(native)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for depth, value in cases:
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=dict(os.environ, GTI_RECURSION_INPUT=str(value),
                                                 GTI_RECURSION_DEPTH=str(depth)), timeout=15)
                observation = result.returncode, result.stdout, result.stderr
                answer = expected(depth, value)
                if answer is None:
                    assert observation[:2] == (70, "99\n"), (backend, level, depth, value, observation)
                    assert "integer_overflow" in result.stderr
                else:
                    assert observation == (0, f"{answer}\n99\n", ""), (backend, level, depth, value, observation)
                if level == "-O0":
                    observations[depth, value] = observation
                else:
                    assert observation == observations[depth, value], (backend, level, depth, value, observation)
            print(f"Bounded recursion: {backend} {level} passed {len(cases)} cases", flush=True)


if __name__ == "__main__":
    main()
