#!/usr/bin/env python3
"""Execute every bounded SIMD helper shape against strict native scalar oracles.

Component evidence only until the shared Body/coverage registration is wired.
No integer-lane, public-type-name or full-language SIMD claim is made here.
"""

import argparse
import ctypes
import json
import math
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitter", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--clang", default="clang++")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    output.mkdir(parents=True)
    commands = []

    def run(argv):
        result = subprocess.run([str(x) for x in argv], cwd=root, text=True,
                                capture_output=True, timeout=180)
        log = output / f"command-{len(commands):03}.log"
        log.write_text(result.stdout + result.stderr)
        commands.append({"argv": [str(x) for x in argv], "exit": result.returncode, "log": log.name})
        assert result.returncode == 0, str(log)
        return result

    count = 0
    try:
        ir = output / "simd.ll"
        run([args.emitter.resolve(), ir])
        text = ir.read_text()
        assert "fdiv <" in text and "fcmp oeq <" in text and "fcmp olt <" in text
        assert " fast " not in text and " contract " not in text and "getelementptr inbounds" not in text
        clang = shutil.which(args.clang)
        assert clang
        oracle = root / "tests/fixtures/llvm_optimization/scalar_oracle.c"
        for optimization in ("0", "1", "2", "3"):
            library = output / f"simd-O{optimization}.{'dylib' if platform.system() == 'Darwin' else 'so'}"
            run([clang, f"-O{optimization}", "-fno-fast-math", "-ffp-contract=off", "-fPIC",
                 "-dynamiclib" if platform.system() == "Darwin" else "-shared",
                 ir, oracle, "-o", library])
            loaded = ctypes.CDLL(str(library))
            for suffix, scalar, format_code in (("f", ctypes.c_float, "f"), ("d", ctypes.c_double, "d")):
                reference = getattr(loaded, f"oracle_{suffix}")
                reference.argtypes = [ctypes.c_int, scalar, scalar]
                reference.restype = scalar
                smallest = 2 ** (-149 if suffix == "f" else -1074)
                values = [0.0, -0.0, 1.0, -1.0, math.inf, -math.inf, math.nan,
                          smallest, -smallest, 3.5, -7.25, 2 ** 60]
                for lanes in (1, 2, 3, 4, 5, 7, 8, 31, 64):
                    for operation in range(6):
                        function = getattr(loaded, f"probe_{suffix}_{lanes}_{operation}")
                        function.argtypes = [ctypes.c_void_p] * 3
                        function.restype = None
                        result_type = ctypes.c_uint8 if operation >= 4 else scalar
                        for shift in range(len(values)):
                            left = (scalar * lanes)(*[values[(i + shift) % len(values)] for i in range(lanes)])
                            right = (scalar * lanes)(*[values[(i * 3 + shift + 1) % len(values)] for i in range(lanes)])
                            result = (result_type * (lanes + 2))()
                            result[0], result[-1] = 79, 91
                            original_left, original_right = bytes(left), bytes(right)
                            function(left, right, ctypes.byref(result, ctypes.sizeof(result_type)))
                            assert result[0] == 79 and result[-1] == 91, "output boundary overwritten"
                            assert bytes(left) == original_left and bytes(right) == original_right, "input mutated"
                            for lane in range(lanes):
                                expected = reference(operation, left[lane], right[lane])
                                actual = result[lane + 1]
                                if operation >= 4:
                                    assert actual in (0, 1) and actual == expected
                                elif math.isnan(expected):
                                    assert math.isnan(actual)
                                else:
                                    assert struct.pack(format_code, actual) == struct.pack(format_code, expected), (
                                        suffix, lanes, operation, shift, lane, actual, expected)
                            count += 1
        print(f"PASS: {count} strict SIMD execution cases at O0/O1/O2/O3")
    finally:
        (output / "manifest.json").write_text(json.dumps({"cases": count, "commands": commands}, indent=2) + "\n")


if __name__ == "__main__":
    main()
