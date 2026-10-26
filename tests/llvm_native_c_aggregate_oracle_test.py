#!/usr/bin/env python3
"""Public passive C aggregate calls, exact source staging and closed boundaries."""
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

SOURCE_PATHS = {
    name: f"tests/fixtures/llvm_native_c_abi/{name}.gti" for name in
    ("native_records", "native_record_order", "native_record_failure")
}
# Former admission rejections now verify actual returned values on both backends.
SOURCE_PATHS.update({
    "record_argument": "tests/fixtures/llvm_native_c_abi/reject_record_argument.gti",
    "record_result": "tests/fixtures/llvm_native_c_abi/reject_record_result.gti",
    "record_example": "examples/llvm/native-c-record-values.gti",
    **{name: f"tests/fixtures/llvm_native_c_abi/{name}.gti" for name in
       ("native_record_owned_view", "native_record_owned_view_failure")},
})
OWNED = ("native_record_owned_view", "native_record_owned_view_failure")


def evidence_cases(root):
    return {name: (root / path).read_text() for name, path in SOURCE_PATHS.items()}


def evidence_profiles(root):
    del root
    return {"passive-imports": tuple(SOURCE_PATHS)}


def evidence_manifest(root):
    return build_manifest("llvm_native_c_aggregate_execution", evidence_cases(root),
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
    parser.add_argument("--focus", choices=("independent", "owned"))
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler, emitter = args.compiler.resolve(), args.emitter.resolve()
    cxx = shutil.which(args.cxx) or args.cxx
    environment = {**os.environ, "GTI_CXX": cxx, "CXX": cxx}
    report = {"head": subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "dirty": bool(subprocess.check_output(
            ["git", "-C", str(root), "status", "--porcelain"], text=True)),
        "platform": platform.platform(), "cc": args.cc, "cxx": cxx,
        "focus": args.focus, "compiler_sha256": sha256(compiler.read_bytes()).hexdigest(),
        "input_sha256": {path: sha256((root / path).read_bytes()).hexdigest()
                         for path in SOURCE_PATHS.values()}, "steps": []}

    def save():
        (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(argv, tag, expected=0):
        argv = [str(part) for part in argv]
        started = time.monotonic()
        result = subprocess.run(argv, capture_output=True, timeout=180, env=environment)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        report["steps"].append({"argv": argv, "exit": result.returncode,
                                "expected": expected, "seconds": time.monotonic() - started,
                                "log": str(log)})
        save()
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    def expected_exit(name):
        if name in ("native_record_failure", "native_record_owned_view_failure"):
            return 70
        return 41 if name in ("record_argument", "record_result") else 0

    def observe(name, result):
        expected = {
            "native_record_order": b"record:1\nrecord:2\nentered:1,2\nmark:99\n",
            "native_record_failure": b"record:1\nmark:99\n",
            "native_record_owned_view": b"record:1\nmark:2\nmark:3\nentered-view:1,6,3\nmark:99\n",
            "native_record_owned_view_failure": b"record:1\nmark:2\nmark:99\n",
            "record_example": b"records:7,8\ncleanup\n",
        }.get(name, b"")
        if result.stdout != expected:
            raise RuntimeError(f"{name}: source order/value/cleanup mismatch")
        if expected_exit(name) == 70:
            source = root / SOURCE_PATHS[name]
            line = next(i for i, text in enumerate(source.read_text().splitlines(), 1)
                        if "1 / native_record_zero()" in text)
            if (f"{source.name}:{line}:".encode() not in result.stderr or
                    b"division" not in result.stderr.lower()):
                raise RuntimeError(f"{name}: original later-argument failure site lost")
        elif result.stderr:
            raise RuntimeError(f"{name}: unexpected diagnostic output")

    run([args.cc, "--version"], "c-version")
    run([cxx, "--version"], "cxx-version")
    if args.focus != "owned":
        for level in range(4):
            run([emitter, fixture / "native_records.gti", output / f"contracts-O{level}.o",
                 output / f"contracts-O{level}.ll", level, "--aggregate-contracts"],
                f"contracts-O{level}")
        header = output / "native-records.h"
        run([compiler, fixture / "native_records.gti", "--emit-native-header", "-o", header],
            "native-header")
        for standard in ("c++20", "c++23"):
            for isolated in (False, True):
                source = output / f"header-{standard}-{isolated}.cpp"
                source.write_text(("#define GTI_NATIVE_HEADER_NO_SOURCE_NAMES\n" if isolated else "") +
                                  '#include "native-records.h"\n')
                run([cxx, f"-std={standard}", "-Werror", "-fsyntax-only", "-I",
                     root / "runtime/include", source], source.stem)
        source = output / "header-c.c"
        source.write_text('#include "native-records.h"\n')
        run([args.cc, "-std=c17", "-Werror", "-fsyntax-only", "-I",
             root / "runtime/include", source], source.stem)
    sources = tuple(name for name in SOURCE_PATHS
                    if (args.focus != "owned" or name in OWNED) and
                    (args.focus != "independent" or name not in OWNED))
    executions = controls = negatives = 0
    for c_level in (0, 3):
        host = output / f"host-c{c_level}.o"
        run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror", "-I",
             root / "runtime/include", "-c", fixture / "native_records.c", "-o", host],
            f"host-c{c_level}")
        example = output / f"example-c{c_level}.o"
        if "record_example" in sources:
            run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror", "-c",
                 root / "examples/llvm/native-c-record-values-native/records.c", "-o", example],
                f"example-host-c{c_level}")
        for name in sources:
            path = root / SOURCE_PATHS[name]
            objects = [example] if name == "record_example" else [host]
            oracle = None
            for standard in ("c++20", "c++23"):
                tag = f"{name}-{standard}-c{c_level}"
                binary = output / tag
                run([compiler, path, "--backend", "cpp", "--std", standard, "-O3", "-o",
                     binary, "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag, expected_exit(name))
                observe(name, result)
                if oracle is not None and oracle != (result.stdout, result.stderr):
                    raise RuntimeError("supported C++ comparator standards disagree")
                oracle = result.stdout, result.stderr
                controls += 1
            for level in range(4):
                tag = f"{name}-llvm{level}-c{c_level}"
                binary = output / tag
                run([compiler, path, "--backend", "llvm", f"-O{level}", "-o", binary,
                     "--", *objects], "build-" + tag)
                result = run([binary], "run-" + tag, expected_exit(name))
                observe(name, result)
                if (result.stdout, result.stderr) != oracle:
                    raise RuntimeError("native aggregate execution differs from its C++ comparator")
                executions += 1
    if args.focus != "owned":
        for name in ("reject_native_record_function_field", "reject_native_record_function_input"):
            for standard in ("c++20", "c++23"):
                binary = output / f"{name}-{standard}"
                run([compiler, fixture / (name + ".gti"), "--backend", "cpp", "--std", standard,
                     "-O3", "-o", binary, "--", host], "build-" + binary.name)
                result = run([binary], "run-" + binary.name)
                if result.stdout or result.stderr:
                    raise RuntimeError("supported callback-field/input C++ comparator mismatch")
                controls += 1
            for level in (0, 3):
                destination = output / f"{name}-O{level}"
                run([compiler, fixture / (name + ".gti"), "--backend", "llvm",
                     f"-O{level}", "-o", destination, "--", host], "build-" + destination.name)
                result = run([destination], "run-" + destination.name)
                if result.stdout or result.stderr:
                    raise RuntimeError("native function field/input transport mismatch")
                executions += 1
        for name, code, site in (
                ("reject_native_record_mutable_view", b"GTI-S2054", b":3:80:"),
                ("reject_native_record_view_return", b"GTI-S2054", b":2:19:"),
                ("reject_native_record_view_mutation", b"GTI-S2017", b":12:72:")):
            for backend in ("cpp", "llvm"):
                destination = output / f"{name}-{backend}"
                sentinel = b"existing native artifact\n"
                destination.write_bytes(sentinel)
                result = run([compiler, fixture / (name + ".gti"), "--backend", backend,
                              "-O3", "-o", destination], destination.name, 65)
                if (code not in result.stderr or site not in result.stderr or
                        (name + ".gti").encode() not in result.stderr or
                        b"GTI-B0003" in result.stderr or destination.read_bytes() != sentinel):
                    raise RuntimeError("view contract rejection/borrow endpoint/artifact mismatch")
                negatives += 1
    report["totals"] = {"llvm_executions": executions, "cpp_controls": controls,
                        "artifact_negatives": negatives}
    save()
    print(f"PASS: {executions} public LLVM aggregate executions, {controls} C++ controls, {negatives} artifact negatives; {output}")


if __name__ == "__main__":
    main()
