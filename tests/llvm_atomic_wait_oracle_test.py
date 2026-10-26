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
    parser.add_argument("--native-lane-emitter", type=Path,
                        help="also test real emitter lanes from verified GTI MIR; "
                             "this does not admit GTI wait bodies")
    args = parser.parse_args()
    root = args.root.resolve()
    with tempfile.TemporaryDirectory(prefix="gti-wait-oracle-") as directory:
        work = Path(directory)
        c_object = work / "c-lanes.o"
        run([args.cc, "-std=c17", "-O3", "-c",
             str(root / "tests/fixtures/llvm_atomic_wait_lane_oracle.c"),
             "-o", str(c_object)])
        lane_sources = [
            ("handwritten", root / "tests/fixtures/llvm_atomic_wait_lane_oracle.ll")]
        if args.native_lane_emitter:
            generated = work / "native-lanes.ll"
            run([str(args.native_lane_emitter.resolve()), str(root),
                 "--emit-wait-lanes", str(generated)])
            lane_sources.append(("mir-emitter", generated))
        for origin, source in lane_sources:
            for mode in ("O0", "O3"):
                ir_object = work / f"llvm-{origin}-{mode}.o"
                executable = work / f"wait-{origin}-{mode}"
                run([args.llvm_cc, "-x", "ir", f"-{mode}", "-c",
                     str(source),
                     "-o", str(ir_object)])
                run([args.cxx, "-std=c++20", "-O3", "-pthread",
                     str(root / "tests/llvm_atomic_wait_oracle.cpp"),
                     str(c_object), str(ir_object), "-o", str(executable)])
                undefined = run(["nm", "-u", str(executable)])
                if "__atomic_" in undefined or "libatomic" in undefined:
                    raise AssertionError(f"unintended atomic runtime dependency: {undefined}")
                print(f"{origin} {mode}: {run([str(executable)]).strip()}", flush=True)
                for name in ("null", "width", "alignment", "order", "bits", "notify"):
                    run([str(executable), name], death=True)
                print(f"{origin} {mode}: six malformed ABI inputs terminate", flush=True)


if __name__ == "__main__":
    main()
