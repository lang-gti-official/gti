#!/usr/bin/env python3

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command, text=True, capture_output=True, timeout=180, check=False
    )


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: issue_146_class_value_positions_runtime_test.py "
            "<gti> <fixture-directory>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = (
        pathlib.Path(sys.argv[2]).resolve()
        / "issue_146_trivial_class_values.gti"
    )

    with tempfile.TemporaryDirectory(prefix="gti-issue-146-") as temporary:
        root = pathlib.Path(temporary)
        for mode, flags, native in native_modes():
            executable = root / mode
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
