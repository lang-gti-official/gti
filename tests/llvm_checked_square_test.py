#!/usr/bin/env python3
"""Successful square bounds preserve multiplication/addition failures and cleanup."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from backend_modes import native_modes


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
        for mode, flags, _ in native_modes(levels=("O0", "O2", "O3")):
            binary = Path(directory) / mode
            command = [str(compiler), str(fixtures / "llvm_checked_square.gti"),
                       *flags, "-o", str(binary), "--", str(native)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in range(23):
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=dict(os.environ, GTI_SQUARE_CASE=str(case)),
                                        timeout=15)
                observed = result.returncode, result.stdout, result.stderr
                if case in expected:
                    assert observed == (0, f"{expected[case]}\n99\n", ""), (
                        mode, case, observed)
                else:
                    assert observed[:2] == (70, "99\n"), (mode, case, observed)
                    assert "integer_overflow" in result.stderr, (mode, case, observed)
                if case not in baseline:
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (mode, case, observed,
                                                       baseline[case])
            print(f"Checked square: {mode} passed 23 cases", flush=True)


if __name__ == "__main__":
    main()
