#!/usr/bin/env python3
"""Exact memory/lifetime execution, without a generated-C++ native fallback."""

import os
from pathlib import Path
import sys
import subprocess
import tempfile

from backend_execution import Endpoint, execute


CASES = {
    "layout": "llvm_memory_layout.gti",
    "zero-and-passive": "llvm_memory_zero_and_passive.gti",
    "vacant": "llvm_memory_vacant.gti",
    "guards": "llvm_memory_guards.gti",
    "lifetimes": "llvm_memory_lifetimes.gti",
    "value-lifetimes": "llvm_memory_value_lifetimes.gti",
    "provider": "llvm_memory_provider.gti",
}
REJECT = {"vacant-move": "llvm_memory_reject_vacant_move.gti"}
FAILURES = {"guard-cleanup": ("llvm_memory_guard_cleanup.gti", "GTI-R0014",
                              "Boolean-guard cleanup trace observed")}


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in CASES and selected not in REJECT and selected not in FAILURES:
        raise ValueError(f"unknown memory case: {selected}")
    endpoints = (Endpoint("cpp", "O3", "c++20"), Endpoint("cpp", "O3", "c++23"),
                 *(Endpoint("llvm", level) for level in ("O0", "O1", "O2", "O3")))
    env = {**os.environ, "NO_COLOR": "1"}
    with tempfile.TemporaryDirectory(prefix="gti-llvm-memory-") as directory:
        work = Path(directory)
        for name, filename in CASES.items():
            if selected and selected != name:
                continue
            source = root / "tests/fixtures" / filename
            observed = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}", source, endpoint, env)
                if result.error or result.observable != (0, "", ""):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"memory observable drift: {name}: {observed}")
            print(f"{name}: C++20/23O3 and native LLVM O0-O3 passed", flush=True)
        for name, (filename, code, marker) in FAILURES.items():
            if selected and selected != name:
                continue
            observed = []
            for endpoint in endpoints:
                result = execute(gti, root, work / f"{name}-{endpoint}",
                                 root / "tests/fixtures" / filename, endpoint, env)
                if (result.error or result.returncode != 70 or code not in result.stderr
                        or marker not in result.stderr or "wrong Boolean-guard" in result.stderr):
                    raise AssertionError(f"{name} {endpoint}: {result}")
                observed.append(result.observable)
            if any(value != observed[0] for value in observed):
                raise AssertionError(f"memory failure attribution drift: {name}: {observed}")
            print(f"{name}: source-attributed failure and exactly-once cleanup passed", flush=True)
        for name, filename in REJECT.items():
            if selected and selected != name:
                continue
            for backend in ("cpp", "llvm"):
                output = work / f"rejected-{name}-{backend}"
                output.write_text("preserved", encoding="utf-8")
                result = subprocess.run(
                    [str(gti), str(root / "tests/fixtures" / filename), "--backend", backend,
                     "-O3", "-o", str(output)], cwd=root, env=env,
                    capture_output=True, text=True, timeout=60)
                if (result.returncode != 65 or "error[GTI-S" not in result.stderr
                        or "GTI-B" in result.stderr or output.read_text() != "preserved"):
                    raise AssertionError(f"{name} {backend}: {result}")
            print(f"{name}: source rejection and artifact preservation passed", flush=True)


if __name__ == "__main__":
    main()
