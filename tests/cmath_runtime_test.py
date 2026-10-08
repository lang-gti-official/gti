#!/usr/bin/env python3
"""Exercise every shipped binary32 math operation through verified MIR."""

import pathlib
import subprocess
import sys
import tempfile

from native_symbol_spelling import native_spelling


def run(command):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if result.returncode:
        raise AssertionError(
            f"{command!r}: exit {result.returncode}\n{result.stdout}{result.stderr}"
        )
    return result


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    root = pathlib.Path(__file__).resolve().parent.parent
    source = root / "tests/fixtures/stdlib_surface_cmath.gti"
    with tempfile.TemporaryDirectory(prefix="gti-cmath-") as directory:
        output = pathlib.Path(directory)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                binary = output / f"math-{standard}-{optimization}"
                run([str(compiler), str(source), f"-{optimization}",
                     "--std", standard, "--keep-cpp", "-o", str(binary)])
                result = run([str(binary)])
                if result.stdout or result.stderr:
                    raise AssertionError(f"unexpected output: {result!r}")
                generated = binary.with_suffix(".gti.cpp").read_text()
                for name in ("floor", "ceil", "round", "trunc", "sqrt", "cbrt",
                             "pow", "fmod", "exp", "log", "log10", "sin", "cos",
                             "tan", "asin", "acos", "atan", "atan2"):
                    symbol = native_spelling(generated, f"gti_rt_math_{name}_f32_v1")
                    if f"{symbol}(" not in generated:
                        raise AssertionError(f"missing scalar ABI call for {name}")
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing verified-MIR emission witness")
        # The same GTI fixture also exercises the generated/native boundary
        # with address and undefined-behavior instrumentation.
        binary = output / "math-sanitized"
        run([str(compiler), str(source), "-O1", "--std", "c++23",
             "-o", str(binary), "--", "-fsanitize=address,undefined",
             "-fno-omit-frame-pointer"])
        result = run([str(binary)])
        if result.stdout or result.stderr:
            raise AssertionError(f"unexpected sanitizer output: {result!r}")


if __name__ == "__main__":
    main()
