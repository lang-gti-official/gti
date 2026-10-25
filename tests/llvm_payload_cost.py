#!/usr/bin/env python3
"""Non-gating O3 payload cost evidence, with an independent scalar oracle."""

import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess
import tempfile
import time

from backend_execution import Endpoint, execute


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("gti", type=Path)
    parser.add_argument("root", type=Path)
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    gti, root = args.gti.resolve(), args.root.resolve()
    env = {**os.environ, "NO_COLOR": "1"}
    seed, checksum = 1, 0
    for _ in range(2000000):
        seed = seed * 48271 % 2147483647
        checksum = (checksum + seed + 3 * (seed & 1)) % 2147483647
    assert (seed, checksum) == (24123260, 2111565664)
    records = []
    source = root / "tests/fixtures/llvm_payload_kernel.gti"
    with tempfile.TemporaryDirectory(prefix="gti-llvm-payload-cost-") as directory:
        for backend, standard in (("cpp", "c++23"), ("llvm", None)):
            case = Path(directory) / backend
            start = time.perf_counter()
            result = execute(gti, root, case, source,
                             Endpoint(backend, "O3", standard), env)
            elapsed = time.perf_counter() - start
            if result.error or result.observable != (0, "", ""):
                raise AssertionError(result)
            executable = case / "program"
            for _ in range(3):
                subprocess.run([executable], check=True, timeout=15, env=env)
            timings = []
            for _ in range(11):
                start = time.perf_counter_ns()
                subprocess.run([executable], check=True, timeout=15, env=env)
                timings.append((time.perf_counter_ns() - start) / 1_000_000)
            records.append({"backend": backend, "optimization": "O3",
                            "compile_and_check_seconds": elapsed,
                            "executable_bytes": executable.stat().st_size,
                            "median_process_ms": statistics.median(timings),
                            "process_ms": timings})
    report = {"schema": 1, "iterations": 2000000,
              "oracle": {"seed": 24123260, "checksum": 2111565664},
              "allocation_policy": "No source heap operation; stack-only payloads.",
              "measurement": "Whole process including startup; no speedup threshold.",
              "records": records}
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))


if __name__ == "__main__":
    main()
