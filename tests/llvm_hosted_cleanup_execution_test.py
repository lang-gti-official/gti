#!/usr/bin/env python3
"""Execute complete owned-argv cleanup with an explicit alternative GTI library.

This is source-level fault injection, not stock-library readiness evidence.
The actual public driver emits the full program; no MIR or LLVM body is replaced.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from llvm_hosted_cleanup_evidence_test import (
    TRACE_PATH, anchor_controls, install_trace_module, inventory, transformed_sources, validate_inventory,
)


def run(command, root, env, directory, label):
    result = subprocess.run(command, cwd=root, env=env, capture_output=True,
                            text=True, check=False, timeout=120)
    (directory / f"{label}.stdout").write_text(result.stdout, encoding="utf-8")
    (directory / f"{label}.stderr").write_text(result.stderr, encoding="utf-8")
    (directory / f"{label}.json").write_text(json.dumps({
        "command": [str(arg) for arg in command], "returncode": result.returncode,
    }, indent=2), encoding="utf-8")
    return result


def check(result, arguments, executable, library, failures, markers=False):
    primary = "fixture-primary" in arguments and failures
    secondary = "fixture-secondary" in arguments and primary
    expected_code = 70 if primary else 0
    if result.returncode != expected_code:
        raise AssertionError(f"cleanup status {result.returncode}, expected {expected_code}: {result.stderr}")
    lines = result.stdout.splitlines()
    if lines.count("main-entered") != (0 if primary else 1):
        raise AssertionError(f"entry execution drift: {lines}")
    if not failures:
        if lines != ["main-entered"] or result.stderr:
            raise AssertionError("baseline library changed behavior")
        return
    if lines.count("primary-trigger") != int(primary):
        raise AssertionError(f"constructor trigger count drift: {lines}")
    completed = arguments[:arguments.index("fixture-primary")] if primary else arguments
    expected_drops = [f"drop:{value}" for value in reversed([str(executable), *completed])]
    if secondary:
        # A second failure during cleanup takes GTI-R0014 immediately; it must
        # not resume the drain or destroy earlier objects (ADR 007).
        expected_drops = expected_drops[:1]
    observed_drops = [line for line in lines if line.startswith("drop:") and line != "drop:"]
    if observed_drops != expected_drops:
        raise AssertionError(f"completed owners must drop once in reverse order: {observed_drops} != {expected_drops}")
    release = "lease-release-returned"
    if not markers and release in lines:
        raise AssertionError("uninstrumented library emitted a release marker")
    if markers:
        # These trace source-level release returns, not allocator/free counts.
        # Each non-failing string drop must finish its lease release; a second
        # failure must stop before that release or any earlier owner's cleanup.
        if secondary:
            if lines[-1] != expected_drops[0] or release not in lines:
                raise AssertionError("secondary cleanup continued past its failing destructor")
        else:
            for drop in expected_drops:
                position = lines.index(drop)
                if lines[position + 1:position + 2] != [release]:
                    raise AssertionError(f"missing source release after {drop}")
            if lines[-2:] != [release, release]:
                raise AssertionError("last string/vector source releases did not complete")
    if primary:
        if lines.index("primary-trigger") > lines.index(expected_drops[0]):
            raise AssertionError("failure cleanup ran before the primary trigger")
        source = (library / "std/string.gti").read_text(encoding="utf-8").splitlines()
        primary_line = next(i for i, text in enumerate(source, 1)
                            if "hosted cleanup fixture primary" in text)
        if secondary:
            secondary_line = next(i for i, text in enumerate(source, 1)
                                  if "hosted cleanup fixture secondary" in text)
            expected_error = (
                f"<std/string>:{secondary_line}: runtime error[GTI-R0014]: "
                "failure_during_cleanup; primary [GTI-R0017] at "
                f"<std/string>:{primary_line} message=\"hosted cleanup fixture primary\"; "
                "secondary [GTI-R0017] message=\"hosted cleanup fixture secondary\"\n")
        else:
            expected_error = (
                f"<std/string>:{primary_line}: runtime error[GTI-R0017]: "
                "panic: \"hosted cleanup fixture primary\"\n")
        if result.stderr != expected_error:
            raise AssertionError(f"lost original failure attribution: {result.stderr}")
    elif result.stderr:
        raise AssertionError(f"successful cleanup reported a failure: {result.stderr}")


def main():
    gti, root, build = (Path(value).resolve() for value in sys.argv[1:4])
    endpoints = [("cpp", "O3"), ("llvm", "O0"), ("llvm", "O3")]
    if len(sys.argv) == 5:
        backend, level = sys.argv[4].split(":")
        if (backend, level) not in endpoints:
            raise ValueError("unknown cleanup endpoint")
        endpoints = [(backend, level)]
    evidence = Path(tempfile.mkdtemp(prefix="llvm-hosted-source-cleanup-", dir=build))
    print(f"Alternative source cleanup evidence: {evidence}", flush=True)
    original = inventory(root / "stdlib")
    anchor_controls(root)
    source = root / "tests/fixtures/llvm_hosted_cleanup_entry.gti"
    for variant, failures, markers in (("baseline", False, False),
                                       ("panics", True, False),
                                       ("release-markers", True, True)):
        directory = evidence / variant
        directory.mkdir()
        library = directory / "stdlib"
        shutil.copytree(root / "stdlib", library)
        for name, text in transformed_sources(root, failures=failures,
                                               release_marker=markers).items():
            (library / name).write_text(text, encoding="utf-8")
        install_trace_module(library, root / "tests/fixtures/llvm_hosted_cleanup_trace.gti")
        changed = {"module-map.json"} | ({"std/string.gti"} if failures else set())
        if markers:
            changed.add("std/detail/memory_storage.gti")
        transformed = inventory(library)
        validate_inventory(original, transformed, changed)
        (directory / "library.json").write_text(json.dumps(transformed, sort_keys=True),
                                                encoding="utf-8")
        env = {**os.environ, "NO_COLOR": "1", "GTI_STDLIB_PATH": str(library)}
        for backend, level in endpoints:
            endpoint = directory / f"{backend}-{level}"
            endpoint.mkdir()
            executable = endpoint / "program"
            command = [str(gti), str(source), "--backend", backend, f"-{level}",
                       "-o", str(executable)]
            if backend == "cpp":
                command += ["--std", "c++23"]
            result = run(command, root, env, endpoint, "build")
            if result.returncode:
                raise AssertionError(f"full source cleanup build failed: {result.stderr}")
            for name, arguments in (
                ("success", ["prior-one", "prior-two"]),
                ("primary", ["prior-one", "prior-two", "fixture-primary"]),
                ("secondary", ["prior-one", "fixture-secondary", "fixture-primary"]),
            ):
                observed = run([str(executable), *arguments], root, env, endpoint, name)
                check(observed, arguments, executable, library, failures, markers)
                if markers:
                    missing = subprocess.CompletedProcess(
                        observed.args, observed.returncode,
                        observed.stdout.replace("lease-release-returned\n", ""),
                        observed.stderr)
                    try:
                        check(missing, arguments, executable, library, failures, markers)
                    except AssertionError:
                        pass
                    else:
                        raise AssertionError("missing source release markers were accepted")
                print(f"{variant} {backend}/{level} {name}: passed", flush=True)
        if inventory(library) != transformed:
            raise AssertionError("alternative library changed during execution")
    if inventory(root / "stdlib") != original:
        raise AssertionError("stock library changed during the test")


if __name__ == "__main__":
    main()
