#!/usr/bin/env python3
"""Public native C global storage and exact admission/permission boundaries."""
import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest

SOURCE_PATHS = {name: f"tests/fixtures/llvm_native_c_abi/{name}.gti" for name in
                ("native_storage", "native_storage_controls", "native_storage_failure", "native_storage_opaque",
                 "native_storage_union_member")}
SOURCE_PATHS.update({"storage_original": "tests/fixtures/llvm_native_c_abi/reject_foreign_storage.gti",
                     "storage_example": "examples/llvm/native-c-storage.gti"})
PREREQUISITES = ("native_storage_opaque", "native_storage_union_member")


def evidence_cases(root):
    return {name: (root / path).read_text() for name, path in SOURCE_PATHS.items()}


def evidence_profiles(root):
    del root
    return {"passive-storage": tuple(SOURCE_PATHS)}


def evidence_manifest(root):
    return build_manifest("llvm_native_c_storage_execution", evidence_cases(root),
                          evidence_profiles(root), SOURCE_PATHS)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--focus", choices=("independent", "prerequisites"))
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler, emitter = args.compiler.resolve(), args.emitter.resolve()
    cxx = shutil.which(args.cxx) or args.cxx
    environment = {**os.environ, "GTI_CXX": cxx, "CXX": cxx}
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "platform": platform.platform(), "cc": args.cc, "cxx": cxx,
              "focus": args.focus, "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(),
              "input_sha256": {path: sha256((root / path).read_bytes()).hexdigest()
                               for path in SOURCE_PATHS.values()}, "steps": []}

    def run(argv, tag, expected=0):
        command = [str(part) for part in argv]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180, env=environment)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "exit": result.returncode,
                                "seconds": time.monotonic() - started, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: expected {expected}, got {result.returncode}; {log}")
        return result

    def observe(name, result):
        expected = {"native_storage": b"swap\nentered-original:2\nentered-replaced:2\nstorage-cleanup\n",
                    "native_storage_controls": b"swap\nentered-original:2\nentered-replaced:2\nstorage-cleanup\n",
                    "native_storage_failure": b"zero\nstorage-cleanup\n",
                    "storage_example": b"native-state:13,31\ncleanup\n"}.get(name, b"")
        if result.stdout != expected:
            raise RuntimeError(f"{name}: value/evaluation/cleanup trace mismatch")
        if name == "native_storage_failure":
            source = root / SOURCE_PATHS[name]
            line = next(i for i, text in enumerate(source.read_text().splitlines(), 1)
                        if "1 / native_storage_zero()" in text)
            if f"{source.name}:{line}:".encode() not in result.stderr or b"division" not in result.stderr.lower():
                raise RuntimeError("foreign load lost original later-argument failure site")
        elif result.stderr:
            raise RuntimeError(f"{name}: unexpected diagnostic output")

    run([args.cc, "--version"], "c-version")
    run([cxx, "--version"], "cxx-version")
    if args.focus != "prerequisites":
        for level in range(4):
            ir = output / f"storage-contracts-O{level}.ll"
            run([emitter, fixture / "native_storage.gti", output / f"storage-contracts-O{level}.o",
                 ir, level, "--storage-contracts"], f"contracts-O{level}")
            for line in ir.read_text().splitlines():
                if line.startswith("@native_") and (" = external global " not in line or ", align " not in line):
                    raise RuntimeError("foreign storage became initialized/constant/unaligned GTI data")
    for name in PREREQUISITES:
        if args.focus == "independent":
            continue
        run([emitter, fixture / (name + ".gti"), output / (name + "-contracts.o"),
             output / (name + "-contracts.ll"), 0, "--storage-contracts"],
            name + "-contracts")
    sources = tuple(name for name in SOURCE_PATHS
                    if (args.focus != "independent" or name not in PREREQUISITES) and
                    (args.focus != "prerequisites" or name in PREREQUISITES))
    executions = controls = negatives = 0
    cpp_unsupported = []
    for c_level in (0, 3):
        host = output / f"host-c{c_level}.o"
        run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror", "-c",
             fixture / "native_storage.c", "-o", host], f"host-c{c_level}")
        example = output / f"example-c{c_level}.o"
        if "storage_example" in sources:
            run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror", "-c",
                 root / "examples/llvm/native-c-storage-native/state.c", "-o", example],
                f"example-c{c_level}")
        for name in sources:
            source = root / SOURCE_PATHS[name]
            objects = [example] if name == "storage_example" else [host]
            expected_exit = 70 if name == "native_storage_failure" else 0
            oracle = None
            for standard in ("c++20", "c++23"):
                tag = f"{name}-{standard}-c{c_level}"
                binary = output / tag
                # Frozen pre-storage767680c reproduces these existing C++ text
                # vocabulary limits: foreign checked-update, projected record
                # snapshot and record-array element assignment. The unchanged
                # source always executes on LLVM, with independent native-C
                # state checks plus semantic/MIR and supported C++ controls.
                if name == "native_storage":
                    sentinel = b"existing C++ comparison artifact\n"
                    binary.write_bytes(sentinel)
                    result = run([compiler, source, "--backend", "cpp", "--std", standard,
                                  "-O3", "-o", binary, "--", *objects], "build-" + tag, 65)
                    match = re.search(rb"backend diagnostics saved at ([^\n]+)", result.stderr)
                    if b"GTI-B0001" not in result.stderr or not match or binary.read_bytes() != sentinel:
                        raise RuntimeError("C++ comparator limitation changed; investigate rather than skip")
                    diagnostic = Path(os.fsdecode(match[1])).read_bytes()
                    if b"verified MIR body is outside the complete C++ text vocabulary" not in diagnostic:
                        raise RuntimeError("unexpected C++ failure must not be classified as the known limitation")
                    retained = output / (tag + "-backend.log")
                    retained.write_bytes(diagnostic)
                    cpp_unsupported.append({"case": tag, "diagnostic": "GTI-B0001/text-vocabulary",
                                            "log": str(retained)})
                    continue
                run([compiler, source, "--backend", "cpp", "--std", standard, "-O3", "-o",
                     binary, "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag, expected_exit)
                observe(name, result)
                if oracle is not None and oracle != (result.stdout, result.stderr):
                    raise RuntimeError("supported C++ storage comparator standards disagree")
                oracle = result.stdout, result.stderr
                controls += 1
            for level in range(4):
                tag = f"{name}-llvm{level}-c{c_level}"
                binary = output / tag
                run([compiler, source, "--backend", "llvm", f"-O{level}", "-o", binary,
                     "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag, expected_exit)
                observe(name, result)
                if oracle is not None and (result.stdout, result.stderr) != oracle:
                    raise RuntimeError("native storage execution differs from supported C++ control")
                executions += 1
    if args.focus != "prerequisites":
        for name in ("reject_native_storage_function_field", "reject_native_storage_aggregate_function"):
            for standard in ("c++20", "c++23"):
                binary = output / f"{name}-{standard}"
                run([compiler, fixture / (name + ".gti"), "--backend", "cpp", "--std", standard,
                     "-O3", "-o", binary], "build-" + binary.name)
                run([binary], "run-" + binary.name)
                controls += 1
            for level in range(4):
                binary = output / f"{name}-O{level}"
                sentinel = b"existing storage artifact\n"
                binary.write_bytes(sentinel)
                result = run([compiler, fixture / (name + ".gti"), "--backend", "llvm", f"-O{level}",
                              "-o", binary], binary.name, 65)
                if b"GTI-B0003" not in result.stderr or binary.read_bytes() != sentinel:
                    raise RuntimeError("closed foreign storage shape published an artifact")
                negatives += 1
        for backend in ("llvm", "cpp"):
            for level in (0, 3):
                name = f"pointer-permission-{backend}-O{level}"
                binary = output / name
                sentinel = b"existing permission artifact\n"
                binary.write_bytes(sentinel)
                result = run([compiler, fixture / "reject_native_storage_pointer_permission.gti",
                              "--backend", backend, f"-O{level}", "-o", binary], name, 65)
                if (b"GTI-S2055" not in result.stderr or
                        b"Raw pointer dereference requires an unsafe block" not in result.stderr or
                        b":5:10:" not in result.stderr or binary.read_bytes() != sentinel):
                    raise RuntimeError("foreign pointer load authorized a later unchecked dereference")
                negatives += 1
    report.update(llvm_executions=executions, cpp_controls=controls,
                  artifact_negatives=negatives, cpp_known_unsupported=cpp_unsupported, completed=True)
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {executions} native storage executions, {controls} C++ controls, "
          f"{negatives} artifact negatives, {len(cpp_unsupported)} documented C++ rejections; {output / 'evidence.json'}")


if __name__ == "__main__":
    main()
