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
    # Independent arithmetic oracle, including the side effects of the
    # iteration that exits early or fails before committing its addition.
    for count in (0, 1, 2, 3, 4, 7, 31, 64, 127, 255):
        for mode in (5, 6):
            indices = range(7, 7 + (min(count, 3) if mode == 6 else count))
            inputs = [(index, index * 17 + count) for index in indices]
            masked = 3 + sum(value & 15 for _, value in inputs)
            remainder = 5 + sum(value % 7 for _, value in inputs)
            extended = 7 + sum(value % 251 for _, value in inputs)
            choice = 13 + sum((value & 3) if index % 2 == 0 else value % 11
                              for index, value in inputs)
            selected = 11 + sum((value & 7) + 1 if index % 2 == 0
                                else value % 5 + value % 251
                                for index, value in inputs)
            cases.append((mode, count, 0,
                          f"{len(inputs)}\n{masked}\n{remainder}\n{extended}\n{choice}\n2\n{selected}\n1\n", ""))
        for mode in (7, 8, 9):
            unsigned = mode != 8
            initial = 4294967291 if unsigned else 2147483640
            maximum = 4294967295 if unsigned else 2147483647
            marker = "dynamic-unsigned-boundary" if unsigned else "dynamic-signed-boundary"
            site = next(index for index, line in enumerate(lines, 1)
                        if f"// {marker}" in line)
            total, visits, failed = initial, 0, False
            for index in range(7, 7 + count):
                visits += 1
                if mode == 9 and index == 9:
                    break
                increment = (index + count) % (3 if unsigned else 5) + (1 if unsigned else 0)
                if total + increment > maximum:
                    failed = True
                    break
                total += increment
            output = "9\n" * visits + "2\n"
            if failed:
                cases.append((mode, count, 70, output + "1\n",
                              f"{fixture.name}:{site}: runtime error[GTI-R0001]: integer_overflow in addition\n"))
            else:
                cases.append((mode, count, 0, output + f"{total}\n1\n", ""))
    for count in (0, 1, 2, 3, 4, 7, 31, 64, 127, 255):
        total = count & 7
        outputs = []
        for outer in range(4):
            total += sum((index + outer + count) % 7 + (index & 3)
                         for index in range(7, 7 + count)
                         if (index + outer) % 2 != 0)
            outputs.append(total)
        output = "".join(f"{value}\n" for value in outputs)
        cases.append((10, count, 0, output + f"{count * 4}\n2\n{total}\n1\n", ""))
        for mode in (11, 12, 13):
            unsigned = mode != 13
            total = (4294967290 + count % 5 if unsigned
                     else 2147483640 + count % 6)
            maximum = 4294967295 if unsigned else 2147483647
            marker = "initial-unsigned-boundary" if unsigned else "initial-signed-boundary"
            site = next(index for index, line in enumerate(lines, 1)
                        if f"// {marker}" in line)
            visits, failed = 0, False
            for index in range(7, 7 + count):
                visits += 1
                if mode == 12 and index == 9:
                    break
                increment = (index + count) % 3 + 1
                if total + increment > maximum:
                    failed = True
                    break
                total += increment
            output = "9\n" * visits + "2\n"
            if failed:
                cases.append((mode, count, 70, output + "1\n",
                              f"{fixture.name}:{site}: runtime error[GTI-R0001]: integer_overflow in addition\n"))
            else:
                cases.append((mode, count, 0, output + f"{total}\n1\n", ""))
    for count in (0, 1, 2, 3, 4, 7, 31, 64, 127, 255):
        finish = 16 + count % 8
        for mode in (14, 17):
            indices = range(7, min(finish, 10) if mode == 17 else finish)
            step = sum(index + (index & 3) for index in indices)
            total = count % 7
            output = ""
            for _ in range(4):
                total += step
                output += f"{total}\n"
            cases.append((mode, count, 0,
                          output + f"{len(indices) * 4}\n2\n{total}\n1\n", ""))
        for mode in (15, 16):
            maximum = 4294967295 if mode == 15 else 2147483647
            total = maximum - count % (13 if mode == 15 else 23)
            marker = "weighted-unsigned-boundary" if mode == 15 else "weighted-signed-boundary"
            site = next(index for index, line in enumerate(lines, 1)
                        if f"// {marker}" in line)
            visits = 0
            for index in range(7, finish):
                visits += 1
                if total + index > maximum:
                    break
                total += index
            else:
                raise AssertionError("weighted failure control no longer overflows")
            cases.append((mode, count, 70, "9\n" * visits + "2\n1\n",
                          f"{fixture.name}:{site}: runtime error[GTI-R0001]: integer_overflow in addition\n"))
    key_site = next(index for index, line in enumerate(lines, 1)
                    if "// ancillary-key-boundary" in line)
    for count in (0, 1, 2, 3, 4, 7, 31, 64, 127, 255):
        finish = 16 + count % 8
        for mode in (18, 19, 20, 21, 22):
            base = (finish if mode == 20 else
                    4294967295 - count % 13 if mode == 21 else
                    count & 15 if mode == 22 else count % 17)
            total = base if mode == 19 else 0
            visits, failed = 0, False
            for index in range(7, finish):
                visits += 1
                if base + index > 4294967295:
                    failed = True
                    break
                total += (base + index) % 7
            output = "9\n" * visits + "2\n"
            if failed:
                cases.append((mode, count, 70, output + "1\n",
                              f"{fixture.name}:{key_site}: runtime error[GTI-R0001]: integer_overflow in addition\n"))
            else:
                cases.append((mode, count, 0, output + f"{total}\n1\n", ""))
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
