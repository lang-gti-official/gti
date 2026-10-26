#!/usr/bin/env python3
"""Measure identical GTI synchronization source through public LLVM and C++ backends."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("gti", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cpp-standard", action="append", choices=("c++20", "c++23"),
                        help="explicit comparator selection; default runs both standards")
    parser.add_argument("--workload", action="append", choices=(
        "empty_notify_one", "empty_notify_all", "ready_acquire", "mutex_uncontended"),
        help="explicit source workload selection; default runs all three atomic cases")
    args = parser.parse_args()
    root, compiler, output = args.root.resolve(), args.gti.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    template = (root / "tests/fixtures/llvm_atomic_wait_cost.gti").read_text()
    selected = args.workload or ["empty_notify_one", "empty_notify_all", "ready_acquire"]
    standards = args.cpp_standard or ["c++20", "c++23"]
    if len(selected) != len(set(selected)) or len(standards) != len(set(standards)):
        parser.error("workloads and comparator standards must be unique")
    commands, observations = [], []
    environment = dict(os.environ, GTI_STDLIB_DIR=str(root / "stdlib"))

    def run(name, argv, timed=False):
        argv = [str(value) for value in argv]
        if timed:
            argv = ["/usr/bin/time", "-l" if sys.platform == "darwin" else "-v", *argv]
        start = time.monotonic()
        result = subprocess.run(argv, capture_output=True, text=True, timeout=180,
                                cwd=root, env=environment)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        commands.append({"name": name, "argv": argv, "exit": result.returncode,
                         "seconds": time.monotonic() - start})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {output / (name + '.log')}")
        return result.stdout

    run("compiler-version", [compiler, "--version"])
    for label in selected:
        source = output / f"{label}.gti"
        if label == "mutex_uncontended":
            source.write_text((root / "tests/fixtures/llvm_mutex_cost.gti").read_text())
            iterations = 20000000
        else:
            operation = {"empty_notify_one": 0, "empty_notify_all": 1, "ready_acquire": 2}[label]
            marker = "constexpr int32_t operation = 0;"
            if template.count(marker) != 1:
                raise AssertionError("cost source lost its exact compile-time operation selector")
            source.write_text(template.replace(marker, f"constexpr int32_t operation = {operation};"))
            iterations = 200000000
        endpoints = []
        for backend, standard in (*(("cpp", standard) for standard in standards), ("llvm", None)):
            name = f"{label}-{backend}-{standard or 'native'}"
            program = output / name
            language = ["--std", standard] if standard else []
            run(f"build-{name}", [compiler, source, "--backend", backend, *language,
                                  "--execution-profile", "concurrent", "-O3", "-o", program], timed=True)
            endpoints.append({"backend": backend, "standard": standard,
                              "name": name, "program": program, "samples": []})
        for sample in range(9):
            order = endpoints if sample % 2 == 0 else list(reversed(endpoints))
            for endpoint in order:
                name, program = endpoint["name"], endpoint["program"]
                if run(f"measure-{name}-{sample}", [program]):
                    raise AssertionError("cost program changed its no-output contract")
                endpoint["samples"].append(commands[-1]["seconds"] * 1e9 / iterations)
        for endpoint in endpoints:
            name, program, samples = endpoint["name"], endpoint["program"], endpoint["samples"]
            run(f"resources-{name}", [program], timed=True)
            observations.append({"backend": endpoint["backend"], "standard": endpoint["standard"],
                                 "operation": label,
                                 "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                 "iterations": iterations,
                                 "executable_bytes": program.stat().st_size,
                                 "samples_ns": samples,
                                 "median_ns": statistics.median(samples),
                                 "min_ns": min(samples), "max_ns": max(samples)})
    report = {"template_sha256": hashlib.sha256(template.encode()).hexdigest(),
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "platform": platform.platform(), "machine": platform.machine(),
              "iterations": (20000000 if selected == ["mutex_uncontended"] else
                             200000000 if "mutex_uncontended" not in selected else None),
              "workloads": selected, "cpp_standards": standards,
              "samples": 9, "observations": observations,
              "sample_order": "all endpoints built first; fresh processes alternate endpoint order each round",
              "timing_scope": "whole public program wall time including startup/exit, divided by iterations; no baseline subtraction",
              "allocation_scope": "source loop has no heap allocation; runtime native-domain and binary sizes are reported separately",
              "acceptance": "identical-source optimized public-backend observations; no whole-O13 acceptance"}
    (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
