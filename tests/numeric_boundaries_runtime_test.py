#!/usr/bin/env python3
"""Independent numeric boundary oracles and cleanup-preserving failures."""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command):
    environment = os.environ.copy()
    for variable in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        previous = environment.get(variable, "")
        environment[variable] = (previous + ":" if previous else "") + "halt_on_error=1"
    return subprocess.run(list(map(str, command)), capture_output=True, text=True,
                          timeout=scaled(120), env=environment)


def built(command):
    result = run(command)
    assert result.returncode == 0, (command, result.stdout, result.stderr)


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "numeric_boundaries.gti"
    failure = fixtures / "numeric_boundaries_failure.gti"
    oracle = fixtures / "numeric_boundaries_oracle.cpp"
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-numeric-boundaries-") as directory:
        root = Path(directory)
        for mode, flags, native_flags in modes:
            executable = root / mode
            built([compiler, source, *flags, "--keep-cpp", "-o", executable,
                   "--", oracle, *native_flags])
            result = run([executable])
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (mode, result)
            generated = executable.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated
            # Public arithmetic stays source-defined, not a backend std:: shim.
            for operation in ("gcd", "lcm", "midpoint"):
                assert re.search(rf"__gti_inst_[0-9a-f]+_{operation}__gti_mir_failure\(", generated), operation
                assert f"::std::{operation}(" not in generated, operation
            failing = root / f"{mode}-failure"
            built([compiler, failure, *flags, "-o", failing, "--", *native_flags])
            for argc in range(2, 16):
                result = run([failing, *(["x"] * (argc - 1))])
                assert result.returncode == 70 and result.stdout == "cleaned\n", (mode, argc, result)
                narrowing = argc in (2, 3, 6, 7, 10, 11)
                error = ("GTI-R0006]: numeric_conversion_out_of_range in numeric_cast"
                         if narrowing else "GTI-R0001]: integer_overflow in ")
                assert error in result.stderr, (mode, argc, result.stderr)
                assert re.fullmatch(r"<std/numeric>:\d+: runtime error\[GTI-R000[16]\]: [a-z_]+ in [a-z_]+\n",
                                    result.stderr), result.stderr
            print(f"numeric boundaries: {mode} passed", flush=True)

        # Generic restrictions still reject non-integral gcd/lcm and bool
        # midpoint; explicit generic calls have the same contract as inference.
        for index, expression in enumerate(("std::gcd(1.0, 2.0)",
                                             "std::lcm<double>(1.0d, 2.0d)",
                                             "std::midpoint<bool>(true, false)")):
            invalid = root / f"invalid-{index}.gti"
            invalid.write_text(f"#include <std/numeric>\nint main() {{ {expression}; return 0; }}\n")
            artifact = root / f"invalid-{index}.cpp"
            result = run([compiler, invalid, "--emit-cpp", "-o", artifact])
            assert result.returncode == 65 and "error[GTI-S" in result.stderr, result
            assert "GTI-B0001" not in result.stderr and not artifact.exists(), result


if __name__ == "__main__":
    main()
