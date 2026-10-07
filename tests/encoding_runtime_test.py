#!/usr/bin/env python3
"""Strict Base64 and hexadecimal codecs across native configurations."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: encoding_runtime_test.py <gti>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    source = repository / "tests" / "fixtures" / "stdlib_surface_encoding.gti"
    modes = [
        (f"{standard}-{level}", ["--std", standard, f"-{level}"], [])
        for standard in ("c++20", "c++23")
        for level in ("O0", "O1", "O3")
    ]
    modes += [
        (
            "no-elide",
            ["--std", "c++23", "-O0"],
            ["-fno-elide-constructors"],
        ),
        (
            "sanitized",
            ["--std", "c++23", "-O1"],
            ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"],
        ),
    ]
    environment = dict(
        os.environ,
        GTI_STDLIB_PATH=str(repository / "stdlib"),
        ASAN_OPTIONS="halt_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )

    with tempfile.TemporaryDirectory(prefix="gti-encoding-") as temporary:
        root = pathlib.Path(temporary)
        for label, flags, native_flags in modes:
            binary = root / label
            command = [str(compiler), str(source), *flags, "-o", str(binary)]
            if native_flags:
                command += ["--", *native_flags]
            built = subprocess.run(
                command,
                capture_output=True,
                text=True,
                timeout=180,
                env=environment,
                check=False,
            )
            if built.returncode != 0:
                raise AssertionError(
                    f"{label} build: exit {built.returncode}\n"
                    f"stdout={built.stdout}\nstderr={built.stderr}"
                )
            executed = subprocess.run(
                [str(binary)],
                capture_output=True,
                text=True,
                timeout=60,
                env=environment,
                check=False,
            )
            if (executed.returncode, executed.stdout, executed.stderr) != (
                0,
                "",
                "",
            ):
                raise AssertionError(
                    f"{label} execution: exit {executed.returncode}\n"
                    f"stdout={executed.stdout}\nstderr={executed.stderr}"
                )
            print(f"encoding runtime: {label} passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
