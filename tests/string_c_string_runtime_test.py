#!/usr/bin/env python3
"""Owned strings expose live, terminated C-string views through verified MIR."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command):
    environment = os.environ.copy()
    for option in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        previous = environment.get(option, "")
        environment[option] = (previous + ":" if previous else "") + "halt_on_error=1"
    return subprocess.run(
        list(map(str, command)),
        capture_output=True,
        text=True,
        timeout=180,
        env=environment,
    )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: string_c_string_runtime_test.py <gti>")
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "string_c_string.gti"
    native = fixtures / "string_c_string.cpp"
    modes = [
        (f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
        for standard in ("c++20", "c++23")
        for level in ("O0", "O1", "O3")
    ]
    modes.append(
        (
            "sanitized",
            ["--std", "c++23", "-O1"],
            ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
        )
    )

    with tempfile.TemporaryDirectory(prefix="gti-string-c-string-") as directory:
        root = Path(directory)
        mir = root / "string-views.mir"
        analyzed = run([compiler, source, "--emit-mir", "-o", mir])
        assert analyzed.returncode == 0, (analyzed.stdout, analyzed.stderr)
        operations = mir.read_text()
        assert "intrinsic=c-string-from-address" in operations
        assert "intrinsic=prefix-storage-c-string" not in operations
        assert "intrinsic=prefix-storage-try-c-string" not in operations
        for name, flags, native_flags in modes:
            binary = root / name
            built = run(
                [
                    compiler,
                    source,
                    *flags,
                    "--keep-cpp",
                    "-o",
                    binary,
                    "--",
                    native,
                    *native_flags,
                ]
            )
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated

            executed = run([binary])
            assert (executed.returncode, executed.stdout, executed.stderr) == (
                0,
                "",
                "",
            ), (name, executed)
            print(f"string C-string interop: {name} passed", flush=True)


if __name__ == "__main__":
    main()
