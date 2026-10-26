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

FIXTURE_SOURCES = ("contracts", "native_indirect", "native_indirect_order",
                   "native_indirect_failure", "native_indirect_roots")
EXAMPLE = "native_c_function_values"
SOURCE_PATHS = {name: f"tests/fixtures/llvm_native_c_abi/{name}.gti"
                for name in FIXTURE_SOURCES}
SOURCE_PATHS[EXAMPLE] = "examples/llvm/native-c-function-values.gti"
SOURCES = tuple(SOURCE_PATHS)


def evidence_cases(root):
    return {name: (root / path).read_text() for name, path in SOURCE_PATHS.items()}


def evidence_profiles(root):
    del root
    return {profile: SOURCES for profile in
            ("function-values", "function-imports", "scalar-indirect")}


def evidence_manifest(root):
    return build_manifest(
        "llvm_native_c_indirect_execution", evidence_cases(root),
        evidence_profiles(root),
        SOURCE_PATHS)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--emitter", type=Path, required=True)
    parser.add_argument("--focus", choices=("reachability", "example"))
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    fixture = root / "tests/fixtures/llvm_native_c_abi"
    dormant = (fixture / "native_indirect_roots.gti").read_text()
    required = (fixture / "reject_native_indirect_required.gti").read_text()
    dormant_edge = "  if (required_dependency(8) != 9) return 3;"
    required_edge = "  if (dormant_unsupported() != 9) return 3;"
    if (dormant.count(dormant_edge) != 1 or
            dormant.replace(dormant_edge, required_edge) != required):
        raise RuntimeError("dormant/required fixtures must differ only in their main-call edge")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    report = {"head": subprocess.check_output(
        ["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "focus": args.focus,
        "reachability_pair": {"only_change": [dormant_edge, required_edge],
                              "equal_after_edge_replacement": True}, "steps": []}

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
        elif source == EXAMPLE:
            expected = b"selected\nargument:31\nresult:42\ncleanup\n"
            if result.stdout != expected or result.stderr:
                raise RuntimeError("native function-value example selection/result/cleanup mismatch")
        elif source == "native_indirect_roots":
            if result.stdout != b"mark:99\n" or result.stderr:
                raise RuntimeError("native exact roots or typed cleanup mismatch")
        elif result.stdout or result.stderr:
            raise RuntimeError("unexpected native scalar indirect output")

    run([args.cc, "--version"], "c-version")
    run([args.cxx, "--version"], "cxx-version")
    root_checks = (
            ("native_indirect_roots", "--reachability"),
            ("reject_native_indirect", "--conservative-reachability"),
            ("reject_native_indirect_callback", "--conservative-reachability"))
    if args.focus == "example":
        root_checks = ()
    for source, mode in root_checks:
        run([args.emitter.resolve(), fixture / (source + ".gti"),
             output / (source + ".o"), output / (source + ".ll"), 0, mode],
            "roots-" + source)
    executions = controls = negatives = 0
    sources = (("native_indirect_roots",) if args.focus == "reachability" else
               (EXAMPLE,) if args.focus == "example" else SOURCES)
    levels = (0, 3) if args.focus == "reachability" else range(4)
    for c_level in (0, 3):
        objects = []
        for source in ("contracts_oracle", "native_indirect"):
            obj = output / f"{source}-c{c_level}.o"
            run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror",
                 "-DGTI_SOURCE_HOST", "-c", fixture / (source + ".c"), "-o", obj],
                f"host-{source}-c{c_level}")
            objects.append(obj)
        example_object = output / f"example-c{c_level}.o"
        if EXAMPLE in sources:
            run([args.cc, "-std=c17", f"-O{c_level}", "-Wall", "-Wextra", "-Werror",
                 "-c", root / "examples/llvm/native-c-function-values-native/operation.c",
                 "-o", example_object], f"host-example-c{c_level}")
        oracles = {}
        for source in sources:
            source_path = root / SOURCE_PATHS[source]
            source_objects = [example_object] if source == EXAMPLE else objects
            for standard in ("c++20", "c++23"):
                tag = f"{source}-{standard}-c{c_level}"
                binary = output / tag
                run([compiler, source_path, "--std", standard,
                     "-O3", "-o", binary, "--", *source_objects], "build-" + tag)
                result = run([binary], "run-" + tag,
                             70 if source == "native_indirect_failure" else 0)
                observe(source, result)
                controls += 1
                value = (result.stdout, result.stderr)
                if source in oracles and oracles[source] != value:
                    raise RuntimeError("supported C++ comparator standards disagree")
                oracles[source] = value
            for level in levels:
                tag = f"{source}-llvm{level}-c{c_level}"
                binary = output / tag
                run([compiler, source_path, "--backend", "llvm",
                     f"-O{level}", "-o", binary, "--", *source_objects], "build-" + tag)
                result = run([binary], "run-" + tag,
                             70 if source == "native_indirect_failure" else 0)
                observe(source, result)
                if (result.stdout, result.stderr) != oracles[source]:
                    raise RuntimeError("native indirect execution differs from supported C++ comparator")
                executions += 1
    rejected = fixture / "reject_native_indirect.gti"
    for standard in (() if args.focus else ("c++20", "c++23")):
        binary = output / f"aggregate-indirect-{standard}"
        run([compiler, rejected, "--std", standard, "-O3", "-o", binary,
             "--", *objects], "build-" + binary.name)
        result = run([binary], "run-" + binary.name)
        controls += 1
        if result.stdout or result.stderr:
            raise RuntimeError("supported aggregate C++ comparator output mismatch")
    rejected_sources = ("reject_native_indirect_required", "reject_native_indirect_callback")
    if not args.focus:
        rejected_sources = ("reject_native_indirect", *rejected_sources)
    elif args.focus == "example":
        rejected_sources = ()
    for source in rejected_sources:
        for level in (0, 3):
            destination = output / f"{source}-O{level}"
            sentinel = b"existing native artifact\n"
            destination.write_bytes(sentinel)
            result = run([compiler, fixture / (source + ".gti"), "--backend", "llvm",
                          f"-O{level}", "-o", destination], destination.name, 65)
            if b"GTI-B0003" not in result.stderr or destination.read_bytes() != sentinel:
                raise RuntimeError("unsupported required native boundary replaced an artifact")
            if source == "reject_native_indirect_required":
                if (b"MirDefinitionKind.Declaration (O-LLVM-03)" not in result.stderr or
                        b"reject_native_indirect_required.gti:4:11:" not in result.stderr or
                        b"consume_record" not in result.stderr):
                    raise RuntimeError("required body must reject at its unchanged native aggregate import")
            if source == "reject_native_indirect_callback":
                if (b"MirOperation.NativeCallback (O-LLVM-11)" not in result.stderr or
                        b"reject_native_indirect_callback.gti:4:5:" not in result.stderr):
                    raise RuntimeError("source-created callback must retain its original pending boundary")
            negatives += 1
    rejections = (
        ("reject_native_indirect_unsafe", b"GTI-S2055", b":6:21:"),
        ("reject_native_indirect_signature", b"GTI-S2003", b":5:46:"),
    )
    if args.focus:
        rejections = ()
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
            negatives += 1
    report["totals"] = {"llvm_executions": executions, "cpp_controls": controls,
                        "root_checks": len(root_checks), "artifact_negatives": negatives}
    (output / "evidence.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS: {executions} public LLVM native-function executions, {controls} C++ controls, {len(root_checks)} exact-root checks and {negatives} artifact negatives; {output}")


if __name__ == "__main__":
    main()
