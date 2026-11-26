#!/usr/bin/env python3
"""Cross-language comparison of the voxel world kernels: GTI vs C++.

Builds the GTI headless benchmark (`gti build voxel-bench --release`) and the
C++ reference port (`clang++ -O3 -std=c++20 -ffp-contract=off`), runs each
binary K times per thread count, verifies that every run produces the same
checksum, prints a markdown table (median over runs) and writes
bench/results/<date>-compare.json.

-ffp-contract=off is required for bit-identical worlds: clang contracts a*b+c
into FMA by default, while GTI evaluates each binary32 operation with its own
rounding. Without it the C++ terrain (and checksum) differs.

Usage: python3 bench/compare.py [--runs K] [--seed N] [--radius R]
                                [--threads 1,2,4,8] [--no-build]
"""

import argparse
import datetime
import json
import os
import platform
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
GTI_BIN = os.path.join(PROJECT, "build", "gti", "release", "arm64-apple-macos", "voxel-bench")
CPP_SRC = os.path.join(HERE, "reference", "voxel_bench.cpp")
CPP_BIN = os.path.join(PROJECT, "build", "reference", "voxel_bench_cpp")
CPP_FLAGS = ["-O3", "-std=c++20", "-ffp-contract=off"]

PHASES = [("generate", "Generate"), ("mesh_plain", "Mesh (culled)"), ("mesh_greedy", "Mesh (greedy)")]


def run(cmd, cwd=None):
    proc = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout)
        raise SystemExit(f"command failed ({proc.returncode}): {' '.join(cmd)}")
    return proc.stdout


def gti_binary_path():
    # The release artifact lands under build/gti/release/<target>/.
    base = os.path.join(PROJECT, "build", "gti", "release")
    if os.path.isfile(GTI_BIN):
        return GTI_BIN
    for target in sorted(os.listdir(base)) if os.path.isdir(base) else []:
        candidate = os.path.join(base, target, "voxel-bench")
        if os.path.isfile(candidate):
            return candidate
    raise SystemExit("GTI voxel-bench binary not found; build it first")


def build():
    print("building GTI voxel-bench (--release)...", flush=True)
    run(["gti", "build", "voxel-bench", "--release"], cwd=PROJECT)
    print("building C++ reference (clang++ " + " ".join(CPP_FLAGS) + ")...", flush=True)
    os.makedirs(os.path.dirname(CPP_BIN), exist_ok=True)
    run(["clang++", *CPP_FLAGS, CPP_SRC, "-o", CPP_BIN])


def bench_once(binary, seed, radius, threads):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "out.json")
        run([binary, "--seed", str(seed), "--radius", str(radius), "--threads", str(threads),
             "--json", path])
        with open(path) as f:
            return json.load(f)


def us(ns):
    return ns / 1000.0


def summarize(runs):
    """Median over process runs of each phase metric."""
    out = {}
    for key, _ in PHASES:
        out[key] = {
            metric: statistics.median(r[key][metric] for r in runs)
            for metric in ("total_ns", "min_ns", "median_ns", "p95_ns", "max_ns")
        }
        out[key]["count"] = runs[0][key]["count"]
    out["quads"] = runs[0]["quads"]
    out["checksum"] = runs[0]["checksum"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5, help="process runs per configuration (K)")
    ap.add_argument("--seed", type=int, default=1337)
    ap.add_argument("--radius", type=int, default=8)
    ap.add_argument("--threads", default="1,2,4,8", help="comma-separated generation thread counts")
    ap.add_argument("--no-build", action="store_true")
    args = ap.parse_args()
    thread_counts = [int(t) for t in args.threads.split(",")]

    if not args.no_build:
        build()
    binaries = {"gti": gti_binary_path(), "cpp": CPP_BIN}

    raw = {lang: {} for lang in binaries}
    checksums = set()
    for threads in thread_counts:
        for i in range(args.runs):
            # Interleave languages so machine-load drift affects both alike.
            for lang, binary in binaries.items():
                result = bench_once(binary, args.seed, args.radius, threads)
                raw[lang].setdefault(threads, []).append(result)
                checksums.add(result["checksum"])
    if len(checksums) != 1:
        raise SystemExit(f"CHECKSUM MISMATCH: {sorted(checksums)}")
    checksum = checksums.pop()

    summary = {lang: {t: summarize(runs) for t, runs in per.items()} for lang, per in raw.items()}
    base = thread_counts[0]
    g = summary["gti"][base]
    c = summary["cpp"][base]
    side = 2 * args.radius + 1

    lines = []
    lines.append(f"seed {args.seed}, radius {args.radius} ({side * side} chunks generated, "
                 f"{(side - 2) ** 2} meshed), {args.runs} runs, median; checksum `{checksum}` (identical)")
    lines.append("")
    lines.append(f"| Phase ({base} thread) | GTI µs/chunk (median) | C++ µs/chunk (median) | GTI/C++ | "
                 "GTI total ms | C++ total ms | GTI p95 µs | C++ p95 µs |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|")
    for key, label in PHASES:
        gm, cm = us(g[key]["median_ns"]), us(c[key]["median_ns"])
        lines.append(f"| {label} | {gm:.1f} | {cm:.1f} | {gm / cm:.2f}x | "
                     f"{g[key]['total_ns'] / 1e6:.1f} | {c[key]['total_ns'] / 1e6:.1f} | "
                     f"{us(g[key]['p95_ns']):.1f} | {us(c[key]['p95_ns']):.1f} |")
    q = g["quads"]
    lines.append("")
    lines.append(f"Quads: culled {q['plain_opaque']} opaque + {q['plain_translucent']} translucent; "
                 f"greedy {q['greedy_opaque']} + {q['greedy_translucent']}")
    if len(thread_counts) > 1:
        lines.append("")
        lines.append("| Generation threads | GTI total ms | GTI chunks/s | GTI speedup | C++ total ms | "
                     "C++ chunks/s | C++ speedup |")
        lines.append("|---:|---:|---:|---:|---:|---:|---:|")
        g1 = summary["gti"][base]["generate"]["total_ns"]
        c1 = summary["cpp"][base]["generate"]["total_ns"]
        for t in thread_counts:
            gt = summary["gti"][t]["generate"]["total_ns"]
            ct = summary["cpp"][t]["generate"]["total_ns"]
            n = side * side
            lines.append(f"| {t} | {gt / 1e6:.1f} | {n / (gt / 1e9):.0f} | {g1 / gt:.2f}x | "
                         f"{ct / 1e6:.1f} | {n / (ct / 1e9):.0f} | {c1 / ct:.2f}x |")
    table = "\n".join(lines)
    print()
    print(table)

    gti_version = run(["gti", "--version"]).strip()
    clang_version = run(["clang++", "--version"]).splitlines()[0]
    date = datetime.date.today().isoformat()
    os.makedirs(os.path.join(HERE, "results"), exist_ok=True)
    out_path = os.path.join(HERE, "results", f"{date}-compare.json")
    with open(out_path, "w") as f:
        json.dump({
            "date": date,
            "machine": platform.platform(),
            "gti_version": gti_version,
            "cpp_compiler": clang_version,
            "cpp_flags": CPP_FLAGS,
            "seed": args.seed,
            "radius": args.radius,
            "runs": args.runs,
            "checksum": checksum,
            "summary": {lang: {str(t): s for t, s in per.items()} for lang, per in summary.items()},
            "raw": {lang: {str(t): r for t, r in per.items()} for lang, per in raw.items()},
            "markdown": table,
        }, f, indent=2)
    print(f"\nwrote {out_path}")


if __name__ == "__main__":
    main()
