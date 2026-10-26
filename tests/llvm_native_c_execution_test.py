#!/usr/bin/env python3
"""Real GTI-to-C execution, native headers, staging, foreign effects and cleanup."""

import argparse
import json
import platform
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--analysis-only", action="store_true")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    fixture = repo / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    compiler = args.compiler.resolve()
    evidence = {"head": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "cc": args.cc,
        "analysis_only": args.analysis_only, "steps": []}

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

    run([compiler, "--version"], "compiler-version")
    run([args.cc, "--version"], "c-version")
    for name in ("native_calls", "native_failure"):
        run([compiler, fixture / f"{name}.gti", "--emit-mir", "-o",
             output / f"{name}.mir"], f"mir-{name}")
    run([compiler, fixture / "native_calls.gti", "--emit-native-header", "-o",
         output / "gti.native.h"], "native-header")
    run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", "-O2",
         "-I", output, "-I", repo / "runtime/include", "-c",
         fixture / "header_oracle.c", "-o", output / "header-oracle.o"], "header-c17")
    executions = 0
    for c_level in (() if args.analysis_only else (0, 3)):
        obj = output / f"native-c-O{c_level}.o"
        run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror", f"-O{c_level}",
             "-I", repo / "runtime/include", "-c", fixture / "native_calls.c", "-o", obj],
            f"c-object-O{c_level}")
        for level in range(4):
            for name in ("native_calls", "native_failure"):
                executable = output / f"{name}-llvm{level}-c{c_level}"
                tag = executable.name
                run([compiler, fixture / f"{name}.gti", "--backend", "llvm", f"-O{level}",
                     "-o", executable, "--", obj], "build-" + tag)
                result = run([executable], "run-" + tag,
                             expected=70 if name == "native_failure" else 0)
                if name == "native_calls":
                    if result.stdout or result.stderr:
                        raise RuntimeError(f"unexpected success output: {tag}")
                elif (result.stdout != "mark:1\nmark:99\n" or
                      "GTI-R" not in result.stderr or
                      "division" not in result.stderr.lower() or
                      "native_failure.gti" not in result.stderr):
                    raise RuntimeError(f"native argument failure/cleanup mismatch: {tag}")
                executions += 1

    invalid = {
        "foreign-read-needs-unsafe": (
            'extern "C" { mut int32_t native_counter; }\n'
            'int main() { return native_counter; }\n'),
        "foreign-const-write": (
            'extern "C" { int32_t native_limit; }\n'
            'int main() { unsafe { native_limit = 1; } return 0; }\n'),
        "record-reference": (
            '[[c_abi]] struct Bad { mut int32_t& value; };\n'
            'int main() { return 0; }\n'),
        "record-nonpassive": (
            'struct Owned { mut int32_t value; ~Owned() {} };\n'
            '[[c_abi]] struct Bad { mut Owned value; };\n'
            'int main() { return 0; }\n'),
    }
    for name, source in invalid.items():
        path = output / f"{name}.gti"
        path.write_text(source)
        target = output / f"{name}.artifact"
        target.write_bytes(b"preserve-existing-native-artifact\n")
        result = run([compiler, path, "--backend", "llvm", "-O3", "-o", target],
                     name, expected=65)
        if "GTI-S" not in result.stderr or target.read_bytes() != b"preserve-existing-native-artifact\n":
            raise RuntimeError(f"negative admission/publication mismatch: {name}")
    if args.analysis_only:
        print(f"ANALYSIS ONLY: verified MIR, C17 header and {len(invalid)} rejection controls; "
              f"no LLVM execution claimed; {output}")
    else:
        print(f"PASS: {executions} real GTI/C executions, header and {len(invalid)} rejection controls; {output}")


if __name__ == "__main__":
    main()
