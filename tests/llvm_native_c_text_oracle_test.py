#!/usr/bin/env python3
"""Public counted C record transport; production GTI execution is opt-in."""

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
            for source in ("counted_text", "counted_stdout_failure"):
                name = f"source-{source}-{tag}"
                target = output / name
                if args.preflight:
                    source_obj = output / (name + ".o")
                    run([args.emitter.resolve(), fixture / f"{source}.gti", source_obj,
                         output / (name + ".ll"), level, "--source-preflight"], "emit-" + name)
                    run([args.cxx, source_obj, host, args.runtime.resolve(), "-pthread",
                         "-o", target], "link-" + name)
                else:
                    run([args.compiler.resolve(), fixture / f"{source}.gti", "--backend",
                         "llvm", f"-O{level}", "-o", target, "--", host], "build-" + name)
                failure = source == "counted_stdout_failure"
                result = run([target], "run-" + name, expected=70 if failure else 0)
                if failure:
                    if (result.stdout or b"counted-cleanup\n" not in result.stderr or
                            b"stdout write failed" not in result.stderr):
                        raise RuntimeError(f"source stdout status/panic cleanup mismatch: {name}")
                elif result.stdout != b"A\0B\xc3\xa9\xce\xbb\n" or result.stderr:
                    raise RuntimeError(f"source counted stdout mismatch: {name}")
    scope = ("production GTI source" if args.compiler else
             "real-source preflight, public admission unchanged" if args.preflight else
             "helper only; no GTI source-body or admission proof")
    print(f"PASS: counted C input/runtime stdout matrix; {scope}; {output}")


if __name__ == "__main__":
    main()
