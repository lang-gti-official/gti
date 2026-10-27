#!/usr/bin/env python3
"""Observe C++/exception allocation providers in hash-bound C04 artifacts."""

import argparse
import json
import platform
import shutil
import subprocess
from pathlib import Path

from llvm_c04_cost import checksum, verify_artifacts
from llvm_c04_mir_evaluation_test import digest, run


def verify_relink_sources(evidence, root):
    sources = evidence.get("source_sha256", {})
    required = {"tests/fixtures/llvm_c04_mir/driver.cpp", "tests/fixtures/llvm_c04_mir/carrier.h"}
    if evidence.get("profile") == "scoped-workers":
        required.add("tests/fixtures/llvm_c04_mir/worker_driver.h")
    if not required.issubset(sources):
        raise ValueError("allocation relink lacks validated driver/carrier sources")
    for name, expected in sources.items():
        if digest(root / name) != expected:
            raise ValueError("allocation relink source differs from validated execution")


def validate_counts(counts, mode, outcome, iterations, fixture="native_boundary"):
    failed = iterations if outcome == "failure" else iterations // 2 if outcome == "mixed" else 0
    workers = fixture == "scoped_worker"
    # At least two real runtime thread states per joined scope; the host thread
    # library may allocate too. An independent empty-task run measures that
    # baseline through the same runtime, without GTI frames or failures.
    # Worker failure handoff
    # uses the existing C++ runtime marker in both candidates; unwind also
    # raises two GTI worker carriers and one caller carrier after join.
    allocations = counts.get("runtime_new_calls", 0)
    baseline_bytes = counts.get("runtime_new_bytes", 0)
    if (workers and (allocations < 2 * iterations or baseline_bytes < allocations or
                     counts.get("runtime_delete_calls") != allocations or
                     counts.get("runtime_exception_calls") != 0 or
                     counts.get("runtime_exception_bytes") != 0)):
        raise ValueError("worker allocation observation lacks an independent balanced runtime baseline")
    if not workers and any(counts.get(key, 0) != 0 for key in (
            "runtime_new_calls", "runtime_new_bytes", "runtime_delete_calls",
            "runtime_exception_calls", "runtime_exception_bytes")):
        raise ValueError("non-worker allocation evidence acquired a worker baseline")
    expected = failed * (5 if mode == "unwind" else 2) if workers else failed if mode == "unwind" else 0
    if (counts.get("controls") is not True or counts["new_calls"] != allocations or
            counts["new_bytes"] != baseline_bytes or
            counts["delete_calls"] != allocations or
            counts["exception_calls"] != expected or
            (counts["exception_bytes"] < expected if expected else counts["exception_bytes"] != 0)):
        raise ValueError("allocation provider observation disagrees with the closed source workload")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--fixtures", nargs="+", default=["native_boundary"])
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    evidence_path = args.evidence.resolve()
    evidence = json.loads(evidence_path.read_text())
    artifacts = verify_artifacts(evidence, evidence_path.parent, args.fixtures)
    verify_relink_sources(evidence, root)
    if digest(args.runtime) != evidence["runtime_sha256"]:
        raise ValueError("allocation link requires the original validated runtime")
    if platform.system() not in ("Darwin", "Linux"):
        raise ValueError("allocation providers reviewed only on Darwin/Linux")
    fixtures = root / "tests/fixtures/llvm_c04_mir"
    darwin = platform.system() == "Darwin"
    library = output / ("libc04alloc.dylib" if darwin else "libc04alloc.so")
    run([args.cxx, "-std=c++20", "-O3", "-fPIC",
         "-dynamiclib" if darwin else "-shared", fixtures / "allocation_probe.cpp",
         *([] if darwin else ["-ldl"]), "-o", library], output, "build-provider")
    report = {
        "schema": 1, "completed": False,
        "source_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "source_status": subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
        "evidence_sha256": digest(evidence_path), "runtime_sha256": digest(args.runtime),
        "provider_sha256": digest(library), "tool_sha256": digest(Path(__file__)),
        "sources": {name: digest(fixtures / name) for name in
                    ("allocation_probe.cpp", "allocation_probe.h", "driver.cpp", "carrier.h")},
        "platform": platform.platform(), "profile": evidence.get("profile", "scalar-cleanup"),
        "call_boundaries": evidence["call_boundaries"],
        "coverage": ["successful replacement C++ new requests and requested bytes",
                     "C++ delete calls", "Itanium primary exception allocation payload requests"],
        "limitations": ["not malloc/calloc/realloc or all dynamic-library allocations",
                        "exception provider may use a pool; payload bytes exclude ABI headers",
                        "exception deallocation uses runtime-internal calls and is not counted",
                        "not allocator residency, peak live bytes, or OOM behavior",
                        "instrumented counts are not timing evidence"], "observations": [],
    }

    def save():
        (output / "allocations.json").write_text(json.dumps(report, indent=2) + "\n")

    save()
    for (fixture, mode), case in artifacts.items():
        executable = output / f"{fixture}-{mode}"
        run([args.cxx, "-std=c++20", "-O3", "-pthread", "-DGTI_C04_ALLOCATIONS=1",
             "-DGTI_C04_FAILED_CONSTRUCTION=0", "-DGTI_C04_OWNED_ROLLBACK=0",
             "-I", root / "runtime/include", "-I", case,
             fixtures / "driver.cpp", case / "generated.o", args.runtime.resolve(),
             library, f"-Wl,-rpath,{output}", "-o", executable], output, executable.name + "-link")
        for outcome in ("success", "failure", "mixed"):
            for dispatch in ("direct", "indirect"):
                name = f"{fixture}-{mode}-{outcome}-{dispatch}"
                counts_path = output / (name + "-counts.json")
                result = run([executable, outcome, dispatch, "64"], output, name,
                             environment={"DYLD_INSERT_LIBRARIES" if darwin else "LD_PRELOAD": str(library),
                                          "GTI_C04_ALLOCATION_REPORT": str(counts_path)})
                words = result.stdout.split()
                if (result.stderr or len(words) != 3 or words[0] != "64" or
                        int(words[2]) != checksum(outcome, 64)):
                    raise ValueError("allocation probe lost its independent execution oracle")
                counts = json.loads(counts_path.read_text())
                validate_counts(counts, mode, outcome, 64, fixture)
                report["observations"].append({"fixture": fixture, "mode": mode,
                                               "outcome": outcome, "dispatch": dispatch,
                                               "iterations": 64, "counts": counts,
                                               "executable_sha256": digest(executable),
                                               "object_sha256": digest(case / "generated.o")})
                save()
    report["completed"] = True
    save()
    print(f"PASS: {len(report['observations'])} allocation observations; limits in {output / 'allocations.json'}")


if __name__ == "__main__":
    main()
