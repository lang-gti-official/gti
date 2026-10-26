#!/usr/bin/env python3
"""Production source callbacks, recursive C signatures and terminal containment."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest

SOURCE_PATHS = {name: f"tests/fixtures/llvm_native_c_abi/{name}.gti" for name in
                ("native_callbacks", "native_callback_failure", "native_callback_exception")}


def evidence_cases(root):
    return {name: (root / path).read_text() for name, path in SOURCE_PATHS.items()}


def evidence_profiles(root):
    del root
    return {"native-signatures": ("native_callbacks",), "callbacks": tuple(SOURCE_PATHS)}


def evidence_manifest(root):
    return build_manifest("llvm_native_c_callback_execution", evidence_cases(root),
                          evidence_profiles(root), SOURCE_PATHS)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    cxx = shutil.which(args.cxx) or args.cxx
    environment = {**os.environ, "GTI_CXX": cxx, "CXX": cxx}
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "platform": platform.platform(), "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(),
              "input_sha256": {path: sha256((root / path).read_bytes()).hexdigest()
                               for path in SOURCE_PATHS.values()}, "steps": []}

    def run(argv, tag, expected=0):
        command = [str(part) for part in argv]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180, env=environment)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "exit": result.returncode, "expected": expected,
                                "seconds": time.monotonic() - started, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: expected {expected}, got {result.returncode}; {log}")
        return result

    def observe(name, result):
        if result.stdout:
            raise RuntimeError(f"{name}: unexpected stdout")
        if name == "native_callback_exception":
            if result.stderr != b"foreign-cleanup\n":
                raise RuntimeError("native exception escaped or skipped foreign cleanup")
        elif name == "native_callback_failure":
            if (not result.stderr.startswith(b"gti-cleanup\n") or b"GTI-R" not in result.stderr or
                    b"division" not in result.stderr.lower() or
                    b"native_callback_failure.gti:" not in result.stderr or
                    b"returned-through-C" in result.stderr):
                raise RuntimeError("callback failure lost cleanup, attribution or terminal containment")
        elif result.stderr:
            raise RuntimeError(f"{name}: unexpected stderr")

    run([args.cc, "--version"], "cc-version")
    run([cxx, "--version"], "cxx-version")
    run([args.emitter.resolve(), root / SOURCE_PATHS["native_callbacks"],
         output / "closure.o", output / "closure.ll", 0, "--native-boundary-reachability"],
        "exact-callback-roots")
    executions = controls = 0
    for host_level in (0, 3):
        host = output / f"native-c{host_level}.o"
        terminal = output / f"terminal-c{host_level}.o"
        run([args.cc, "-std=c17", f"-O{host_level}", "-Wall", "-Wextra", "-Werror", "-c",
             fixture / "native_callbacks.c", "-o", host], host.stem)
        run([cxx, "-std=c++20", f"-O{host_level}", "-Wall", "-Wextra", "-Werror", "-c",
             fixture / "native_callback_terminal.cpp", "-o", terminal], terminal.stem)
        for name, source in SOURCE_PATHS.items():
            expected = 70 if name == "native_callback_failure" else 86 if name == "native_callback_exception" else 0
            obj = host if name == "native_callbacks" else terminal
            for backend, standard, level in [*(('llvm', 'c++20', n) for n in range(4)),
                                             ('cpp', 'c++20', 3), ('cpp', 'c++23', 3)]:
                tag = f"{name}-{backend}-{standard}-O{level}-c{host_level}"
                binary = output / tag
                standard_args = ["--std", standard] if backend == "cpp" else []
                run([compiler, root / source, "--backend", backend, *standard_args,
                     f"-O{level}", "-o", binary, "--", obj], "build-" + tag)
                observe(name, run([binary], "run-" + tag, expected))
                if backend == "llvm":
                    executions += 1
                else:
                    controls += 1
    negatives = 0
    for name, code in (("reject_native_callback_lambda", b"GTI-S2003"),
                       ("reject_native_callback_bodyless", b"GTI-S2076")):
        for backend in ("llvm", "cpp"):
            for level in (0, 3):
                binary = output / f"{name}-{backend}-O{level}"
                sentinel = b"retained callback artifact\n"
                binary.write_bytes(sentinel)
                result = run([compiler, fixture / (name + ".gti"), "--backend", backend,
                              f"-O{level}", "-o", binary], binary.name, 65)
                if code not in result.stderr or binary.read_bytes() != sentinel:
                    raise RuntimeError("callback adaptation widened its semantic eligibility")
                negatives += 1
    report.update({"completed": True, "counts": {"llvm_executions": executions,
                   "cpp_controls": controls, "artifact_negatives": negatives}})
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {executions} public callback executions, {controls} C++ controls; {output}")


if __name__ == "__main__":
    main()
