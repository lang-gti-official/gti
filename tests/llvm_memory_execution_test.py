#!/usr/bin/env python3
"""Exact memory/lifetime execution, without a generated-C++ native fallback."""

import os
from pathlib import Path
import sys
import tempfile

from backend_execution import Endpoint, execute


CASES = {
    "layout": "llvm_memory_layout.gti",
    "lifetimes": "llvm_memory_lifetimes.gti",
}


def main():
    gti, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    selected = sys.argv[3] if len(sys.argv) > 3 else None
    if selected and selected not in CASES:
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


if __name__ == "__main__":
    main()
