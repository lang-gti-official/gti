#!/usr/bin/env python3
"""Unexposed recurrence state and success-only recursive scalar publication."""

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
    source = fixtures / "optimizer_recurrence_capture.gti"
    baseline = {}
    with tempfile.TemporaryDirectory(prefix="gti-recurrence-capture-") as directory:
        native = Path(directory) / "native.o"
        subprocess.run([*shlex.split(os.environ.get("CXX", "c++")), "-std=c++17",
                        "-O2", "-c", str(fixtures / "optimizer_recurrence_capture_native.cpp"),
                        "-o", str(native)], check=True, capture_output=True, timeout=60)
        for mode, flags, _ in native_modes(levels=("O0", "O2", "O3")):
            binary = Path(directory) / mode
            command = [str(compiler), str(source), *flags, "-o", str(binary),
                       "--", str(native)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=180)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for case in range(8):
                result = subprocess.run([str(binary)], capture_output=True, text=True,
                                        env=dict(os.environ, GTI_RECURRENCE_CASE=str(case)),
                                        timeout=15)
                observed = result.returncode, result.stdout, result.stderr
                if case in (0, 4):
                    assert observed == (0, f"{214748364700 if case == 0 else 100}\n99\n", ""), (
                        mode, case, observed)
                elif case == 5:
                    assert observed == (0, "55\n" * 63 + "256\n99\n", ""), (
                        mode, case, observed)
                elif case == 6:
                    assert observed[:2] == (70, "55\n" * 7 + "99\n"), (
                        mode, case, observed)
                    assert "integer_overflow" in result.stderr, (mode, case, observed)
                elif case == 7:
                    assert observed == (0, "15\n99\n", ""), (mode, case, observed)
                else:
                    assert observed[:2] == (70, "99\n"), (mode, case, observed)
                    assert "integer_overflow" in result.stderr if case >= 2 else (
                        "conversion" in result.stderr), (mode, case, observed)
                if case not in baseline:
                    baseline[case] = observed
                else:
                    assert observed == baseline[case], (mode, case, observed,
                                                       baseline[case])
            print(f"Recurrence and scalar publication: {mode} passed 8 cases", flush=True)


if __name__ == "__main__":
    main()
