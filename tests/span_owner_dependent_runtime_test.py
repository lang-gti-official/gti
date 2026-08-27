#!/usr/bin/env python3

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
            "usage: span_owner_dependent_runtime_test.py <gti> <fixture>"
        )

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-owner-span-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                executable = root / f"owner-span-{standard}-{optimization}"
                built = run(
                    [
                        str(compiler),
                        str(fixture),
                        "--std",
                        standard,
                        f"-{optimization}",
                        "-o",
                        str(executable),
                    ]
                )
                if built.returncode != 0:
                    return fail(f"build {standard} {optimization}", built)

                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"run {standard} {optimization}", executed)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
