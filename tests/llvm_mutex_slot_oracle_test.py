#!/usr/bin/env python3
"""Test-only native lock slot contract, not GTI mutex admission evidence."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def run(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise AssertionError(f"{command}: {result.returncode}\n"
                             f"{result.stdout[-2000:]}{result.stderr[-2000:]}")
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    root = args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="gti-mutex-slot-") as temporary:
        work = Path(temporary)
        c_object = work / "c-slot.o"
        run([args.cc, "-std=c17", "-O3", "-c",
             str(root / "tests/fixtures/llvm_mutex_slot_oracle.c"),
             "-o", str(c_object)])
        for standard in ("c++20", "c++23"):
            executable = work / standard
            run([args.cxx, f"-std={standard}", "-O3", "-pthread",
                 str(root / "tests/llvm_mutex_slot_oracle.cpp"),
                 str(c_object), "-o", str(executable)])
            print(f"{standard}: {run([str(executable)]).strip()}", flush=True)


if __name__ == "__main__":
    main()
