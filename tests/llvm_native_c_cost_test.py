#!/usr/bin/env python3
"""C boundary cost probe; --measure requires an uncontended host window."""

import argparse
import json
from pathlib import Path
import platform
import re
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--measure", action="store_true")
    family = parser.add_mutually_exclusive_group()
    family.add_argument("--indirect", action="store_true")
    family.add_argument("--aggregate", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stem = ("aggregate_cost" if args.aggregate else
            "indirect_cost" if args.indirect else "direct_cost")
    report = {"head": subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "cc": args.cc,
        "family": ("passive-aggregate" if args.aggregate else
                   "scalar-indirect" if args.indirect else "direct"),
        "calls_per_process": (40000003 if args.aggregate else
                              40000002 if args.indirect else 40000001),
        "measured": args.measure,
        "measurement": "whole process including startup; same C oracle and checked additions",
        "records": {}, "steps": []}

    def save():
        (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(argv, name, measured_build=False):
        command = [str(part) for part in argv]
        metrics = output / (name + ".metrics")
        if measured_build:
            flags = ["-l"] if platform.system() == "Darwin" else ["-v"]
            command = ["/usr/bin/time", *flags, "-o", str(metrics), *command]
        started = time.perf_counter()
        result = subprocess.run(command, capture_output=True, timeout=180)
        elapsed = time.perf_counter() - started
        log = output / (name + ".log")
        log.write_bytes(result.stdout + result.stderr)
        step = {"argv": command, "exit": result.returncode,
                "seconds": elapsed, "log": str(log)}
        if measured_build:
            raw = metrics.read_text()
            pattern = (r"(\d+)\s+maximum resident set size" if platform.system() == "Darwin"
                       else r"Maximum resident set size \(kbytes\):\s*(\d+)")
            match = re.search(pattern, raw)
            if not match:
                raise RuntimeError(f"missing compiler peak memory: {metrics}")
            step["peak_rss_kib"] = int(match[1]) / (1024 if platform.system() == "Darwin" else 1)
            step["metrics"] = str(metrics)
        report["steps"].append(step)
        save()
        if result.returncode:
            raise RuntimeError(f"{name}: exit {result.returncode}; {log}")
        return step

    run([args.cc, "--version"], "c-version")
    host = output / "host.o"
    run([args.cc, "-std=c17", "-O3", "-Wall", "-Wextra", "-Werror",
         "-I", root / "runtime/include", "-c",
         fixture / ("aggregate_cost.c" if args.aggregate else "direct_cost.c"), "-o", host], "host")
    native, baseline = output / "native", output / "baseline"
    run([args.compiler.resolve(), fixture / (stem + ".gti"), "--backend", "llvm",
         "-O3", "-o", native, "--", host], "build-native", measured_build=True)
    run([args.cc, "-std=c17", "-O3", "-I", root / "runtime/include",
         fixture / (stem + "_baseline.c"), host, "-o", baseline],
        "build-baseline", measured_build=True)
    obj, ir = output / "native.o", output / "native.ll"
    run([args.emitter.resolve(), fixture / (stem + ".gti"), obj, ir, 3,
         "--source-preflight"], "inspect-generated")
    allocation_calls = [line.strip() for line in ir.read_text().splitlines()
                        if re.search(r"\b(call|invoke)\b", line) and
                        re.search(r"@(malloc|calloc|realloc|aligned_alloc|_Zn[aw])", line)]
    if allocation_calls:
        raise RuntimeError("direct call kernel unexpectedly allocates")
    report["allocation_evidence"] = {
        "generated_module_allocation_calls": allocation_calls,
        "scope": "generated GTI module only; runtime startup allocations not measured",
        "native_object_bytes": obj.stat().st_size,
    }
    for name, executable in (("native", native), ("baseline", baseline)):
        step = run([executable], "correctness-" + name)
        if (output / ("correctness-" + name + ".log")).read_bytes():
            raise RuntimeError("cost kernel produced unexpected output")
        report["records"][name] = {"executable_bytes": executable.stat().st_size,
                                    "correctness_seconds": step["seconds"]}
    if args.measure:
        samples = {name: [] for name in report["records"]}
        for sample in range(9):
            order = (("native", native), ("baseline", baseline))
            if sample % 2:
                order = tuple(reversed(order))
            for name, executable in order:
                step = run([executable], f"timing-{name}-{sample}")
                samples[name].append(step["seconds"])
        for name, times in samples.items():
            report["records"][name].update(samples_seconds=times,
                                            median_seconds=statistics.median(times))
        ratio = (report["records"]["native"]["median_seconds"] /
                 report["records"]["baseline"]["median_seconds"])
        report["native_to_checked_c_ratio"] = ratio
        report["material_regression_threshold"] = 1.25
        save()
        if ratio > 1.25:
            raise RuntimeError(f"material direct-call runtime cost requires investigation: {ratio:.3f}")
    save()
    print(f"PASS: {report['family']} C cost {'measurement' if args.measure else 'correctness only'}; {output / 'cost.json'}")


if __name__ == "__main__":
    main()
