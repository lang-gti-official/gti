#!/usr/bin/env python3
"""Non-wrapping envelopes must not change actual modular arithmetic."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).parent / "fixtures/wrapping_context.gti"
    lines = fixture.read_text().splitlines()
    cases = [((), (0, "", ""))]
    for count, name, detail in ((1, "add_limit", "subtraction"),
                                (2, "sub_limit", "addition"),
                                (3, "mul_limit", "addition")):
        start = next(i for i, line in enumerate(lines, 1) if line.startswith(f"int {name}("))
        site = start + 1
        cases.append((("failure",) * count, (70, "", f"{fixture.name}:{site}: "
                      f"runtime error[GTI-R0001]: integer_overflow in {detail}\n")))
    # Dynamic values prevent constant folding from replacing the guarded
    # operation; successful neighbours prove the same paths remain usable.
    for count, marker, detail, success, failing in (
        (4, "runtime-unsigned-add", "addition", "", "x"),
        (6, "runtime-unsigned-subtract", "subtraction", "", "x"),
        (7, "runtime-unsigned-multiply", "multiplication", "x", "xx"),
    ):
        site = next(i for i, line in enumerate(lines, 1) if f"// {marker}" in line)
        padding = ("padding",) * (count - 1)
        cases.append(((success, *padding), (0, "2\n1\n", "")))
        cases.append(((failing, *padding), (70, "2\n1\n",
                      f"{fixture.name}:{site}: runtime error[GTI-R0001]: "
                      f"integer_overflow in {detail}\n")))
    narrow_site = next(i for i, line in enumerate(lines, 1)
                       if "// runtime-narrow-update" in line)
    padding = ("padding",) * 4
    cases.append((("x", *padding), (0, "255\n2\n1\n", "")))
    cases.append((("xx", *padding), (70, "254\n2\n1\n",
                  f"{fixture.name}:{narrow_site}: runtime error[GTI-R0006]: "
                  "numeric_conversion_out_of_range in numeric_cast\n")))
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
             for standard in ("c++20", "c++23") for level in ("O0", "O2", "O3")]
    modes += [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"])
              for level in ("O0", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1", "--",
                                "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-wrapping-context-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            built = subprocess.run([str(compiler), str(fixture), "-o", str(binary), *flags],
                                   capture_output=True, text=True, timeout=120, env=environment)
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for arguments, expected in cases:
                result = subprocess.run([str(binary), *arguments], capture_output=True,
                                        text=True, timeout=30, env=environment)
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, arguments, result)
            print(f"Wrapping context: {name} passed ({len(cases)} cases)", flush=True)


if __name__ == "__main__":
    main()
