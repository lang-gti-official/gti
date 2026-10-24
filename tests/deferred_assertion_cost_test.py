#!/usr/bin/env python3
"""Compare source assertions and result macros to explicit GTI control flow.

The macro variant includes scalar and passive class local-result publication,
a prepared value argument, a fresh lambda capture, and unevaluated
written-expression metadata in the measured loop.
Correctness gates are complete-state parity and zero observed allocations/cold
message calls. Timings are reported, not gated on noisy shared test hosts.
"""

import argparse
import json
import os
import pathlib
import resource
import statistics
import subprocess
import sys
import tempfile
import time

from native_fixture_build import build_fixture


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--measure", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[1]
    source = root / "tests/fixtures/deferred_assertion_cost.gti"
    provider = root / "tests/fixtures/stdlib_adoption_allocation_counts.cpp"
    env = dict(os.environ, GTI_STDLIB_PATH=str(root / "stdlib"))
    report = {}
    output = None
    with tempfile.TemporaryDirectory(prefix="gti-assertion-cost-") as directory:
        binaries = {}
        samples = {}
        for disabled in (False, True):
            for manual in (False, True):
                key = f"{'manual' if manual else 'macro'}-NDEBUG={disabled}"
                binary = pathlib.Path(directory) / key
                command = [str(args.compiler.resolve()), str(source), "-O3"]
                for flag, active in (("NDEBUG", disabled), ("COST_MANUAL", manual)):
                    if active:
                        command += ["-D", flag]
                started = time.perf_counter()
                result = build_fixture(command, binary, native_sources=[provider],
                                       optimization="O3", capture_output=True,
                                       text=True, env=env, timeout=180)
                assert result.returncode == 0, (key, result.stdout, result.stderr)
                report[key] = {
                    "build_seconds": round(time.perf_counter() - started, 6),
                    "native_bytes": binary.stat().st_size,
                    "generated_cpp_bytes": binary.with_suffix(".gti.cpp").stat().st_size,
                }
                binaries[key] = binary
                samples[key] = []
        # Interleave variants and reverse their order on alternating rounds.
        for round_number in range(11 if args.measure else 3):
            keys = list(binaries)
            if round_number % 2:
                keys.reverse()
            for key in keys:
                started = time.perf_counter()
                result = subprocess.run([str(binaries[key])], capture_output=True,
                                        text=True, env=env, timeout=20)
                elapsed = time.perf_counter() - started
                assert result.returncode == 0 and not result.stderr, (key, result)
                if output is None:
                    output = result.stdout
                assert result.stdout == output and output.strip().isdigit(), (key, result)
                if round_number >= 2:
                    samples[key].append(elapsed)
        for key, values in samples.items():
            report[key]["runtime_median_seconds"] = round(statistics.median(values), 6)
            report[key]["runtime_samples_seconds"] = [round(value, 6) for value in values]
        peak = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        print(json.dumps({"variants": report, "complete_state": output.strip(),
                          "allocations_each": 0, "cold_message_calls_each": 0,
                          "cumulative_child_peak_kib": peak / 1024 if sys.platform == "darwin" else peak},
                         indent=2))


if __name__ == "__main__":
    main()
