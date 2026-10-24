#!/usr/bin/env python3
"""Require successful runtime cases and exact native-entry firewall outcomes."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def execute(command, expected=0, *, environment=None):
    result = subprocess.run(
        [str(part) for part in command], capture_output=True, text=True,
        timeout=60, env=environment,
    )
    if result.returncode != expected or result.stdout or result.stderr:
        raise AssertionError(
            f"{command!r}: expected {expected}, got {result.returncode}\n"
            f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
        )


def check(binary, *, environment=None):
    execute([binary], environment=environment)
    for mode in ("throws", "missing-leave"):
        execute([binary, mode], 86, environment=environment)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--cxx", type=pathlib.Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    check(args.binary.resolve())
    if args.cxx is None:
        if args.sanitizers:
            parser.error("--sanitizers requires --cxx")
        return

    root = pathlib.Path(__file__).resolve().parent.parent
    modes = [
        (f"{standard}-{optimization}", [f"-std={standard}", f"-{optimization}"])
        for standard in ("c++20", "c++23")
        for optimization in ("O0", "O1", "O3")
    ]
    if args.sanitizers:
        modes.extend([
            ("asan-ubsan", ["-std=c++23", "-O1", "-g",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]),
            ("tsan", ["-std=c++23", "-O1", "-g",
                      "-fsanitize=thread", "-fno-omit-frame-pointer"]),
        ])

    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    with tempfile.TemporaryDirectory(prefix="gti-cthread-boundary-") as temporary:
        for name, flags in modes:
            binary = pathlib.Path(temporary) / name
            execute([
                args.cxx.absolute(), *flags, "-Wall", "-Wextra", "-Werror",
                "-pthread", "-I", root / "runtime/src",
                "-I", root / "runtime/include",
                root / "runtime/src/cthread_state.cpp",
                root / "runtime/src/thread.cpp",
                root / "runtime/src/failure.cpp",
                root / "tests/runtime_cthread_tests.cpp", "-o", binary,
            ])
            check(binary, environment=environment)
            print(f"C-thread native boundary: {name} passed", flush=True)


if __name__ == "__main__":
    main()
