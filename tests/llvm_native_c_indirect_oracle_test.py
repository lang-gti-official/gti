#!/usr/bin/env python3
"""Production scalar native-function values, staged calls and closed boundaries."""
import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest

SOURCES = ("contracts", "native_indirect", "native_indirect_order",
           "native_indirect_failure")


def evidence_cases(root):
    return {name: (root / "tests/fixtures/llvm_native_c_abi" /
                   (name + ".gti")).read_text() for name in SOURCES}


def evidence_manifest(root):
    return build_manifest(
        "llvm_native_c_indirect_execution", evidence_cases(root),
        {profile: SOURCES for profile in
         ("function-values", "function-imports", "scalar-indirect")},
        {name: f"tests/fixtures/llvm_native_c_abi/{name}.gti" for name in SOURCES})


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    report = {"head": subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "steps": []}

    def run(argv, tag, expected=0):
        argv = [str(part) for part in argv]
        started = time.monotonic()
        result = subprocess.run(argv, capture_output=True, timeout=180)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": argv, "exit": result.returncode,
                                "expected": expected,
                                "seconds": time.monotonic() - started,
                                "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    def observe(source, result):
        if source == "native_indirect_order":
            expected = b"selected\nmark:1\nmark:2\nentered:1,2\nmark:99\n"
            if result.stdout != expected or result.stderr:
                raise RuntimeError("native receiver/argument staging or cleanup mismatch")
        elif source == "native_indirect_failure":
            line = next(index for index, text in enumerate(
                (fixture / (source + ".gti")).read_text().splitlines(), 1)
                        if "1 / native_zero()" in text)
            site = f"{source}.gti:{line}:".encode()
            if (result.stdout != b"selected\nmark:1\nmark:99\n" or
                    b"division" not in result.stderr.lower() or site not in result.stderr):
                raise RuntimeError("native later-argument failure/cleanup attribution mismatch")
        elif result.stdout or result.stderr:
            raise RuntimeError("unexpected native scalar indirect output")

    run([args.cc, "--version"], "c-version")
    run([args.cxx, "--version"], "cxx-version")
    executions = 0
    for c_level in (0, 3):
        objects = []
        for source in ("contracts_oracle", "native_indirect"):
            obj = output / f"{source}-c{c_level}.o"
            run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror",
                 "-DGTI_SOURCE_HOST", "-c", fixture / (source + ".c"), "-o", obj],
                f"host-{source}-c{c_level}")
            objects.append(obj)
        oracles = {}
        for source in SOURCES:
            for standard in ("c++20", "c++23"):
                tag = f"{source}-{standard}-c{c_level}"
                binary = output / tag
                run([compiler, fixture / (source + ".gti"), "--std", standard,
                     "-O3", "-o", binary, "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag,
                             70 if source == "native_indirect_failure" else 0)
                observe(source, result)
                value = (result.stdout, result.stderr)
                if source in oracles and oracles[source] != value:
                    raise RuntimeError("supported C++ comparator standards disagree")
                oracles[source] = value
            for level in range(4):
                tag = f"{source}-llvm{level}-c{c_level}"
                binary = output / tag
                run([compiler, fixture / (source + ".gti"), "--backend", "llvm",
                     f"-O{level}", "-o", binary, "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag,
                             70 if source == "native_indirect_failure" else 0)
                observe(source, result)
                if (result.stdout, result.stderr) != oracles[source]:
                    raise RuntimeError("native indirect execution differs from supported C++ comparator")
                executions += 1
    rejected = fixture / "reject_native_indirect.gti"
    for standard in ("c++20", "c++23"):
        binary = output / f"aggregate-indirect-{standard}"
        run([compiler, rejected, "--std", standard, "-O3", "-o", binary,
             "--", *objects], "build-" + binary.name)
        result = run([binary], "run-" + binary.name)
        if result.stdout or result.stderr:
            raise RuntimeError("supported aggregate C++ comparator output mismatch")
    for level in (0, 3):
        destination = output / f"reject-aggregate-indirect-O{level}"
        sentinel = b"existing native artifact\n"
        destination.write_bytes(sentinel)
        result = run([compiler, rejected, "--backend", "llvm", f"-O{level}",
                      "-o", destination], destination.name, 65)
        if b"GTI-B0003" not in result.stderr or destination.read_bytes() != sentinel:
            raise RuntimeError("unsupported aggregate indirect call replaced an artifact")
    rejections = (
        ("reject_native_indirect_unsafe", b"GTI-S2055", b":6:21:"),
        ("reject_native_indirect_signature", b"GTI-S2003", b":5:46:"),
    )
    for source, code, site in rejections:
        for backend in ("cpp", "llvm"):
            destination = output / f"{source}-{backend}"
            sentinel = b"existing native artifact\n"
            destination.write_bytes(sentinel)
            result = run([compiler, fixture / (source + ".gti"), "--backend", backend,
                          "-O3", "-o", destination], destination.name, 65)
            if (code not in result.stderr or site not in result.stderr or
                    (source + ".gti").encode() not in result.stderr or
                    b"GTI-B0003" in result.stderr or
                    destination.read_bytes() != sentinel):
                raise RuntimeError("native source rejection or artifact preservation mismatch")
    print(f"PASS: {executions} public LLVM native-function executions, 18 C++ controls and 6 artifact negatives; {output}")


if __name__ == "__main__":
    main()
