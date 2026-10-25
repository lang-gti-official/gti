#!/usr/bin/env python3
"""Null-owner publication and rollback use no allocation or release."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run_case(gti: Path, root: Path, work: Path, backend: str,
             optimization: str) -> None:
    output = work / f"program-{backend}-{optimization}"
    command = [str(gti),
               str(root / "tests/fixtures/llvm_callable_null_owner_rollback.gti"),
               "--backend", backend, f"-{optimization}"]
    if backend == "cpp":
        command += ["--std", "c++23"]
    command += ["-o", str(output), "--",
                str(root / "tests/fixtures/llvm_callable_null_owner_provider.cpp")]
    built = subprocess.run(command, cwd=root, env=os.environ.copy(), text=True,
                           capture_output=True, check=False, timeout=240)
    if built.returncode != 0 or not output.is_file():
        raise AssertionError(
            f"null-owner {backend} {optimization} build failed: {built}"
        )
    ran = subprocess.run([str(output)], cwd=root, env=os.environ.copy(),
                         text=True, capture_output=True, check=False,
                         timeout=15)
    if (ran.returncode != 70 or
            "null owner later initializer failed" not in ran.stderr or
            "null owner rollback trace drift" in ran.stderr or
            ran.stdout):
        raise AssertionError(
            f"null-owner {backend} {optimization} execution drift: {ran}"
        )


def main() -> None:
    gti = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    oracle_only = "--oracle-only" in sys.argv[3:]
    component_preflight = "--component-preflight" in sys.argv[3:]
    if component_preflight:
        with tempfile.TemporaryDirectory(prefix="gti-llvm-null-owner-") as raw:
            work = Path(raw)
            for level in ("O0", "O1", "O2", "O3"):
                output = work / f"preserved-{level}"
                output.write_text("preserved", encoding="utf-8")
                command = [
                    str(gti),
                    str(root /
                        "tests/fixtures/llvm_callable_null_owner_rollback.gti"),
                    "--backend", "llvm", f"-{level}", "-o", str(output),
                    "--",
                    str(root /
                        "tests/fixtures/llvm_callable_null_owner_provider.cpp"),
                ]
                rejected = subprocess.run(
                    command, cwd=root, env=os.environ.copy(), text=True,
                    capture_output=True, check=False, timeout=120
                )
                if (rejected.returncode != 65 or
                        "GTI-B0003" not in rejected.stderr or
                        output.read_text(encoding="utf-8") != "preserved"):
                    raise AssertionError(
                        f"null-owner component preflight {level}: {rejected}"
                    )
        print("null-owner combined-prerequisite boundary: LLVM O0-O3 "
              "rejection preserved artifacts", flush=True)
        return
    endpoints = [("cpp", "O3")]
    if not oracle_only:
        endpoints += [("llvm", level) for level in ("O0", "O1", "O2", "O3")]
    with tempfile.TemporaryDirectory(prefix="gti-llvm-null-owner-") as raw:
        work = Path(raw)
        for backend, optimization in endpoints:
            run_case(gti, root, work, backend, optimization)
    mode = "C++23 oracle" if oracle_only else "C++23/LLVM O0-O3"
    print(f"null-owner later-failure rollback: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
