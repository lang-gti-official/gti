#!/usr/bin/env python3
"""Propagation-only frames retain checks and ancestor cleanup failure identity."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main():
    compiler = Path(sys.argv[1]).resolve()
    source = Path(__file__).parent / "fixtures/propagation_only_context.gti"
    site = next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                if "12 / divisor" in line)
    ordinary = (f"{source.name}:{site}: runtime error[GTI-R0002]: "
                "division_by_zero in integer_division\n")
    emergency = (f"{source.name}:{site}: runtime error[GTI-R0014]: "
                 "failure_during_cleanup; primary [GTI-R0002] at "
                 f"{source.name}:{site}; secondary [GTI-R0002]\n")
    cases = [((), (0, "2\n1\n", "")),
             (("primary",), (70, "2\n1\n", ordinary)),
             (("primary", "secondary"), (70, "2\n", emergency))]
    modes = [(f"{standard}-{level}", ["--std", standard, f"-{level}"])
             for standard in ("c++20", "c++23") for level in ("O0", "O2", "O3")]
    modes.append(("sanitized", ["--std", "c++23", "-O1", "--",
                                "-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer"]))
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-propagation-context-") as directory:
        for name, flags in modes:
            binary = Path(directory) / name
            built = subprocess.run([str(compiler), str(source), "-o", str(binary), *flags],
                                   capture_output=True, text=True, timeout=120,
                                   env=environment)
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for arguments, expected in cases:
                result = subprocess.run([str(binary), *arguments], capture_output=True,
                                        text=True, timeout=30, env=environment)
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, arguments, result)
            print(f"Propagation context: {name} passed", flush=True)


if __name__ == "__main__":
    main()
