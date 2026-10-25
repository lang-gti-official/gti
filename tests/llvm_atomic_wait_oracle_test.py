#!/usr/bin/env python3
"""Test-only wait ABI proposal, not generated GTI execution/admission evidence."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def run(argv: list[str], *, death: bool = False) -> str:
    result = subprocess.run(argv, capture_output=True, text=True, timeout=30)
    if (result.returncode >= 0 if death else result.returncode != 0):
        raise AssertionError(f"{argv}: exit {result.returncode}\n"
                             f"{result.stdout[-2000:]}{result.stderr[-2000:]}")
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cc", default="clang")
    args = parser.parse_args()
    root = args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="gti-wait-oracle-") as directory:
        work = Path(directory)
        c_object = work / "c-lanes.o"
        run([args.cc, "-std=c17", "-O3", "-c",
             str(root / "tests/fixtures/llvm_atomic_wait_lane_oracle.c"),
             "-o", str(c_object)])
        for mode in ("O0", "O3"):
            ir_object, executable = work / f"llvm-{mode}.o", work / f"wait-{mode}"
            run([args.llvm_cc, "-x", "ir", f"-{mode}", "-c",
                 str(root / "tests/fixtures/llvm_atomic_wait_lane_oracle.ll"),
                 "-o", str(ir_object)])
            run([args.cxx, "-std=c++20", "-O3", "-pthread",
                 str(root / "tests/llvm_atomic_wait_oracle.cpp"),
                 str(c_object), str(ir_object), "-o", str(executable)])
            undefined = run(["nm", "-u", str(executable)])
            if "__atomic_" in undefined or "libatomic" in undefined:
                raise AssertionError(f"unintended atomic runtime dependency: {undefined}")
            print(f"{mode}: {run([str(executable)]).strip()}", flush=True)
            for name in ("null", "width", "alignment", "order", "bits", "notify"):
                run([str(executable), name], death=True)
            print(f"{mode}: six malformed ABI inputs terminate", flush=True)


if __name__ == "__main__":
    main()
