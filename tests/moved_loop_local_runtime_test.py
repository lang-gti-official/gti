#!/usr/bin/env python3
"""Moved loop-local regression: a local declared inside a loop body and
consumed with std::move must compile and rerun its declaration correctly when
the back edge returns to it, at -O0 and -O3."""

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(
        f"{label}: exit={process.returncode} stdout={process.stdout!r} "
        f"stderr={process.stderr!r}\n"
    )
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(
            "usage: moved_loop_local_runtime_test.py <gti> <fixture.gti>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-moved-loop-local-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O3"):
            executable = root / optimization
            built = run(
                [
                    str(compiler),
                    str(fixture),
                    f"-{optimization}",
                    "-o",
                    str(executable),
                ]
            )
            if built.returncode != 0:
                return fail(f"build {optimization}", built)
            executed = run([str(executable)])
            if (
                executed.returncode != 0
                or executed.stdout != ""
                or executed.stderr != ""
            ):
                return fail(f"run {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
