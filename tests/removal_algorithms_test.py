#!/usr/bin/env python3
"""Removal over a large vector allocates nothing (native allocation counts).

The lifecycle fixture, predicate failure and semantic rejections are tests/ui
cases (tests/ui/stdlib/algorithm/removal/); this one links a native
allocation counter.
"""
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repo = pathlib.Path(__file__).resolve().parent.parent
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repo / "stdlib"))
    with tempfile.TemporaryDirectory(prefix="gti-removal-") as temporary:
        binary = pathlib.Path(temporary) / "cost"
        result = subprocess.run(
            [str(compiler), str(repo / "tests/fixtures/removal_algorithms_cost.gti"),
             "--backend", "llvm", "-O3", "-o", str(binary), "--",
             str(repo / "tests/fixtures/stdlib_adoption_allocation_counts.cpp")],
            env=environment, text=True, capture_output=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        start = time.monotonic()
        result = subprocess.run([str(binary)], env=environment, text=True,
                                capture_output=True, timeout=240)
        assert result.returncode == 0, result.stdout + result.stderr
        print(json.dumps({"cost_seconds": time.monotonic() - start,
                          "binary_bytes": binary.stat().st_size,
                          "elements": 65536, "iterations": 16,
                          "removal_allocations": 0}))


if __name__ == "__main__":
    main()
