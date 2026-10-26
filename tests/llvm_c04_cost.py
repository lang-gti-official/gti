#!/usr/bin/env python3
"""Measure validated MIR transport artifacts; not production acceptance."""

import argparse
import hashlib
import json
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path


FIXTURES = ("scalar", "borrowed_cleanup", "owned_parameter", "return_cleanup",
            "replacement_cleanup")
MODES = ("status", "unwind")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def checksum(outcome, iterations):
    # The input cycles over 0..1023; mixed calls fail at odd indices. The
    # versioned integer-overflow code contributes 1 for each failed invocation.
    cycle = [1 if outcome == "failure" or (outcome == "mixed" and i % 2)
             else i + 1 for i in range(1024)]
    cycles, remainder = divmod(iterations, len(cycle))
    return cycles * sum(cycle) + sum(cycle[:remainder])


def verify_artifacts(evidence, directory, fixtures):
    if evidence.get("sanitized_native_carrier") is not False:
        raise ValueError("cost requires an unsanitized carrier/runtime evaluation")
    boundaries = evidence.get("call_boundaries", "fixed")
    if boundaries not in ("fixed", "optimizable"):
        raise ValueError("unknown call-boundary policy")
    selected = {}
    for fixture in fixtures:
        signatures = set()
        for mode in MODES:
            rows = [row for row in evidence["receipts"]
                    if (row["fixture"], row["mode"], row["level"]) ==
                    (fixture, mode, "O3")]
            if len(rows) != 1:
                raise ValueError(f"missing or duplicate O3 receipt: {fixture}/{mode}")
            row = rows[0]
            if row.get("call_boundaries", "fixed") != boundaries:
                raise ValueError("candidates used different call-boundary policies")
            case = directory / f"{fixture}-{mode}-O3"
            for file, key in (("generated.lowered", "input_sha256"),
                              ("generated.o", "object_sha256"),
                              ("probe", "executable_sha256")):
                if digest(case / file) != row[key]:
                    raise ValueError(f"artifact changed after validation: {case / file}")
            if "codegen_ir_sha256" in row and digest(case / "generated.codegen.ll") != row["codegen_ir_sha256"]:
                raise ValueError("optimized IR changed after validation")
            signatures.add(row["input_sha256"])
            selected[fixture, mode] = case
        if len(signatures) != 1:
            raise ValueError(f"candidates used different MIR: {fixture}")
    return selected


def sections(output):
    result = {}
    for line in output.splitlines():
        match = re.fullmatch(r"\s*(\S+)\s+(\d+)\s+(?:0x[0-9a-fA-F]+|\d+)\s*", line)
        if match:
            name, size = match.groups()
            # Mach-O can expose the same section name in different segments.
            # llvm-size's SysV output omits segment names; retain the raw output
            # and aggregate every row rather than overwriting one segment.
            result[name] = result.get(name, 0) + int(size)
    if not result:
        raise ValueError("size tool returned no section sizes")
    return result


