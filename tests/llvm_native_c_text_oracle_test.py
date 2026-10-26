#!/usr/bin/env python3
"""Public counted C record transport; production GTI execution is opt-in."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import sys
import time

from llvm_evidence_manifest import build_manifest


TEXT_SOURCES = {
    "counted_text": "tests/fixtures/llvm_native_c_abi/counted_text.gti",
    "counted_stdout_failure": "tests/fixtures/llvm_native_c_abi/counted_stdout_failure.gti",
    "basics": "examples/001-basics.gti",
}


def evidence_cases(root: Path) -> dict[str, str]:
    return {name: (root / path).read_text(encoding="utf-8")
            for name, path in TEXT_SOURCES.items()}


def evidence_profiles(root: Path) -> dict[str, tuple[str, ...]]:
    del root
    return {"counted-inputs": tuple(TEXT_SOURCES)}


def evidence_manifest(root: Path) -> dict:
    return build_manifest("llvm_native_c_counted_input", evidence_cases(root),
                          evidence_profiles(root), TEXT_SOURCES)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--list-evidence":
        print(json.dumps(evidence_manifest(Path(sys.argv[2])), sort_keys=True))
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--cxx", default="clang++")
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--preflight", action="store_true")
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    if args.preflight and args.compiler:
        parser.error("--preflight and --compiler are separate evidence modes")
    repo = Path(__file__).resolve().parents[1]
    fixture = repo / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {"head": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "production_execution": bool(args.compiler),
        "real_source_preflight": args.preflight, "steps": []}

    def run(argv, tag, expected=0):
        argv = [str(item) for item in argv]
        started = time.monotonic()
        result = subprocess.run(argv, capture_output=True, timeout=180)
        log = output / (tag + ".log")
        log.write_bytes(result.stdout + result.stderr)
        evidence["steps"].append({"argv": argv, "exit": result.returncode,
                                  "expected": expected,
                                  "seconds": time.monotonic() - started,
                                  "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        if result.returncode != expected:
            raise RuntimeError(f"{tag}: exit {result.returncode}, expected {expected}; {log}")
        return result

    run([args.cc, "--version"], "c-version")
    run([args.cxx, "--version"], "cxx-version")
    for level in range(4):
        obj = output / f"counted-O{level}.o"
        run([args.emitter.resolve(), fixture / "counted_text.gti", obj,
             output / f"counted-O{level}.ll", level, "--text-contracts"],
            f"emit-O{level}")
        for c_level in (0, 3):
            tag = f"llvm{level}-c{c_level}"
            c_obj, executable = output / f"c-{tag}.o", output / f"helper-{tag}"
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 "-I", repo / "runtime/include", "-c",
                 fixture / "counted_text_oracle.c", "-o", c_obj], "c-" + tag)
            run([args.cxx, obj, c_obj, args.runtime.resolve(), "-pthread", "-o",
                 executable], "link-" + tag)
            result = run([executable], "run-" + tag)
            if result.stdout != b"A\0B\xc3\xa9\xce\xbb" or result.stderr:
                raise RuntimeError(f"counted C record/runtime stdout bytes mismatch: {tag}")
            result = run([executable, "write-failure"], "failure-" + tag)
            if result.stdout or result.stderr:
                raise RuntimeError(f"counted runtime failure status mismatch: {tag}")
            if not args.preflight and not args.compiler:
                continue
            host = output / f"host-{tag}.o"
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 "-DGTI_SOURCE_HOST", "-I", repo / "runtime/include", "-c",
                 fixture / "counted_text_oracle.c", "-o", host], "host-" + tag)
            for source, source_path in TEXT_SOURCES.items():
                name = f"source-{source}-{tag}"
                target = output / name
                if args.preflight:
                    source_obj = output / (name + ".o")
                    run([args.emitter.resolve(), repo / source_path, source_obj,
                         output / (name + ".ll"), level, "--source-preflight"], "emit-" + name)
                    run([args.cxx, source_obj, host, args.runtime.resolve(), "-pthread",
                         "-o", target], "link-" + name)
                else:
                    run([args.compiler.resolve(), repo / source_path, "--backend",
                         "llvm", f"-O{level}", "-o", target, "--", host], "build-" + name)
                failure = source == "counted_stdout_failure"
                result = run([target], "run-" + name, expected=70 if failure else 0)
                if failure:
                    if (result.stdout or result.stderr.count(b"counted-cleanup\n") != 1 or
                            b"stdout write failed" not in result.stderr or
                            b"counted_stdout_failure.gti:9:" not in result.stderr):
                        raise RuntimeError(f"source stdout status/panic cleanup mismatch: {name}")
                else:
                    expected = (b"GTI values are typed and immutable by default\n"
                                if source == "basics" else b"A\0B\xc3\xa9\xce\xbb\n")
                    if result.stdout != expected or result.stderr:
                        raise RuntimeError(f"source counted stdout mismatch: {name}")
    if args.compiler:
        for source in ("reject_native_record_function_field", "reject_native_record_function_input",
                       "reject_foreign_storage", "reject_native_indirect"):
            for level in (0, 3):
                destination = output / f"{source}-O{level}"
                sentinel = b"existing native artifact\n"
                destination.write_bytes(sentinel)
                result = run([args.compiler.resolve(), fixture / f"{source}.gti",
                              "--backend", "llvm", f"-O{level}", "-o", destination],
                             destination.name, expected=65)
                if b"GTI-B0003" not in result.stderr or destination.read_bytes() != sentinel:
                    raise RuntimeError(f"unsupported native boundary published an artifact: {source}")
    scope = ("production GTI source" if args.compiler else
             "real-source preflight, public admission unchanged" if args.preflight else
             "helper only; no GTI source-body or admission proof")
    print(f"PASS: counted C input/runtime stdout matrix; {scope}; {output}")


if __name__ == "__main__":
    main()
