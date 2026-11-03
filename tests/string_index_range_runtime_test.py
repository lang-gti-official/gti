#!/usr/bin/env python3
"""Indexed string copies, checked endpoints, and failure cleanup through MIR."""

import pathlib
import re
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def check_execution(binary):
    for arguments in range(8):
        result = run([str(binary), *(["invalid"] * arguments)])
        if arguments == 0:
            valid = (result.returncode, result.stdout, result.stderr) == (0, "", "")
        else:
            valid = (
                result.returncode == 70
                and result.stdout == "99\n"
                and re.fullmatch(
                    r"<std/string>:\d+: runtime error\[GTI-R0007\]: "
                    r"index_out_of_bounds in string\n",
                    result.stderr,
                ) is not None
            )
        if not valid:
            raise AssertionError(
                f"{binary.name}, {arguments} arguments: exit={result.returncode}, "
                f"stdout={result.stdout!r}, stderr={result.stderr!r}"
            )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: string_index_range_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/string_index_range.gti"
    with tempfile.TemporaryDirectory(prefix="gti-string-index-range-") as temporary:
        root = pathlib.Path(temporary)
        modes = native_modes()
        for name, flags, native in modes:
            binary = root / name
            command = [
                str(compiler), str(source), *flags,
                *keep_cpp(flags), "-o", str(binary),
            ]
            if name == "sanitized":
                command.extend([
                    "--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                ])
            built = run(command)
            if built.returncode != 0:
                raise AssertionError(f"{name} build failed:\n{built.stdout}{built.stderr}")
            if is_cpp_mode(flags):
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing verified-MIR emission witness")
            check_execution(binary)
            print(f"string index range: {name} passed", flush=True)
        for optimization in ("O0", "O3"):
            binary = root / f"llvm-{optimization}"
            built = run([str(compiler), str(source), "--backend", "llvm",
                         f"-{optimization}", "-o", str(binary)])
            if built.returncode != 0:
                raise AssertionError(f"LLVM {optimization} build failed:\n{built.stdout}{built.stderr}")
            check_execution(binary)
            print(f"string index range: llvm-{optimization} passed", flush=True)


if __name__ == "__main__":
    main()
