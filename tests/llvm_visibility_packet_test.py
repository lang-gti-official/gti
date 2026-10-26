#!/usr/bin/env python3
"""Replay the published O09 pointer-cost packet through O15's general policy.

This binds an external production packet to verified source/ABI/provenance and
executes it, but does not replace the composed production compiler gate. The
fixture checks 400M calls and checksum 80000000200000000 with checked overflow.
Timing is opt-in and reserves the existing shared windows on an idle host.
"""

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import resource
import statistics
import subprocess
import time

from llvm_runtime_cost_test import measurement_window


SOURCE_SHA = "853a7bba86bde98248fcfdbbbdc8d06058dd7b861a35527d5423cefaa9b5cc91"
PACKET_SHA = "d6abe7dbc8ea87e9d19456acd66936fdca589e9172b523f32099e3fdfcdb770e"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emitter", required=True, type=Path)
    parser.add_argument("--baseline-emitter", type=Path,
                        help="explicit retained pre-hook emitter built from checkpoint55602c87")
    parser.add_argument("--baseline-emitter-sha256")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--stdlib", required=True, type=Path)
    parser.add_argument("--packet", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cpp", type=Path, help="optional equivalent-contract O09 executable")
    parser.add_argument("--cpp-sha256")
    parser.add_argument("--measure", action="store_true")
    parser.add_argument("--all-levels", action="store_true",
                        help="rooted correctness at O0-O3; no timing or legacy baseline")
    parser.add_argument("--slot")
    parser.add_argument("--wait-seconds", type=int, default=1200)
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--repetitions", type=int, default=5)
    args = parser.parse_args()
    args.parent_suite = False
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    assert not output.exists() and not output.is_relative_to(root)
    assert digest(args.source) == SOURCE_SHA, "not the published checked O09 fixture"
    assert digest(args.packet) == PACKET_SHA, "not the published pre-O3 packet"
    assert not args.measure or args.slot, "a coordinated slot is required"
    assert not args.all_levels or not (args.measure or args.baseline_emitter)
    assert args.samples >= 3 and args.repetitions >= 3
    assert bool(args.cpp) == bool(args.cpp_sha256)
    assert bool(args.baseline_emitter) == bool(args.baseline_emitter_sha256)
    if args.baseline_emitter:
        assert digest(args.baseline_emitter) == args.baseline_emitter_sha256
    if args.cpp:
        assert digest(args.cpp) == args.cpp_sha256
    output.mkdir(parents=True)
    manifest = {
        "status": "running", "schema": 1, "commands": [], "timing": {},
        "source_sha256": SOURCE_SHA, "packet_sha256": PACKET_SHA,
        "emitter_sha256": digest(args.emitter), "runtime_sha256": digest(args.runtime),
        "baseline_emitter_sha256": args.baseline_emitter_sha256,
        "iterations_per_process": 400000000, "checksum": 80000000200000000,
        "slot": args.slot, "limits": [
            "packet replay uses the production ABI hook, but is not a composed production compiler",
            "optional baseline emitter is explicitly pinned to the retained pre-hook producer",
            "packet identity/ABI binding is not instruction-by-instruction MIR replay",
            "composed optimized production compiler and failure/cleanup gates still required",
            "external O09 packet is a dependency, not an in-tree permanent fixture",
        ],
        "timing_contract": "alternating native process groups including startup/cleanup/exit; same checked fixture",
    }
    env = dict(os.environ, NO_COLOR="1", GTI_COLOR="never")

    def run(argv, *, execution=False):
        result = subprocess.run([str(x) for x in argv], capture_output=True,
                                text=True, cwd=root, env=env, timeout=240)
        log = f"command-{len(manifest['commands']):03}.log"
        (output / log).write_text(result.stdout + result.stderr)
        manifest["commands"].append({"argv": [str(x) for x in argv],
                                     "exit": result.returncode, "log": log})
        assert result.returncode == 0, str(output / log)
        if execution:
            assert not result.stdout and not result.stderr, str(output / log)
        return result

    def quiet():
        deadline = time.monotonic() + args.wait_seconds
        while os.getloadavg()[0] > max(1, (os.cpu_count() or 1) * .25):
            assert time.monotonic() < deadline, "host not quiet; no valid measurement"
            time.sleep(1)

    reservation = ExitStack()
    try:
        reservation.enter_context(measurement_window(root, args))
        manifest["head"] = run(["git", "rev-parse", "HEAD"]).stdout.strip()
        manifest["dirty"] = run(["git", "status", "--short"]).stdout
        manifest["clang_version"] = run([args.clang, "--version"]).stdout.strip()
        manifest["host"] = run(["uname", "-a"]).stdout.strip()
        endpoints = {}
        modes = ([("rooted", level) for level in range(4)] if args.all_levels else
                 ([("baseline", 3), ("rooted", 3)] if args.baseline_emitter else [("rooted", 3)]))
        for mode, level in modes:
            label = f"{mode}-O{level}" if args.all_levels else mode
            obj, binary = output / f"{label}.o", output / label
            emitter = args.baseline_emitter if mode == "baseline" else args.emitter
            command = [emitter, args.source, args.stdlib, args.packet, obj, mode]
            if mode == "rooted":
                command.append(str(level))
            run(command)
            run([args.clang, obj, args.runtime, "-pthread", "-o", binary])
            run([binary], execution=True)
            report = json.loads(Path(str(obj) + ".json").read_text())
            ir = Path(str(obj) + ".ll").read_text()
            assert "llvm.uadd.with.overflow.i64" in ir, "checked arithmetic disappeared"
            manifest[label] = {"report": report, "object_sha256": digest(obj),
                              "binary_sha256": digest(binary), "binary_bytes": binary.stat().st_size}
            endpoints[label] = binary
            states = {x["after"] for x in report["source_definitions"]}
            if mode == "rooted":
                assert "external" in states and states & {"internal", "removed"}
                if level == 3:
                    assert "removed" in states
        if args.baseline_emitter:
            after = {x["symbol"]: x["after"] for x in manifest["rooted"]["report"]["source_definitions"]}
            before = {x["symbol"]: x["after"] for x in manifest["baseline"]["report"]["source_definitions"]}
            assert before.keys() == after.keys() and set(before.values()) == {"external"}
        if args.cpp:
            run([args.cpp], execution=True)
            endpoints["cpp"] = args.cpp
            manifest["cpp"] = {"binary_sha256": digest(args.cpp),
                               "binary_bytes": args.cpp.stat().st_size}
        if args.measure:
            quiet()
            manifest["load_before"] = os.getloadavg()
            samples = {name: [] for name in endpoints}
            for binary in endpoints.values():
                run([binary], execution=True)
            for sample in range(args.samples):
                names = list(endpoints)
                if sample % 2:
                    names.reverse()
                for name in names:
                    quiet()
                    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
                    started = time.perf_counter()
                    for _ in range(args.repetitions):
                        run([endpoints[name]], execution=True)
                    wall = time.perf_counter() - started
                    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
                    cpu = (cpu_after.ru_utime + cpu_after.ru_stime -
                           cpu_before.ru_utime - cpu_before.ru_stime)
                    assert wall >= .25, "sample group too short"
                    assert cpu / wall >= .95, "sample descheduled; no cost acceptance"
                    samples[name].append({"wall_seconds": wall, "cpu_seconds": cpu})
            manifest["timing"] = {
                "samples": samples, "repetitions": args.repetitions,
                "median_seconds_per_process": {
                    name: statistics.median(x["wall_seconds"] for x in values) / args.repetitions
                    for name, values in samples.items()},
                "load_after": os.getloadavg(),
            }
        manifest["status"] = "passed-packet-correctness-and-cost" if args.measure else "passed-packet-correctness"
        print(manifest["status"] + "; production composition remains required")
    except BaseException as error:
        manifest["status"], manifest["error"] = "failed", str(error)
        raise
    finally:
        reservation.close()
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
