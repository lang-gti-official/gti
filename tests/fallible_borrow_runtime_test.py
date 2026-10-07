#!/usr/bin/env python3

"""Build and run the checked-borrow Expected carrier across native modes."""

import pathlib
import subprocess
import sys
import tempfile


def run(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, text=True, capture_output=True, check=False)


def fail(label: str, process: subprocess.CompletedProcess[str]) -> int:
    sys.stderr.write(f"{label} failed with exit {process.returncode}\n")
    sys.stderr.write(process.stdout)
    sys.stderr.write(process.stderr)
    return 1


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: fallible_borrow_runtime_test.py <gti> <fixture>")

    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-fallible-borrow-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O3"):
                label = f"{standard}-{optimization}"
                executable = root / label
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
                    return fail(f"fallible-borrow build ({label})", built)
                executed = run([str(executable)])
                if (
                    executed.returncode != 0
                    or executed.stdout != ""
                    or executed.stderr != ""
                ):
                    return fail(f"fallible-borrow runtime ({label})", executed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
