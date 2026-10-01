#!/usr/bin/env python3
"""Non-gating native versus scalar SIMD representation benchmark."""
from __future__ import annotations
import os
import pathlib
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    repository = pathlib.Path(__file__).resolve().parent.parent
    native = shutil.which(os.environ.get("CXX", "c++"))
    if not native:
        raise SystemExit("native C++ compiler unavailable")
    version = subprocess.check_output([native, "--version"], text=True)
    flags = (["-fno-vectorize", "-fno-slp-vectorize"] if "clang" in version.lower()
             else ["-fno-tree-vectorize", "-fno-tree-slp-vectorize"])
    environment = dict(os.environ, GTI_STDLIB_PATH=str(repository / "stdlib"))
    measurements: dict[str, list[float]] = {"native": [], "scalar": []}
    checksum = None
    print(f"Host: {platform.platform()}; {version.splitlines()[0]}")
    with tempfile.TemporaryDirectory(prefix="gti-simd-benchmark-") as temporary:
        executables = {}
        for scalar in (False, True):
            label = "scalar" if scalar else "native"
            executable = pathlib.Path(temporary) / label
            command = [str(compiler), str(repository / "tests/fixtures/simd_benchmark.gti"),
                       "-O3", "--std", "c++20", "--cxx", native,
                       "-o", str(executable), "--", *flags,
                       str(repository / "tests/fixtures/simd_benchmark_input.cpp")]
            if scalar:
                command.append("-DGTI_SIMD_SCALAR_REFERENCE=1")
            built = subprocess.run(command, env=environment, capture_output=True, text=True, timeout=180)
            if built.returncode:
                raise SystemExit(built.stdout + built.stderr)
            executables[label] = executable
        # Warm both binaries, then alternate run order to limit systematic
        # startup/cache/thermal bias. Report wall time, not a gating speed ratio.
        for executable in executables.values():
            subprocess.run([str(executable)], capture_output=True, timeout=120, check=True)
        for sample in range(8):
            labels = ("native", "scalar") if sample % 2 == 0 else ("scalar", "native")
            for label in labels:
                start = time.perf_counter()
                result = subprocess.run([str(executables[label])], capture_output=True, text=True, timeout=120, check=True)
                measurements[label].append(time.perf_counter() - start)
                if checksum is None:
                    checksum = result.stdout
                if result.stdout != checksum or result.stderr:
                    raise AssertionError("native and scalar benchmark results differ")
    medians = {label: statistics.median(times) for label, times in measurements.items()}
    for label, duration in medians.items():
        print(f"{label}: {duration:.6f}s median of eight interleaved runs; checksum {checksum.strip()}")
        if duration < 0.2:
            print("Warning: run too short for a reliable performance comparison.")
    print(f"scalar/native: {medians['scalar'] / medians['native']:.2f}x")
    print("Auto-vectorization disabled in both builds; this microbenchmark is not an application speedup guarantee.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
