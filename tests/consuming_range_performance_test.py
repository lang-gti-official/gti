#!/usr/bin/env python3
"""Record optimized ownership-transfer costs; allocation and cleanup are asserted in GTI."""
import argparse
import json
import pathlib
import resource
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parent.parent
    with tempfile.TemporaryDirectory(prefix="gti-consuming-cost-") as folder:
        binary = pathlib.Path(folder) / "cost"
        start = time.perf_counter()
        built = subprocess.run([str(args.compiler.resolve()),
            str(root / "tests/fixtures/consuming_range_cost.gti"),
            "--backend", "llvm", "-O3", "-o", str(binary), "--",
            str(root / "tests/fixtures/process_resource_policy_native.cpp")],
            capture_output=True, text=True, timeout=120)
        assert built.returncode == 0, built
        result = {"compile_seconds": time.perf_counter() - start,
                  "compile_child_maxrss_native_units": resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss,
                  "binary_bytes": binary.stat().st_size,
                  "elements_per_sample": 2048 * 65536,
                  "allocations_per_vector": 1,
                  "releases_per_vector": 1}
        samples = {"borrowed": [], "consuming": []}
        # Alternate ordering and discard one warmup for each mode. Both paths
        # fill, sum, and destroy the same number of noncopyable elements.
        for repetition in range(8):
            modes = ("borrowed", "consuming") if repetition % 2 else ("consuming", "borrowed")
            for mode in modes:
                arguments = ["large", "workload"] + (["borrowed"] if mode == "borrowed" else [])
                start = time.perf_counter()
                run = subprocess.run([str(binary), *arguments], capture_output=True, text=True, timeout=30)
                duration = time.perf_counter() - start
                assert (run.returncode, run.stdout, run.stderr) == (0, "", ""), run
                if repetition:
                    samples[mode].append(duration)
        result["samples_seconds"] = samples
        result["median_seconds"] = {mode: statistics.median(times) for mode, times in samples.items()}
        result["consuming_to_borrowed_ratio"] = result["median_seconds"]["consuming"] / result["median_seconds"]["borrowed"]
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
