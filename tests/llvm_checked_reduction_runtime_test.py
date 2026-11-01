#!/usr/bin/env python3
"""Native checked reductions preserve results, overflow and cleanup order."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    fixture = root / "tests/fixtures/llvm_checked_reductions.gti"
    lines = fixture.read_text().splitlines()
    cases = []
    for count in (0, 1, 2, 3, 7, 31, 64, 127, 255):
        selected = sum(index % 3 != 1 for index in range(count))
        cases.append((0, count, 0,
                      f"{count}\n{selected}\n{7 + 5 * selected}\n2\n{selected}\n1\n", ""))
        iterations = min(count, 6)
        cases.append((4, count, 0, f"2\n{5 + iterations * (count - 2)}\n1\n", ""))
    for mode, initial, step, marker, code, detail in (
        (1, 2147483640, 3, "signed-boundary", "GTI-R0001", "integer_overflow in addition"),
        (2, 4294967290, 2, "unsigned-boundary", "GTI-R0001", "integer_overflow in addition"),
        (3, 120, 3, "narrow-boundary", "GTI-R0006", "numeric_conversion_out_of_range in numeric_cast"),
    ):
        site = next(index for index, line in enumerate(lines, 1) if f"// {marker}" in line)
        for count in (0, 1, 2):
            cases.append((mode, count, 0, "9\n" * count + f"2\n{initial + step * count}\n1\n", ""))
        for count in (3, 4, 31):
            cases.append((mode, count, 70, "9\n" * 3 + "2\n1\n",
                          f"{fixture.name}:{site}: runtime error[{code}]: {detail}\n"))
    modes = [("llvm-O0", ["--backend", "llvm", "-O0"]),
             ("llvm-O2", ["--backend", "llvm", "-O2"]),
             ("llvm-O3", ["--backend", "llvm", "-O3"]),
             ("cpp-O3", ["--backend", "cpp", "--std", "c++23", "-O3"])]
    with tempfile.TemporaryDirectory(prefix="gti-checked-reductions-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            command = [str(compiler), str(fixture), "-o", str(binary), *flags]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for mode, count, status, stdout, stderr in cases:
                result = subprocess.run([str(binary), "m" * mode, "x" * count],
                                        capture_output=True, text=True, timeout=15)
                assert (result.returncode, result.stdout, result.stderr) == (status, stdout, stderr), (
                    name, mode, count, (status, stdout, stderr),
                    (result.returncode, result.stdout, result.stderr))
            print(f"Checked reductions: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
