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


def run(command, directory, name, *, expected=0, timeout=180, measure_build=False, environment=None):
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
                            env={**os.environ, "NO_COLOR": "1", **(environment or {})})
    elapsed = time.perf_counter() - start
    (directory / f"{name}.stdout").write_text(result.stdout)
    (directory / f"{name}.stderr").write_text(result.stderr)
    record = {"command": command, "returncode": result.returncode,
              "seconds": elapsed, "environment": environment or {}}
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
    parser.add_argument("--profile", choices=("scalar-cleanup", "native-boundary", "dynamic-calls", "scoped-workers"), default="scalar-cleanup")
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    fixtures = root / "tests/fixtures/llvm_c04_mir"
    receipts = []
    native = args.profile == "native-boundary"
    dynamic = args.profile == "dynamic-calls"
    workers = args.profile == "scoped-workers"
    selected_fixtures = ("scoped_worker", "scoped_cleanup", "scoped_cleanup_failure", "scoped_cleanup_secondary") if workers else ("dynamic_pointer",) if dynamic else ("native_boundary",) if native else ("scalar", "cleanup", "borrowed_cleanup",
                                "fallible_cleanup", "fallible_constructor", "nested_failure",
                                "owned_field", "owned_child_failure", "owned_rollback",
                                "owned_nested_failure", "owned_parameter",
                                "prepared_parameter", "owned_return", "return_cleanup",
                                "raw_field", "replacement", "replacement_cleanup",
                                "replacement_preparation", "replacement_nested_failure",
                                "projected_replacement", "field_replacement_cleanup",
                                "field_replacement_preparation", "field_replacement_nested_failure")
    profiles = [(fixture, mode, emitter.resolve(), level)
                for fixture in selected_fixtures
                for mode, emitter in (("status", args.status_emitter),
                                      ("unwind", args.unwind_emitter))
                for level in ("O0", "O3")]
    snapshots = {}
    for fixture, mode, emitter, level in profiles:
        directory = output / f"{fixture}-{mode}-{level}"
        directory.mkdir()
        prefix = directory / "generated"
        profile_args = [args.profile, "--worker-contracts"] if workers else [args.profile, "--dynamic-contracts"] if dynamic else [args.profile, "--native-contracts"] if native else []
        emitted = run([emitter, fixtures / f"{fixture}.gti", prefix, level, args.call_boundaries, *profile_args], directory, "emit",
            measure_build=args.build_metrics)
        if native and not re.search(r"native mutation controls: [1-9]\d*", emitted.stdout):
            raise AssertionError("native profile did not exercise its mutation controls")
        if dynamic and not re.search(r"dynamic mutation controls: [1-9]\d*", emitted.stdout):
            raise AssertionError("dynamic profile did not exercise its mutation controls")
        if workers and not re.search(r"worker mutation controls: [1-9]\d*", emitted.stdout):
            raise AssertionError("worker profile did not exercise its mutation controls")
        snapshot = prefix.with_suffix(".lowered").read_bytes()
        if fixture in snapshots and snapshot != snapshots[fixture]:
            raise AssertionError("transport candidates did not consume identical sealed-program text")
        snapshots[fixture] = snapshot
        ir = prefix.with_suffix(".ll").read_text()
        if "load %gti.failure_channel.v1," in ir:
            raise AssertionError("failure channel copy regressed to aggregate SSA transport")
        if "load %gti.c.failure_record.v2," in ir:
            raise AssertionError("emergency record copy regressed to aggregate SSA transport")
        if mode == "status" and "@llvm.memmove." not in ir:
            raise AssertionError("status propagation lost its bulk failure-channel transfer")
        scalar_calls = None
        attributes = dict(re.findall(r"^attributes #(\d+) = \{([^\n]*)\}",
                                     ir, re.MULTILINE))
        body_attributes = re.findall(
            r"^define [^\n]*@__gti_(?:c04_\w+_v2_f|native_v1_body_)"
            r"[^\s(]*\([^\n]*\) #(\d+)", ir, re.MULTILINE)
        if not body_attributes or any(
                ("noinline" in attributes[group]) != (args.call_boundaries == "fixed")
                for group in body_attributes):
            raise AssertionError("lost experimental GTI body call-boundary policy")
        terminal_helpers = re.findall(
            r"^define internal void @__gti_failure_terminate_cleanup\([^\n]*\) #(\d+) \{\n(.*?)^\}",
            ir, re.MULTILINE | re.DOTALL)
        if "alloca %gti.c.failure_emergency.v2" in ir:
            if len(terminal_helpers) != 1:
                raise AssertionError("emergency envelope must have one isolated terminal helper")
            group, helper_body = terminal_helpers[0]
            if (not all(flag in attributes[group].split()
                        for flag in ("cold", "noinline", "noreturn")) or
                    "nounwind" in attributes[group].split() or
                    helper_body.count("alloca %gti.c.failure_emergency.v2") != 1 or
                    ir.count("alloca %gti.c.failure_emergency.v2") != 1):
                raise AssertionError("lost isolated emergency storage or exact terminal attributes")
        if ("uwtable(sync)" not in ir or "gti.c04.transport" not in ir or
                f'!"gti.c04.fixed-boundaries", i32 {int(args.call_boundaries == "fixed")}' not in ir or
                f'!"gti.c04.native-boundary", i32 {int(native)}' not in ir or
                f'!"gti.c04.dynamic-calls", i32 {int(dynamic)}' not in ir or
                f'!"gti.c04.scoped-workers", i32 {int(workers)}' not in ir):
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
        if "__gti_c04_evaluate_current" in ir:
            transfers = re.findall(r"call void @__gti_c04_evaluate_current\(([^)]*)\)", ir)
            if ("declare void @__gti_c04_evaluate_current(ptr, ptr)" not in ir or
                    not transfers or any(not re.fullmatch(r"ptr [^,]+, ptr [^,]+", arguments)
                                         for arguments in transfers)):
                raise AssertionError("C04 channel transfer lost its actual landingpad exception argument")
        if mode == "unwind" and fixture != "cleanup":
            for operation in ("invoke ", "landingpad ", "resume ", "__gxx_personality_v0"):
                if operation not in ir:
                    raise AssertionError(f"missing MIR unwind lowering: {operation}")
        if mode == "unwind" and fixture == "fallible_cleanup":
            if "invoke i1 @__gti_native_cleanup_" not in ir:
                raise AssertionError("fallible Drop bypassed its MIR failure edge")
        executable = directory / "probe"
        flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitized else []
        flags += [f"-DGTI_C04_FAILED_CONSTRUCTION={int(fixture in ('fallible_constructor', 'replacement_preparation', 'field_replacement_preparation'))}"]
        flags += [f"-DGTI_C04_OWNED_ROLLBACK={int(fixture == 'owned_rollback')}"]
        flags += [f"-DGTI_C04_TASK_CLEANUP={int(workers and fixture != 'scoped_worker')}"]
        run([args.cxx, "-std=c++20", "-O3", *flags, "-pthread",
             "-I", root / "runtime/include", "-I", directory,
             fixtures / "driver.cpp", prefix.with_suffix(".o"),
             args.runtime.resolve(), "-o", executable], directory, "link",
            measure_build=args.build_metrics)
        for outcome in ("success", "failure"):
            for dispatch in ("direct", "indirect"):
                nested = fixture in ("nested_failure", "owned_nested_failure", "replacement_nested_failure", "field_replacement_nested_failure", "scoped_cleanup_secondary")
                if nested and outcome == "failure":
                    observation = directory / f"{outcome}-{dispatch}.slots"
                    observation.write_bytes(struct.pack("=iii", 0, 0, 0))
                    result = run([executable, outcome, dispatch, "1", observation],
                                 directory, f"{outcome}-{dispatch}", expected=70, timeout=15)
                    primary = {"nested_failure": 19, "owned_nested_failure": 28,
                               "replacement_nested_failure": 11,
                               "field_replacement_nested_failure": 11, "scoped_cleanup_secondary": 19}[fixture]
                    secondary = 15 if fixture == "scoped_cleanup_secondary" else 11
                    error = (f"{fixture}.gti:{secondary}: runtime error[GTI-R0014]: "
                             "failure_during_cleanup; primary [GTI-R0001] at "
                             f"{fixture}.gti:{primary}; secondary [GTI-R0001]\n")
                    if (result.stdout, result.stderr) != ("", error):
                        raise AssertionError("lost original primary/secondary failure attribution")
                    expected_slots = (1, 2, 0) if fixture == "scoped_cleanup_secondary" else (0, 1, 1) if fixture == "owned_nested_failure" else (1, 1, 0)
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
                        "replacement_preparation": 5, "replacement_nested_failure": 11,
                        "projected_replacement": 14, "field_replacement_cleanup": 11,
                        "field_replacement_preparation": 5, "field_replacement_nested_failure": 11,
                        "native_boundary": 20, "dynamic_pointer": 15, "scoped_worker": 14,
                        "scoped_cleanup": 16, "scoped_cleanup_failure": 15, "scoped_cleanup_secondary": 19}[fixture]
                expected_error = f"{fixture}.gti:{line}: runtime error[GTI-R0001]: integer_overflow in addition\n"
                expected = ("1\n", "") if outcome == "success" else ("", expected_error)
                if (result.stdout, result.stderr) != expected:
                    raise AssertionError(f"{mode}/{level}/{outcome}/{dispatch}: {result}")
                repeated = run([executable, outcome, dispatch, "64"], directory,
                               f"repeat-{outcome}-{dispatch}", timeout=15)
                if repeated.stderr or not re.fullmatch(r"64 \d+ \d+\n", repeated.stdout):
                    raise AssertionError("repeated entry/cleanup/failure carrier reset failed")
        for dispatch in ("direct", "indirect"):
            if fixture not in ("nested_failure", "owned_nested_failure", "replacement_nested_failure", "field_replacement_nested_failure", "scoped_cleanup_secondary"):
                run([executable, "mixed", dispatch, "64"], directory,
                    f"repeat-mixed-{dispatch}", timeout=15)
        for negative in ("array_field", "opaque_pointer_field", "native_call", "global", "borrowed_replacement"):
            destination = directory / negative
            marker = b"existing artifact must survive rejection\n"
            destination.with_suffix(".o").write_bytes(marker)
            result = run([emitter, fixtures / f"{negative}.gti", destination, level, args.call_boundaries],
                         directory, f"reject-{negative}", expected=65)
            if "C04 evaluation" not in result.stderr:
                raise AssertionError("negative fixture failed before the structural evaluation boundary")
            if negative.endswith("_field") and "class outside ordinary scalar/reference/observer/record-field cleanup profile" not in result.stderr:
                raise AssertionError("field fixture did not reach its exact boundary")
            if negative == "borrowed_replacement" and "function signature or definition" not in result.stderr:
                raise AssertionError("borrowed record escaped the local replacement profile")
            if destination.with_suffix(".o").read_bytes() != marker:
                raise AssertionError("rejected experimental shape overwrote an artifact")
        if dynamic:
            if "_function_pointer_" not in ir:
                raise AssertionError("dynamic profile did not emit actual GTI adapters")
            for source, profile_args in (("dynamic_pointer", ["scalar-cleanup", "--standard-library"]),
                                         ("scalar", ["dynamic-calls"]),
                                         ("native_boundary", ["dynamic-calls"])):
                destination = directory / ("dynamic-reject-" + source)
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / f"{source}.gti", destination, level,
                              args.call_boundaries, *profile_args], directory, destination.name, expected=65)
                if "C04" not in result.stderr or destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("dynamic profile lost explicit activation/artifact rejection")
        if workers:
            if "@__gti_c04_worker_scope" not in ir:
                raise AssertionError("worker profile did not emit its actual runtime join boundary")
            for source, profile_args in (("scoped_worker", ["scalar-cleanup", "--standard-library"]),
                                         ("scoped_worker", ["dynamic-calls"]),
                                         ("scalar", ["scoped-workers"])):
                destination = directory / ("worker-reject-" + source + "-" + profile_args[0])
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / f"{source}.gti", destination, level,
                              args.call_boundaries, *profile_args], directory, destination.name, expected=65)
                if destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("worker profile omission manufactured an artifact")
            for source, diagnostic in (("scoped_worker_local_failure", "emergency-handoff"),
                                       ("scoped_worker_nested", "nested worker cleanup")):
                destination = directory / ("worker-reject-" + source)
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / f"{source}.gti", destination, level,
                              args.call_boundaries, args.profile], directory, destination.name, expected=65)
                if diagnostic not in result.stderr or destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("worker emergency/nesting boundary was weakened")
            if fixture != "scoped_worker":
                source = directory / "missing-transfer.gti"
                source.write_text((fixtures / f"{fixture}.gti").read_text().replace("[[unsafe_transfer]]", ""))
                destination = directory / "missing-transfer"
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, source, destination, level, args.call_boundaries, args.profile],
                             directory, "missing-transfer", expected=65)
                if "GTI-S2084" not in result.stderr or destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("worker source transfer obligation was weakened")
            for started in (0, 1):
                for dispatch in ("direct", "indirect"):
                    result = run([executable, "success", dispatch, "64"], directory,
                                 f"creation-failure-{started}-{dispatch}",
                                 environment={"GTI_C04_WORKER_FAIL_AFTER": str(started)})
                    if result.stderr or not re.fullmatch(r"64 \d+ 2080\n", result.stdout):
                        raise AssertionError("worker creation rollback lost join-all or caller cleanup")
        if native:
            for profile in ([], ["scalar-cleanup"]):
                destination = directory / ("omitted-profile" if not profile else "scalar-profile")
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / "native_boundary.gti", destination, level,
                              args.call_boundaries, *profile], directory, destination.name, expected=65)
                if "callee outside selected static closure" not in result.stderr or destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("native profile omission manufactured an artifact")
            for negative in ("array_field", "opaque_pointer_field", "global", "borrowed_replacement", "native_indirect", "native_permission"):
                destination = directory / ("native-reject-" + negative)
                destination.with_suffix(".o").write_bytes(marker)
                result = run([emitter, fixtures / f"{negative}.gti", destination, level,
                              args.call_boundaries, args.profile], directory, destination.name, expected=65)
                diagnostic = "GTI-S2055" if negative == "native_permission" else "C04 evaluation"
                if diagnostic not in result.stderr or destination.with_suffix(".o").read_bytes() != marker:
                    raise AssertionError("native profile lost its rejection/lexical unsafe boundary")
            for outcome in ("success", "failure", "mixed"):
                for dispatch in ("direct", "indirect"):
                    run([executable, outcome, dispatch, "64"], directory,
                        f"translated-{outcome}-{dispatch}", environment={"GTI_C04_NATIVE_CASE": "translated"})
            for foreign in ("foreign-before", "foreign-cleanup", "impostor-cleanup", "opaque-cleanup"):
                for dispatch in ("direct", "indirect"):
                    observation = directory / f"{foreign}-{dispatch}.slots"
                    observation.write_bytes(struct.pack("=iii", 0, 0, 0))
                    outcome = "success" if foreign == "foreign-before" else "failure"
                    result = run([executable, outcome, dispatch, "1", observation], directory,
                                 f"{foreign}-{dispatch}", expected=86,
                                 environment={"GTI_C04_NATIVE_CASE": foreign})
                    if result.stdout or "GTI-R" in result.stderr:
                        raise AssertionError("foreign exception manufactured a GTI failure report")
                    if mode == "unwind":
                        match = re.fullmatch(r"C04 foreign exception: active=(\d) reads=(\d+)\n", result.stderr)
                        active = int(foreign != "foreign-before")
                        if (not match or int(match[1]) != active or
                                (int(match[2]) > 0) != bool(active)):
                            raise AssertionError("foreign exception reused or failed to distinguish the active GTI carrier")
                    elif result.stderr != "C04 foreign exception outside channel\n":
                        raise AssertionError("status experiment lost foreign terminal control")
                    expected_slots = (0, 0, 0) if foreign == "foreign-before" else (1, 1, 0)
                    if struct.unpack("=iii", observation.read_bytes()) != expected_slots:
                        raise AssertionError("foreign fault bypassed its observation point or continued cleanup")
        receipts.append({"fixture": fixture, "mode": mode, "level": level, "emitter_sha256": digest(emitter),
                         "profile": args.profile,
                         "call_boundaries": args.call_boundaries,
                         "scalar_entry_calls_after_codegen": scalar_calls,
                         "input_sha256": hashlib.sha256(snapshot).hexdigest(),
                         "object_sha256": digest(prefix.with_suffix(".o")),
                         "codegen_ir_sha256": digest(prefix.with_suffix(".codegen.ll")),
                         "object_bytes": prefix.with_suffix(".o").stat().st_size,
                         "build": {phase: json.loads((directory / f"{phase}.json").read_text())
                                   for phase in ("emit", "link")},
                         "executable_sha256": digest(executable)})
        print(f"{fixture}/{mode}/{level}: lifecycle outcomes, direct/indirect and profile rejections passed", flush=True)
    evidence = {
        "scope": ("sealed scoped task adapters, actual runtime workers and join-all, leaf worker cleanup and reverse task-owned caller cleanup including first/secondary failures; test-only candidate envelopes"
                  if workers else "sealed runtime-selected function-pointer adapters and source cleanup; test-owned C++ carrier"
                  if dynamic else "native direct scalar/int-pointer C imports, exact lexical unsafe facts, translated results, source cleanup, and labeled foreign-exception classification; test-owned C++ carrier"
                  if native else "scalar MIR, fallible scalar/reference/record-field lifecycle, explicit unsafe int-pointer observers, owning parameter/return transfer and local record/field replacement; test-owned C++ carrier"),
        "call_boundaries": args.call_boundaries,
        "profile": args.profile,
        "head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
        "source_sha256": {str(path.relative_to(root)): digest(path) for path in
                          [Path(__file__).resolve(), fixtures / "driver.cpp", fixtures / "carrier.h",
                           *([fixtures / "native_boundary.gti", fixtures / "native_boundary_driver.h"] if native else []),
                           *([fixtures / "dynamic_pointer.gti"] if dynamic else []),
                           *([fixtures / (name + ".gti") for name in selected_fixtures] + [fixtures / "worker_driver.h"] if workers else [])]},
        "production_transport_selected": False,
        "runtime_sha256": digest(args.runtime),
        "sanitized_native_carrier": args.sanitized,
        "llvm_object_instrumented": False,
        "build_metrics": args.build_metrics,
        "limitations": ["replacement limited to local bindings and ordinary field projections; no borrowed/raw/indexed destinations",
                        "no public callbacks or user TLS; workers only in the explicit scoped-workers profile",
                        "nested scopes and worker-local fallible cleanup remain rejected; task-owned cleanup runs on the caller after join",
                        "raw access limited to explicit unsafe scalar observation; no pointer arithmetic or allocation",
                        "no arrays, private owner representations or hierarchy",
                        "no OOM, forced unwind, cross-runtime C++ exception propagation or artifact-unloading proof",
                        "foreign fault injection is not a supported shim contract or production containment policy" if native else "no native import or foreign exception execution proof in scalar profile",
                        "no timing collected; not a production speedup claim"],
        "receipts": receipts,
    }
    (output / "evidence.json").write_text(json.dumps(evidence, indent=2))
    # Managed runners retain step logs, not arbitrary files in the build tree.
    print(json.dumps(evidence, sort_keys=True), flush=True)


if __name__ == "__main__":
    main()
