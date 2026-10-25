#!/usr/bin/env python3
"""Exact declaration/call contracts and optional real GTI/C service execution."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import time


def main():
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
        for source in ("contracts_oracle", "native_calls"):
            obj = output / f"host-{source}-O{c_level}.o"
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
                 "-DGTI_SOURCE_HOST", "-I", repo / "runtime/include", "-c",
                 fixture / f"{source}.c", "-o", obj], f"host-{source}-O{c_level}")
            objects.append(obj)
        for level in range(4):
            for source in (("scalar_calls", "service_math", "native_failure")
                           if args.preflight or args.profile == "scalar"
                           else ("contracts", "service_math", "native_failure")):
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
                if source != "native_failure":
                    if result.stdout or result.stderr:
                        raise RuntimeError(f"unexpected source output: {tag}")
                elif (result.stdout != "mark:1\nmark:99\n" or
                      "division" not in result.stderr.lower() or
                      "native_failure.gti" not in result.stderr):
                    raise RuntimeError(f"native failure cleanup/attribution mismatch: {tag}")
                executions += 1
    scope = ("real GTI-body preflight, public admission unchanged" if args.preflight
             else f"production GTI/C ({args.profile} profile)")
    print(f"PASS: 16 declaration/storage helper executions plus {executions} {scope} getter/service/cleanup executions; {output}")


if __name__ == "__main__":
    main()
