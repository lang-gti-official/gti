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
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitter", required=True, type=Path)
    outputs = parser.add_mutually_exclusive_group(required=True)
    outputs.add_argument("--output", type=Path)
    outputs.add_argument("--output-root", type=Path)
    parser.add_argument("--clang", default="clang++")
    parser.add_argument("--opt", default="opt")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = (args.output if args.output else args.output_root / uuid.uuid4().hex).resolve()
    assert not output.exists()
    assert args.output_root or not output.is_relative_to(root)
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
    status = "failed"
    try:
        ir = output / "simd.ll"
        run([args.emitter.resolve(), ir])
        text = ir.read_text()
        assert "fdiv <" in text and "fcmp oeq <" in text and "fcmp olt <" in text
        assert " fast " not in text and " contract " not in text and "getelementptr inbounds" not in text
        clang = shutil.which(args.clang)
        opt = shutil.which(args.opt)
        assert clang and opt
        scalar_ir = output / "simd-scalarized.ll"
        run([opt, ir, "-passes=scalarizer,dce", "-S", "-o", scalar_ir])
        scalar_text = scalar_ir.read_text()
        assert not any(f"{operation} <" in scalar_text for operation in ("fadd", "fsub", "fmul", "fdiv", "fcmp oeq", "fcmp olt"))
        oracle = root / "tests/fixtures/llvm_optimization/scalar_oracle.c"
        for optimization, selected_ir in [(str(i), ir) for i in range(4)] + [("0", scalar_ir), ("3", scalar_ir)]:
            label = f"O{optimization}" + ("-scalarized" if selected_ir == scalar_ir else "")
            library = output / f"simd-{label}.{'dylib' if platform.system() == 'Darwin' else 'so'}"
            run([clang, f"-O{optimization}", "-fno-fast-math", "-ffp-contract=off", "-fPIC",
                 "-fno-vectorize", "-fno-slp-vectorize",
                 "-dynamiclib" if platform.system() == "Darwin" else "-shared",
                 selected_ir, oracle, "-o", library])
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
        assert count == 7776, f"incomplete SIMD matrix: {count}"
        status = "passed"
        print(f"PASS: {count} strict SIMD execution cases; evidence {output}")
    finally:
        (output / "manifest.json").write_text(json.dumps({"status": status, "cases": count, "commands": commands}, indent=2) + "\n")


if __name__ == "__main__":
    main()
