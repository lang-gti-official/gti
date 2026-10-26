#!/usr/bin/env python3
"""Same-contract context/dispatch cost, with optional quiet-host measurement."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--measure", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/embedding"
    args.output_dir.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix="cost-", dir=args.output_dir.resolve()))
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "contract": "2000001 uint64 XOR exports with identical context entry/leave and output publication",
              "timing_acceptance": args.measure, "steps": []}

    def run(command, tag):
        command = [str(item) for item in command]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180)
        (output / (tag + ".log")).write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "exit": result.returncode, "seconds": time.monotonic() - started})
        (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{tag}: {result.returncode}; {output / (tag + '.log')}")
        return result.stdout.decode()

    stem = output / "native"
    run([args.emitter.resolve(), fixture / "infallible.gti", stem, "3", args.cxx,
         args.runtime.resolve(), "mix=embed_mix"], "generate")
    suffix = ".dylib" if sys.platform == "darwin" else ".so"
    native = output / "native-generation" / ("libmodule" + suffix)
    baseline = output / ("baseline" + suffix)
    run([args.cxx, "-O3", "-std=c++20", "-fPIC", "-dynamiclib" if sys.platform == "darwin" else "-shared",
         "-I", root / "runtime/include", "-I", root / "runtime/src", fixture / "cost_baseline.cpp",
         args.runtime.resolve(), "-pthread", "-o", baseline], "baseline")
    host = output / "host"
    run([args.cc, "-std=c17", "-D_POSIX_C_SOURCE=200809L", "-O3", "-I", root / "runtime/include",
         fixture / "cost_host.c", *([] if sys.platform == "darwin" else ["-ldl"]), "-o", host], "host")
    samples = {"native": [], "baseline": []}
    for sample in range(9 if args.measure else 1):
        for name, library in (("native", native), ("baseline", baseline))[::1 if sample % 2 == 0 else -1]:
            samples[name].append(float(run([host, library], f"{name}-{sample}")))
    symbols = run(["nm", "-u", native], "undefined")
    allocation_symbols = [line for line in symbols.splitlines()
                          if any(token in line for token in ("malloc", "calloc", "realloc", "_Znwm", "_Znam"))]
    if allocation_symbols:
        raise RuntimeError(f"unexpected module allocation dependency: {allocation_symbols}")
    ratio = statistics.median(samples["native"]) / statistics.median(samples["baseline"])
    report.update({"samples_seconds": samples, "ratio": ratio, "native_bytes": native.stat().st_size,
                   "baseline_bytes": baseline.stat().st_size, "module_allocation_symbols": allocation_symbols,
                   "allocation_limit": "loader/startup allocations excluded; no module allocation imports"})
    (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
    if args.measure and ratio > 1.25:
        raise RuntimeError(f"embedding cost regression: {ratio}")
    print(output / "cost.json")


if __name__ == "__main__":
    main()
