#!/usr/bin/env python3
"""Measure identical GTI wait/notify source through public LLVM and C++ backends."""

import argparse
import hashlib
import json
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
    args = parser.parse_args()
    root, compiler, output = args.root.resolve(), args.gti.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    template = (root / "tests/fixtures/llvm_atomic_wait_cost.gti").read_text()
    commands, observations = [], []

    def run(name, argv, timed=False):
        argv = [str(value) for value in argv]
        if timed:
            argv = ["/usr/bin/time", "-l" if sys.platform == "darwin" else "-v", *argv]
        start = time.monotonic()
        result = subprocess.run(argv, capture_output=True, text=True, timeout=180, cwd=root)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        commands.append({"name": name, "argv": argv, "exit": result.returncode,
                         "seconds": time.monotonic() - start})
        (output / "commands.json").write_text(json.dumps(commands, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {output / (name + '.log')}")
        return result.stdout

    run("compiler-version", [compiler, "--version"])
    for operation, label in ((0, "empty_notify_one"), (1, "empty_notify_all"), (2, "ready_acquire")):
        source = output / f"{label}.gti"
        marker = "constexpr int32_t operation = 0;"
        if template.count(marker) != 1:
            raise AssertionError("cost source lost its exact compile-time operation selector")
        source.write_text(template.replace(marker, f"constexpr int32_t operation = {operation};"))
        for backend, standard in (("cpp", "c++20"), ("cpp", "c++23"), ("llvm", None)):
            name = f"{label}-{backend}-{standard or 'native'}"
            program = output / name
            language = ["--std", standard] if standard else []
            run(f"build-{name}", [compiler, source, "--backend", backend, *language,
                                  "--execution-profile", "concurrent", "-O3", "-o", program], timed=True)
            samples = []
            for sample in range(9):
                if run(f"measure-{name}-{sample}", [program]):
                    raise AssertionError("cost program changed its no-output contract")
                samples.append(commands[-1]["seconds"] * 1e9 / 200000000)
            run(f"resources-{name}", [program], timed=True)
            observations.append({"backend": backend, "standard": standard, "operation": label,
                                 "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                                 "executable_bytes": program.stat().st_size,
                                 "median_ns": statistics.median(samples),
                                 "min_ns": min(samples), "max_ns": max(samples)})
    report = {"template_sha256": hashlib.sha256(template.encode()).hexdigest(),
              "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
              "platform": platform.platform(), "machine": platform.machine(),
              "iterations": 200000000, "samples": 9, "observations": observations,
              "timing_scope": "whole public program wall time including startup/exit, divided by iterations; no baseline subtraction",
              "allocation_scope": "source loop has no heap allocation; runtime native-domain and binary sizes are reported separately",
              "acceptance": "identical-source optimized public-backend observations; no whole-O13 acceptance"}
    (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
