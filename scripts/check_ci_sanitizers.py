#!/usr/bin/env python3
"""Fail before a long build when the runner cannot execute sanitizer binaries."""

import argparse
import pathlib
import subprocess
import tempfile


def run(command, timeout=60):
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0:
        raise RuntimeError(
            f"{command!r}: exit {result.returncode}\n"
            f"{result.stdout}{result.stderr}"
        )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitizer", action="append",
                        choices=("thread", "address,undefined"), required=True)
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parent.parent / "tests/fixtures/sanitizer_host_probe.cpp"
    try:
        with tempfile.TemporaryDirectory(prefix="gti-sanitizer-host-") as directory:
            for sanitizer in args.sanitizer:
                binary = str(pathlib.Path(directory) / "probe")
                # A contended hosted runner has taken over 60 seconds to compile
                # this one file (full validation 41); the probe must fail fast on
                # a broken runner, not on a slow one. Executions keep 60 seconds.
                run([args.cxx, "-std=c++20", "-O1", "-pthread",
                     f"-fsanitize={sanitizer}", "-fno-omit-frame-pointer",
                     str(source), "-o", binary], timeout=600)
                for _ in range(3):
                    run([binary])
                print(f"Runner sanitizer execution passed: {sanitizer}", flush=True)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"Runner sanitizer preflight failed:\n{error}\n"
                    "This probe contains no GTI code. Fix the runner's compiler/runtime, "
                    "address-space limits, or container sandbox before retrying. "
                    "See .github/README.md. Sanitizer tests have NOT been skipped.\n")


if __name__ == "__main__":
    main()
