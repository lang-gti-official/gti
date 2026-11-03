#!/usr/bin/env python3

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


EXPECTED = "named 42 7\n/hello/world.txt\nnamed\n"


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=300, check=False
    )


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: string_owned_output_runtime_test.py <gti> <fixture.gti>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-string-owned-output-") as scratch:
        root = pathlib.Path(scratch)
        for mode, flags, native in native_modes():
            executable = root / f"string-output-{mode}"
            built = run(
                [
                    str(compiler),
                    str(source),
                    *flags,
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                sys.stderr.write(built.stdout)
                sys.stderr.write(built.stderr)
                return 1

            executed = run([str(executable)])
            if executed.returncode != 0 or executed.stdout != EXPECTED:
                sys.stderr.write(
                    "owned-string output overload failed for "
                    f"{mode}: exit={executed.returncode} "
                    f"stdout={executed.stdout!r} stderr={executed.stderr!r}\n"
                )
                return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
