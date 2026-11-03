#!/usr/bin/env python3
"""Exact named move-only returns transfer ownership and clean up once."""

import os
import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def run(command, environment=None):
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        timeout=120,
        env=environment,
        check=False,
    )


def require_success(process, label):
    if process.returncode != 0:
        raise AssertionError(
            f"{label}: exit {process.returncode}\n"
            f"stdout={process.stdout}\nstderr={process.stderr}"
        )


def main():
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: implicit_return_move_runtime_test.py <gti> <fixture>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    environment = dict(
        os.environ,
        ASAN_OPTIONS="halt_on_error=1",
        UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
    )
    modes = native_modes()

    with tempfile.TemporaryDirectory(prefix="gti-implicit-return-move-") as temp:
        root = pathlib.Path(temp)
        for label, flags, native in modes:
            binary = root / label
            command = [
                str(compiler),
                str(source),
                *flags,
                *keep_cpp(flags),
                "-o",
                str(binary),
            ]
            if native:
                command += ["--", *native]
            built = run(command, environment)
            require_success(built, f"{label} build")

            if is_cpp_mode(flags):
                generated = binary.with_suffix(".gti.cpp").read_text(encoding="utf8")
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError(f"{label}: missing verified-MIR witness")

            executed = run([str(binary)], environment)
            require_success(executed, f"{label} execution")
            if executed.stdout or executed.stderr:
                raise AssertionError(
                    f"{label}: unexpected output "
                    f"stdout={executed.stdout!r} stderr={executed.stderr!r}"
                )
            print(f"implicit return move: {label} passed", flush=True)


if __name__ == "__main__":
    main()
