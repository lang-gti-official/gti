#!/usr/bin/env python3
"""Exact declaration/call contracts and optional real GTI/C service execution."""

import argparse
from hashlib import sha256
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest


SCALAR_SOURCES = ("scalar_calls", "scalar_types", "service_math",
                  "native_failure", "direct_order")


def evidence_cases(root: Path) -> dict[str, str]:
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    return {name: (fixture / f"{name}.gti").read_text(encoding="utf-8")
            for name in SCALAR_SOURCES}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return {"direct-imports": SCALAR_SOURCES,
            "native-scalars": ("scalar_types",)}


def evidence_manifest(root: Path) -> dict:
    return build_manifest(
        "llvm_native_c_scalar_execution", evidence_cases(root),
        evidence_profiles(root),
        {name: f"tests/fixtures/llvm_native_c_abi/{name}.gti"
         for name in SCALAR_SOURCES})


def native_enum_imports(repo, compiler, cc, cxx, output, run):
    """Exercise imported integer aliases without admitting dormant C shapes."""
    work = output / "native-enum-imports"
    work.mkdir(parents=True, exist_ok=True)
    example = repo / "examples/native-enum-bindings/native"
    fixture = repo / "tests/fixtures/llvm_native_c_abi"
    bindings = work / "bindings.gti"
    run([compiler, "bindgen", example / "flags.h", "--namespace", "native_flags",
         "-o", bindings], "enum-bindgen")
    source, rejected = work / "main.gti", work / "required-aggregate.gti"
    source.write_bytes((fixture / "native_enum_imports.gti").read_bytes())
    rejected.write_bytes((fixture / "native_enum_aggregate.gti").read_bytes())
    header = work / "native.h"
    run([compiler, source, "--emit-native-header", "-o", header], "enum-header")
    executions = 0
    for c_level in (0, 3):
        obj = work / f"flags-O{c_level}.o"
        run([cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror",
             "-c", example / "flags.c", "-o", obj], f"enum-host-O{c_level}")
        for level in range(4):
            tag = f"enum-llvm{level}-c{c_level}"
            binary = work / tag
            run([compiler, source, "--backend", "llvm", f"-O{level}", "-o", binary,
                 "--", obj], "build-" + tag)
            result = run([binary], "run-" + tag)
            if result.stdout or result.stderr:
                raise RuntimeError(f"imported enum output mismatch: {tag}")
            executions += 1
    for standard in ("c++20", "c++23"):
        for isolated in (False, True):
            probe = work / f"header-{standard}-{isolated}.cpp"
            probe.write_text(
                ("#define GTI_NATIVE_HEADER_NO_SOURCE_NAMES\n" if isolated else "") +
                '#include "native.h"\n')
            run([cxx, f"-std={standard}", "-Werror", "-fsyntax-only", "-I",
                 repo / "runtime/include", probe], "enum-" + probe.stem)
    probe = work / "header.c"
    probe.write_text(
        '#include "native.h"\n'
        '_Static_assert(_Generic(&flags_echo, unsigned long (*)(unsigned long): 1, '
        'default: 0), "wide enum signature");\n'
        '_Static_assert(_Generic(&flags_offset, long (*)(long): 1, default: 0), '
        '"signed enum signature");\n'
        '_Static_assert(_Generic(&flags_write, void (*)(unsigned long *): 1, '
        'default: 0), "enum output signature");\n')
    run([cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-I",
         repo / "runtime/include", probe], "enum-c-header")
    comparator = work / "cpp-aggregate"
    run([compiler, rejected, "--std", "c++23", "-O3", "-o", comparator, "--",
         work / "flags-O3.o"], "enum-build-cpp-aggregate")
    result = run([comparator], "enum-run-cpp-aggregate")
    if result.stdout or result.stderr:
        raise RuntimeError("native enum C++ comparator output mismatch")
    for level in (0, 3):
        destination = work / f"unsupported-aggregate-O{level}"
        sentinel = b"existing native artifact\n"
        destination.write_bytes(sentinel)
        result = run([compiler, rejected, "--backend", "llvm", f"-O{level}",
                      "-o", destination], f"enum-reject-aggregate-O{level}", 65)
        if "GTI-B0003" not in result.stderr or destination.read_bytes() != sentinel:
            raise RuntimeError("unsupported imported aggregate published an artifact")
    return {
        "public_executions": executions, "header_compiles": 5,
        "supported_cpp_comparators": 1, "artifact_negatives": 2,
        "input_sha256": {str(path): sha256(path.read_bytes()).hexdigest()
                         for path in (source, rejected, bindings, header,
                                      example / "flags.h", example / "flags.c")},
    }


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--profile", choices=("scalar", "all"), default="all")
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--runtime", type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if args.preflight and (args.compiler or not args.runtime):
        parser.error("--preflight requires --runtime and excludes --compiler")
    repo = Path(__file__).resolve().parents[1]
    fixture = repo / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {"head": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "cc": args.cc,
        "production_execution": args.compiler is not None,
        "production_profile": args.profile if args.compiler else None,
        "real_source_preflight": args.preflight, "steps": []}

    def run(command, name, expected=0):
        command = [str(item) for item in command]
        started = time.monotonic()
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        log = output / (name + ".log")
        log.write_text(result.stdout + result.stderr)
        evidence["steps"].append({"argv": command, "exit": result.returncode,
                                  "expected": expected,
                                  "seconds": time.monotonic() - started,
                                  "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{name}: exit {result.returncode}, expected {expected}; {log}")
        return result

    run([args.cc, "--version"], "c-version")
    for level in range(4):
        obj = output / f"contracts-O{level}.o"
        run([args.emitter.resolve(), fixture / "contracts.gti", obj,
             output / f"contracts-O{level}.ll", level, "--contracts"], f"emit-O{level}")
        for c_level in (0, 3):
            executable = output / f"contracts-native{level}-c{c_level}"
            tag = executable.name
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 fixture / "contracts_oracle.c", obj, "-o", executable], "link-" + tag)
            result = run([executable], "run-" + tag)
            if result.stdout != "exact native C declaration contracts\n" or result.stderr:
                raise RuntimeError(f"native declaration oracle output mismatch: {tag}")
        storage_obj = output / f"storage-O{level}.o"
        run([args.emitter.resolve(), fixture / "native_calls.gti", storage_obj,
             output / f"storage-O{level}.ll", level, "--storage"], f"emit-storage-O{level}")
        for c_level in (0, 3):
            executable = output / f"storage-native{level}-c{c_level}"
            tag = executable.name
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 "-I", repo / "runtime/include", fixture / "native_calls.c",
                 fixture / "storage_oracle.c", storage_obj, "-o", executable], "link-" + tag)
            result = run([executable], "run-" + tag)
            if result.stdout != "native C foreign storage contracts\n" or result.stderr:
                raise RuntimeError(f"native storage oracle output mismatch: {tag}")
    if not args.compiler and not args.preflight:
        print(f"PASS: 16 declaration/storage helper executions and malformed-contract controls; "
              f"no production GTI body execution claimed; {output}")
        return

    compiler = args.compiler.resolve() if args.compiler else None
    executions = 0
    for c_level in (0, 3):
        objects = []
        for source in ("contracts_oracle", "native_calls", "scalar_types_oracle"):
            obj = output / f"host-{source}-O{c_level}.o"
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 "-DGTI_SOURCE_HOST", "-I", repo / "runtime/include", "-c",
                 fixture / f"{source}.c", "-o", obj], f"host-{source}-O{c_level}")
            objects.append(obj)
        for level in range(4):
            for source in (SCALAR_SOURCES
                           if args.preflight or args.profile == "scalar"
                           else ("contracts", "scalar_types", "service_math", "native_failure")):
                executable = output / f"source-{source}-llvm{level}-c{c_level}"
                tag = executable.name
                if args.preflight:
                    source_obj = output / (tag + ".o")
                    run([args.emitter.resolve(), fixture / f"{source}.gti", source_obj,
                         output / (tag + ".ll"), level, "--source-preflight"], "emit-" + tag)
                    run([args.cxx, source_obj, *objects, args.runtime.resolve(),
                         "-pthread", "-o", executable], "link-" + tag)
                else:
                    run([compiler, fixture / f"{source}.gti", "--backend", "llvm", f"-O{level}",
                         "-o", executable, "--", *objects], "build-" + tag)
                result = run([executable], "run-" + tag,
                             expected=70 if source == "native_failure" else 0)
                if source == "direct_order":
                    if result.stdout != "mark:1\nmark:2\nentered:1,2\nmark:99\n" or result.stderr:
                        raise RuntimeError(f"staged native evaluation/cleanup mismatch: {tag}")
                elif source != "native_failure":
                    if result.stdout or result.stderr:
                        raise RuntimeError(f"unexpected source output: {tag}")
                elif (result.stdout != "mark:1\nmark:99\n" or
                      "division" not in result.stderr.lower() or
                      "native_failure.gti" not in result.stderr):
                    raise RuntimeError(f"native failure cleanup/attribution mismatch: {tag}")
                executions += 1
    if compiler:
        imported = native_enum_imports(repo, compiler, args.cc, args.cxx, output, run)
        executions += imported["public_executions"]
        evidence["native_enum_imports"] = imported
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
    scope = ("real GTI-body preflight, public admission unchanged" if args.preflight
             else f"production GTI/C ({args.profile} profile)")
    print(f"PASS: 16 declaration/storage helper executions plus {executions} {scope} getter/service/cleanup executions; {output}")


if __name__ == "__main__":
    main()
