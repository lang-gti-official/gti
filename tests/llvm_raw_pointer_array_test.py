#!/usr/bin/env python3
"""Passive pointer-array storage, unchanged native slots, and safety boundaries."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest
from llvm_place_execution_test import PASSIVE_ARRAYS

SOURCE_PATHS = {
    "ordinary": "tests/fixtures/llvm_raw_pointer_arrays.gti",
    "inert": "tests/fixtures/llvm_raw_pointer_array_inert.gti",
    "bounds": "tests/fixtures/llvm_raw_pointer_array_failure.gti",
    "native": "tests/fixtures/nested_raw_pointer_slots.gti",
}
REJECTIONS = {
    "unsafe": ('int main() { mut int value = 1; int* slots[2] = {&value, nullptr}; return *slots[0]; }', "GTI-S2055"),
    "immutable": ('int main() { mut int value = 1; int* slots[2] = {&value, nullptr}; slots[0] = nullptr; return 0; }', "GTI-S2002"),
    "extent": ('int main() { int* left[2] = {}; int* right[3] = left; return 0; }', "GTI-S2003"),
    "expired": ('int main() { mut int* expired = nullptr; { mut int value = 1; expired = &value; } int* slots[2] = {expired, nullptr}; int* copied[2] = slots; unsafe { return *copied[0]; } }', "GTI-S2101"),
}


def evidence_cases(root):
    return {"primitive": PASSIVE_ARRAYS,
            **{name: (root / path).read_text() for name, path in SOURCE_PATHS.items()}}


def evidence_profiles(root):
    return {"passive-arrays": tuple(evidence_cases(root))}


def evidence_manifest(root):
    return build_manifest("llvm_raw_pointer_array_execution", evidence_cases(root),
                          evidence_profiles(root), SOURCE_PATHS)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    compiler, output = args.compiler.resolve(), args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    cases = evidence_cases(root)
    report = {"head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
              "dirty": bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root)),
              "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(),
              "platform": platform.platform(),
              "sources": evidence_manifest(root), "steps": [],
              "native_sha256": sha256((root / "tests/fixtures/nested_raw_pointer_slots.c").read_bytes()).hexdigest()}

    def run(tag, argv, expected=0):
        command = [str(item) for item in argv]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, timeout=180)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": command, "exit": result.returncode,
                                "seconds": time.monotonic() - started, "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    native = root / SOURCE_PATHS["native"]
    run("native-header", [compiler, native, "--emit-native-header", "-o", output / "native.h"])
    objects = []
    for level in (0, 3):
        obj = output / f"native-O{level}.o"
        run(f"native-C{level}", [args.cc, "-std=c17", f"-O{level}", "-Wall", "-Wextra", "-Werror",
             "-I", output, "-I", root / "runtime/include", "-c", native.with_suffix(".c"), "-o", obj])
        objects.append(obj)
    modes = [("cpp", "c++20", 3), ("cpp", "c++23", 3),
             *(("llvm", None, level) for level in range(4))]
    executions = 0
    for name, source_text in cases.items():
        source = root / SOURCE_PATHS[name] if name in SOURCE_PATHS else output / (name + ".gti")
        if name not in SOURCE_PATHS:
            source.write_text(source_text)
        oracle = None
        for obj in objects if name == "native" else (None,):
            for backend, standard, level in modes:
                tag = f"{name}-{backend}-{standard}-O{level}-{obj.stem if obj else 'no-C'}"
                binary = output / tag
                flags = ["--std", standard] if standard else []
                if name == "inert":
                    flags += ["--raw-pointer-audit=strict"]
                run("build-" + tag, [compiler, source, "--backend", backend, f"-O{level}",
                                     *flags, "-o", binary, *(["--", obj] if obj else [])])
                result = run("run-" + tag, [binary], 70 if name == "bounds" else 0)
                expected = {"ordinary": b"31\narrays-ok\n", "bounds": b"array-cleanup\n"}.get(name, b"")
                if result.stdout != expected:
                    raise RuntimeError(f"{tag}: snapshot/inert-copy/cleanup order mismatch")
                if name == "bounds":
                    line = next(i for i, text in enumerate(source_text.splitlines(), 1)
                                if "slots[index()][later()]" in text)
                    if (f"{source.name}:{line}:".encode() not in result.stderr or
                            b"GTI-R0007" not in result.stderr or b"fixed_array" not in result.stderr):
                        raise RuntimeError(f"{tag}: wrong checked array failure/source")
                elif result.stderr:
                    raise RuntimeError(f"{tag}: unexpected runtime diagnostics")
                observed = result.returncode, result.stdout, result.stderr
                if oracle is not None and oracle != observed:
                    raise RuntimeError(f"{tag}: supported endpoints disagree")
                oracle = observed
                executions += 1
    negatives = 0
    for name, (source_text, code) in REJECTIONS.items():
        source = output / (name + ".gti")
        source.write_text(source_text)
        for backend in ("cpp", "llvm"):
            for level in (0, 3):
                tag = f"reject-{name}-{backend}-{level}"
                binary = output / tag
                sentinel = b"preserved array output\n"
                binary.write_bytes(sentinel)
                result = run(tag, [compiler, source, "--backend", backend,
                                  "--raw-pointer-audit=strict", f"-O{level}", "-o", binary], 65)
                if code.encode() not in result.stderr or binary.read_bytes() != sentinel:
                    raise RuntimeError(f"{tag}: lost source rejection or output preservation")
                negatives += 1
    source = output / "comparison.gti"
    source.write_text('int main() { int* left[2] = {}; int* right[2] = {}; return left == right ? 0 : 1; }')
    for level in range(4):
        binary = output / f"comparison-O{level}"
        binary.write_bytes(b"preserved comparison\n")
        result = run(binary.name, [compiler, source, "--backend", "llvm", f"-O{level}", "-o", binary], 65)
        if (b"GTI-B0003" not in result.stderr or b"Shape.ArrayValueComparison" not in result.stderr or
                binary.read_bytes() != b"preserved comparison\n"):
            raise RuntimeError("array storage incorrectly admitted whole-array comparison")
        negatives += 1
    report.update(executions=executions, negatives=negatives, completed=True)
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {executions} pointer/scalar array executions, {negatives} artifact negatives; {output}")


if __name__ == "__main__":
    main()
