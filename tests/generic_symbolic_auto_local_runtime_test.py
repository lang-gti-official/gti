#!/usr/bin/env python3
"""Exercise instance-owned types for symbolic generic auto locals."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) not in (3, 4) or (
        len(sys.argv) == 4 and sys.argv[3] != "--include-o1"
    ):
        raise SystemExit(
            "usage: generic_symbolic_auto_local_runtime_test.py <gti> <source> [--include-o1]"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    del sys.argv[3:]  # the former extended-level selection; the policy owns the levels
    with tempfile.TemporaryDirectory(
        prefix="gti-generic-symbolic-auto-"
    ) as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, _native in native_modes():
            executable = root / mode
            built = run([str(compiler), str(source), *flags, "-o", str(executable)])
            if built.returncode != 0 or "GTI-B0001" in built.stderr:
                return fail(f"build {mode}", built)

            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != ""
                or executed.stderr != ""
            ):
                return fail(f"run {mode}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