def observation(text, outcome, iterations):
    match = re.fullmatch(r"(\d+) (\d+) (\d+)\n", text)
    if not match:
        raise ValueError(f"invalid timing observation: {text[:120]!r}")
    count, elapsed, total = map(int, match.groups())
    if count != iterations or total != checksum(outcome, iterations) or elapsed <= 0:
        raise ValueError("timing observation failed its independent result oracle")
    return elapsed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--size-tool", default=shutil.which("llvm-size"))
    parser.add_argument("--fixtures", nargs="+", choices=FIXTURES, default=FIXTURES)
    parser.add_argument("--samples", type=int, default=9)
    parser.add_argument("--warmups", type=int, default=2)
    parser.add_argument("--success-iterations", type=int, default=2000000)
    parser.add_argument("--failure-iterations", type=int, default=10000)
    args = parser.parse_args()
    if (not args.size_tool or args.samples < 2 or args.warmups < 1 or
            min(args.success_iterations, args.failure_iterations) < 2 or
            len(set(args.fixtures)) != len(args.fixtures)):
        parser.error("require size tool, unique fixtures, >=2 samples/iterations and >=1 warmup")
    evidence_path = args.evidence.resolve()
    evidence = json.loads(evidence_path.read_text())
    artifacts = verify_artifacts(evidence, evidence_path.parent, args.fixtures)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "schema": 1, "completed": False, "evidence_sha256": digest(evidence_path),
        "tool_sha256": digest(Path(__file__)), "python": sys.version,
        "platform": platform.platform(), "machine": platform.machine(),
        "samples": args.samples, "warmups": args.warmups,
        "runtime_sha256": evidence["runtime_sha256"],
        "measurement": "in-process checked loop; alternating candidates",
        "call_boundaries": evidence.get("call_boundaries", "fixed"),
        "limitations": ["not a whole-program speedup or production acceptance",
                        "no worker/callback boundary or allocation measurement",
                        "no full-compiler build cost or host-contention isolation"],
        "emission_build_metrics": evidence.get("build_metrics", False),
        "artifacts": [], "comparisons": [],
    }

    def save():
        (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(command, name):
        start = time.perf_counter_ns()
        result = subprocess.run([str(x) for x in command], capture_output=True,
                                text=True, timeout=180)
        elapsed = time.perf_counter_ns() - start
        (output / f"{name}.json").write_text(json.dumps({
            "command": [str(x) for x in command], "exit_code": result.returncode,
            "stdout": result.stdout, "stderr": result.stderr,
            "process_ns": elapsed,
        }, indent=2) + "\n")
        if result.returncode or result.stderr:
            raise RuntimeError(f"cost command failed: {name}; retained in {output}")
        return result.stdout

    save()
    for fixture in args.fixtures:
        for mode in MODES:
            case = artifacts[fixture, mode]
            sizes = sections(run([args.size_tool, "-A", "-d", case / "generated.o"],
                                 f"{fixture}-{mode}-sections"))
            report["artifacts"].append({
                "fixture": fixture, "mode": mode, "sections_by_name": sizes,
                "code_bytes": sum(size for name, size in sizes.items()
                                  if name.startswith((".text", "__text"))),
                "unwind_bytes": sum(size for name, size in sizes.items()
                                    if name.startswith((".eh_frame", ".gcc_except_table",
                                                        "__eh_frame", "__gcc_except_tab",
                                                        "__compact_unwind"))),
                "object_bytes": (case / "generated.o").stat().st_size,
                "executable_bytes": (case / "probe").stat().st_size,
                "executable_sha256": digest(case / "probe"),
                "build": next(row.get("build", {}) for row in evidence["receipts"]
                              if (row["fixture"], row["mode"], row["level"]) ==
                              (fixture, mode, "O3")),
            })
        for dispatch in ("direct", "indirect"):
            for outcome in ("success", "failure", "mixed"):
                iterations = (args.success_iterations if outcome == "success"
                              else args.failure_iterations)
                times = {mode: [] for mode in MODES}
                for sample in range(args.warmups + args.samples):
                    for mode in MODES if sample % 2 == 0 else reversed(MODES):
                        name = f"{fixture}-{dispatch}-{outcome}-{sample}-{mode}"
                        text = run([artifacts[fixture, mode] / "probe", outcome,
                                    dispatch, iterations], name)
                        elapsed = observation(text, outcome, iterations)
                        if sample >= args.warmups:
                            times[mode].append(elapsed / iterations)
                medians = {mode: statistics.median(values) for mode, values in times.items()}
                report["comparisons"].append({
                    "fixture": fixture, "dispatch": dispatch, "outcome": outcome,
                    "iterations": iterations, "ns_per_call": times,
                    "median_ns_per_call": medians,
                    "unwind_over_status": medians["unwind"] / medians["status"],
                })
                save()
    verify_artifacts(evidence, evidence_path.parent, args.fixtures)
    report["completed"] = True
    save()
    print(f"C04 cost: {len(report['comparisons'])} checked comparisons; {output / 'cost.json'}")


if __name__ == "__main__":
    main()
