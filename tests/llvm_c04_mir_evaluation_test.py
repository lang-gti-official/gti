#!/usr/bin/env python3
"""Opt-in MIR-derived transport experiment, never a production acceptance gate."""

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import re
import shutil
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, directory, name, *, expected=0, timeout=180, measure_build=False):
    command = [str(arg) for arg in command]
    metrics = directory / f"{name}.metrics"
    if measure_build:
        if platform.system() not in ("Darwin", "Linux"):
            raise ValueError("build metrics require Darwin or Linux system time")
        command = ["/usr/bin/time", "-l" if platform.system() == "Darwin" else "-v",
                   "-o", str(metrics), *command]
    start = time.perf_counter()
    result = subprocess.run(command, capture_output=True,
                            text=True, check=False, timeout=timeout,
                            env={**os.environ, "NO_COLOR": "1"})
    elapsed = time.perf_counter() - start
    (directory / f"{name}.stdout").write_text(result.stdout)
    (directory / f"{name}.stderr").write_text(result.stderr)
    record = {"command": command, "returncode": result.returncode,
              "seconds": elapsed}
    if measure_build:
        pattern = (r"(\d+)\s+maximum resident set size" if platform.system() == "Darwin"
                   else r"Maximum resident set size \(kbytes\):\s*(\d+)")
        match = re.search(pattern, metrics.read_text())
        if not match:
            raise ValueError(f"missing build peak memory: {metrics}")
        record["peak_rss_kib"] = int(match[1]) / (1024 if platform.system() == "Darwin" else 1)
    (directory / f"{name}.json").write_text(json.dumps(record, indent=2))
    if result.returncode != expected:
        raise AssertionError(f"{name}: exit {result.returncode}, expected {expected}: {result.stderr}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--status-emitter", required=True, type=Path)
    parser.add_argument("--unwind-emitter", required=True, type=Path)
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cxx", default=shutil.which("c++"))
    parser.add_argument("--sanitized", action="store_true")
    parser.add_argument("--build-metrics", action="store_true")
    parser.add_argument("--call-boundaries", choices=("fixed", "optimizable"), default="fixed")
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = root / "tests/fixtures/llvm_c04_mir"
    receipts = []
    profiles = [(fixture, mode, emitter.resolve(), level)
                for fixture in ("scalar", "cleanup", "borrowed_cleanup",
                                "fallible_cleanup", "fallible_constructor", "nested_failure",
                                "owned_field", "owned_child_failure", "owned_rollback",
                                "owned_nested_failure", "owned_parameter",
                                "prepared_parameter", "owned_return", "return_cleanup",
                                "raw_field", "replacement", "replacement_cleanup",
                                "replacement_preparation", "replacement_nested_failure")
                for mode, emitter in (("status", args.status_emitter),
                                      ("unwind", args.unwind_emitter))
                for level in ("O0", "O3")]
    snapshots = {}
    for fixture, mode, emitter, level in profiles:
        directory = output / f"{fixture}-{mode}-{level}"
        directory.mkdir()
        prefix = directory / "generated"
        run([emitter, fixtures / f"{fixture}.gti", prefix, level, args.call_boundaries], directory, "emit",
            measure_build=args.build_metrics)
        snapshot = prefix.with_suffix(".lowered").read_bytes()
        if fixture in snapshots and snapshot != snapshots[fixture]:
            raise AssertionError("transport candidates did not consume identical sealed-program text")
        snapshots[fixture] = snapshot
        ir = prefix.with_suffix(".ll").read_text()
        scalar_calls = None
        if (("noinline" in ir) != (args.call_boundaries == "fixed") or
                "uwtable(sync)" not in ir or "gti.c04.transport" not in ir or
                f'!"gti.c04.fixed-boundaries", i32 {int(args.call_boundaries == "fixed")}' not in ir):
            raise AssertionError("lost explicit experimental boundary/unwind-table metadata")
        if fixture == "scalar" and level == "O3":
            entry = re.search(r"^#define GTI_C04_ENTRY (\w+)$",
                              prefix.with_suffix(".h").read_text(), re.MULTILINE)[1]
            optimized = prefix.with_suffix(".codegen.ll").read_text()
            body = re.search(r"^define [^\n]*@" + re.escape(entry) +
                             r"\([^\n]*\{\n(.*?)^\}", optimized,
                             re.MULTILINE | re.DOTALL)
            if not body:
                raise AssertionError("missing optimized scalar entry")
            scalar_calls = len(re.findall(r"\b(?:call|invoke) [^\n]*@__gti_c04_" +
                                         mode + r"_v2_f\d+\(", body[1]))
            if args.call_boundaries == "fixed" and scalar_calls == 0:
                raise AssertionError("scalar O3 pipeline lost the fixed call boundary")
            # Ordinary LLVM cost heuristics may retain calls. Record the actual
            # outcome instead of forcing inlining to favor either transport.
        if ("__gti_c04_evaluate_raise" in ir) != (mode == "unwind"):
            raise AssertionError("wrong transport implementation in isolated artifact")
        if mode == "unwind" and fixture != "cleanup":
            for operation in ("invoke ", "landingpad ", "resume ", "__gxx_personality_v0"):
                if operation not in ir:
                    raise AssertionError(f"missing MIR unwind lowering: {operation}")
        if mode == "unwind" and fixture == "fallible_cleanup":
            if "invoke i1 @__gti_native_cleanup_" not in ir:
                raise AssertionError("fallible Drop bypassed its MIR failure edge")
        executable = directory / "probe"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitized else []
        flags += [f"-DGTI_C04_FAILED_CONSTRUCTION={int(fixture in ('fallible_constructor', 'replacement_preparation'))}"]
        flags += [f"-DGTI_C04_OWNED_ROLLBACK={int(fixture == 'owned_rollback')}"]
        run([args.cxx, "-std=c++20", "-O3", *flags, "-pthread",
             "-I", root / "runtime/include", "-I", directory,
             fixtures / "driver.cpp", prefix.with_suffix(".o"),
             args.runtime.resolve(), "-o", executable], directory, "link",
            measure_build=args.build_metrics)
        for outcome in ("success", "failure"):
            for dispatch in ("direct", "indirect"):
                nested = fixture in ("nested_failure", "owned_nested_failure", "replacement_nested_failure")
                if nested and outcome == "failure":
                    observation = directory / f"{outcome}-{dispatch}.slots"
                    observation.write_bytes(struct.pack("=iii", 0, 0, 0))
                    result = run([executable, outcome, dispatch, "1", observation],
                                 directory, f"{outcome}-{dispatch}", expected=70, timeout=15)
                    primary = {"nested_failure": 19, "owned_nested_failure": 28,
                               "replacement_nested_failure": 11}[fixture]
                    error = (f"{fixture}.gti:11: runtime error[GTI-R0014]: "
                             "failure_during_cleanup; primary [GTI-R0001] at "
                             f"{fixture}.gti:{primary}; secondary [GTI-R0001]\n")
                    if (result.stdout, result.stderr) != ("", error):
                        raise AssertionError("lost original primary/secondary failure attribution")
                    expected_slots = (0, 1, 1) if fixture == "owned_nested_failure" else (1, 1, 0)
                    if struct.unpack("=iii", observation.read_bytes()) != expected_slots:
                        raise AssertionError("cleanup continued after the secondary failure")
                    continue
                result = run([executable, outcome, dispatch], directory,
                             f"{outcome}-{dispatch}", timeout=15)
                line = {"scalar": 1, "cleanup": 2, "borrowed_cleanup": 12,
                        "fallible_cleanup": 11, "fallible_constructor": 13,
                        "nested_failure": 19, "owned_field": 21,
                        "owned_child_failure": 11, "owned_rollback": 16,
                        "owned_nested_failure": 28, "owned_parameter": 11,
                        "prepared_parameter": 16, "owned_return": 13,
                        "return_cleanup": 15, "raw_field": 2,
                        "replacement": 10, "replacement_cleanup": 11,
                        "replacement_preparation": 5, "replacement_nested_failure": 11}[fixture]
                expected_error = f"{fixture}.gti:{line}: runtime error[GTI-R0001]: integer_overflow in addition\n"
                expected = ("1\n", "") if outcome == "success" else ("", expected_error)
                if (result.stdout, result.stderr) != expected:
                    raise AssertionError(f"{mode}/{level}/{outcome}/{dispatch}: {result}")
                repeated = run([executable, outcome, dispatch, "64"], directory,
                               f"repeat-{outcome}-{dispatch}", timeout=15)
                if repeated.stderr or not re.fullmatch(r"64 \d+ \d+\n", repeated.stdout):
                    raise AssertionError("repeated entry/cleanup/failure carrier reset failed")
        for dispatch in ("direct", "indirect"):
            if fixture not in ("nested_failure", "owned_nested_failure", "replacement_nested_failure"):
                run([executable, "mixed", dispatch, "64"], directory,
                    f"repeat-mixed-{dispatch}", timeout=15)
        for negative in ("array_field", "opaque_pointer_field", "native_call", "global", "projected_replacement"):
            destination = directory / negative
            marker = b"existing artifact must survive rejection\n"
            destination.with_suffix(".o").write_bytes(marker)
            result = run([emitter, fixtures / f"{negative}.gti", destination, level, args.call_boundaries],
                         directory, f"reject-{negative}", expected=65)
            if "C04 evaluation" not in result.stderr:
                raise AssertionError("negative fixture failed before the structural evaluation boundary")
            if negative.endswith("_field") and "class outside ordinary scalar/reference/observer/record-field cleanup profile" not in result.stderr:
                raise AssertionError("field fixture did not reach its exact boundary")
            if negative == "projected_replacement" and "replacement outside whole ordinary record binding" not in result.stderr:
                raise AssertionError("replacement escaped the bounded transfer profile")
            if destination.with_suffix(".o").read_bytes() != marker:
                raise AssertionError("rejected experimental shape overwrote an artifact")
        receipts.append({"fixture": fixture, "mode": mode, "level": level, "emitter_sha256": digest(emitter),
                         "call_boundaries": args.call_boundaries,
                         "scalar_entry_calls_after_codegen": scalar_calls,
                         "input_sha256": hashlib.sha256(snapshot).hexdigest(),
                         "object_sha256": digest(prefix.with_suffix(".o")),
                         "codegen_ir_sha256": digest(prefix.with_suffix(".codegen.ll")),
                         "object_bytes": prefix.with_suffix(".o").stat().st_size,
                         "build": {phase: json.loads((directory / f"{phase}.json").read_text())
                                   for phase in ("emit", "link")},
                         "executable_sha256": digest(executable)})
        print(f"{fixture}/{mode}/{level}: lifecycle outcomes, direct/indirect and 5 rejections passed", flush=True)
    evidence = {
        "scope": "scalar MIR, fallible scalar/reference/record-field lifecycle, explicit unsafe int-pointer observers, owning parameter/return transfer and whole-record replacement; test-owned C++ carrier",
        "call_boundaries": args.call_boundaries,
        "production_transport_selected": False,
        "runtime_sha256": digest(args.runtime),
        "sanitized_native_carrier": args.sanitized,
        "llvm_object_instrumented": False,
        "build_metrics": args.build_metrics,
        "limitations": ["no projected replacement, callbacks, user TLS or workers",
                        "raw access limited to explicit unsafe scalar observation; no pointer arithmetic or allocation",
                        "no arrays, private owner representations or hierarchy",
                        "no OOM, foreign/forced unwind or artifact-unloading proof",
                        "no timing collected; not a production speedup claim"],
        "receipts": receipts,
    }
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2))
    # Managed runners retain step logs, not arbitrary files in the build tree.
    print(json.dumps(evidence, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
