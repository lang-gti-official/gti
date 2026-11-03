#!/usr/bin/env python3
"""Unsafe caller contracts preserve normal calls and MIR failure cleanup."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unsafe_function_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/unsafe_function.gti"
    example = pathlib.Path(__file__).resolve().parent.parent / "examples/117-unsafe-member-contracts.gti"
    failure_line = next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                        if "return numerator / divisor;" in line)
    with tempfile.TemporaryDirectory(prefix="gti-unsafe-functions-") as directory:
        for mode, flags, native in native_modes():
            binary = pathlib.Path(directory) / f"unsafe-{mode}"
            command = [str(compiler), str(source), *flags,
                       *keep_cpp(flags), "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            if is_cpp_mode(flags):
                cpp = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in cpp
            for arguments in ([], ["fail"]):
                result = subprocess.run([str(binary), *arguments], capture_output=True,
                                        text=True, timeout=30)
                stderr = (f"unsafe_function.gti:{failure_line}: runtime error[GTI-R0002]: "
                          "division_by_zero in integer_division\n") if arguments else ""
                assert (result.returncode, result.stdout, result.stderr) == (
                    70 if arguments else 0, "cleanup\ncleanup\n", stderr
                ), (mode, arguments, result)
            print(f"unsafe function: {mode} success/failure passed", flush=True)
            example_binary = binary.with_name(binary.name + "-members")
            example_command = [str(compiler), str(example), *flags,
                               "-o", str(example_binary)]
            built = subprocess.run(example_command, capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (example_command, built.stdout, built.stderr)
            result = subprocess.run([str(example_binary)], capture_output=True, text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), result
            print(f"unsafe members example: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
