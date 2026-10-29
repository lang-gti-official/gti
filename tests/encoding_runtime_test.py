#!/usr/bin/env python3
"""Strict UTF-8, Base64 and hexadecimal codecs across native configurations."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import time

from sanitizer_support import SANITIZED_BUILD_TIMEOUT


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
        (f"llvm-{level}", ["--backend", "llvm", f"-{level}"], [])
        for level in ("O0", "O1", "O2", "O3")
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
                timeout=SANITIZED_BUILD_TIMEOUT if label == "sanitized" else 180,
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
        for backend in ("llvm", "cpp"):
            binary = root / f"utf8-exhaustive-{backend}"
            exhaustive = repository / "tests/fixtures/utf8_scalar_exhaustive.gti"
            subprocess.run(
                [str(compiler), str(exhaustive), "--backend", backend, "-O3",
                 "--progress", "never", "-o", str(binary)],
                check=True, capture_output=True, text=True, timeout=180, env=environment,
            )
            started = time.monotonic()
            subprocess.run([str(binary)], check=True, capture_output=True,
                           text=True, timeout=60, env=environment)
            print(f"UTF-8 all scalar values: {backend} passed in "
                  f"{time.monotonic() - started:.3f}s; binary {binary.stat().st_size} bytes",
                  flush=True)

        rejected = subprocess.run(
            [str(compiler), str(repository / "tests/fixtures/utf8_decoder_owner_mutation_error.gti"),
             "--emit-mir", "--progress", "never"],
            capture_output=True, text=True, timeout=60, env=environment,
        )
        if (rejected.returncode != 65 or "GTI-S2017" not in rejected.stderr or
                "Retained borrow originates here" not in rejected.stderr):
            raise AssertionError(f"UTF-8 decoder must retain its input borrow: {rejected.stderr}")
        print("UTF-8 decoder owner mutation rejected", flush=True)

        example = repository / "examples/094-binary-text-encoding.gti"
        binary = root / "example"
        subprocess.run([str(compiler), str(example), "--backend", "llvm", "-O3",
                        "--progress", "never", "-o", str(binary)],
                       check=True, capture_output=True, text=True, timeout=180, env=environment)
        executed = subprocess.run([str(binary)], capture_output=True, text=True,
                                  timeout=60, env=environment)
        if (executed.returncode, executed.stdout, executed.stderr) != (0, "QQBC\n410042\n", ""):
            raise AssertionError(f"UTF-8 example failed: {executed}")
        print("UTF-8 example passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
