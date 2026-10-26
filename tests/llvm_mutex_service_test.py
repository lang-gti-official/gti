#!/usr/bin/env python3
"""Source-verified mutex emitter/runtime boundary, not public lifecycle admission."""

import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cc", default="clang")
    parser.add_argument("--sanitize", choices=("address,undefined", "thread"))
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = args.evidence_dir.resolve() if args.evidence_dir else output
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    environment = dict(os.environ)
    if args.sanitize == "address,undefined":
        leaks = int(sys.platform != "darwin")
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        print(f"Address instrumentation and C/C++ UBSan; leak detection={leaks}", flush=True)
    elif args.sanitize == "thread":
        environment["TSAN_OPTIONS"] = "halt_on_error=1"

    def run(name, argv, death=False):
        start = time.monotonic()
        result = subprocess.run([str(value) for value in argv], capture_output=True,
                                text=True, timeout=60, env=environment)
        (evidence / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(value) for value in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - start})
        (evidence / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        valid = result.returncode in (-signal.SIGILL, -signal.SIGTRAP) if death else result.returncode == 0
        if not valid:
            raise AssertionError(f"{name}: see {evidence / (name + '.log')}")
        print(f"{name}: PASS", flush=True)
        return result.stdout

    sanitize = [args.sanitize] if args.sanitize else []
    run("emit", [args.emitter.resolve(), root, output / "mutex.ll", *sanitize])
    flags = ["-O3", "-pthread", "-Wall", "-Wextra", "-Werror", "-I", root / "runtime/include"]
    instrumentation = [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    flags += instrumentation
    run("runtime", [args.cxx, "-std=c++20", *flags,
        "-Dgti_rt_mutex_lock_v1=gti_test_mutex_real_lock_v1", "-c", root / "runtime/src/mutex.cpp",
        "-o", output / "runtime.o"])
    run("fixture", [args.cc, "-std=c17", *flags, "-c", root / "tests/fixtures/llvm_mutex_service.c",
                    "-o", output / "fixture.o"])
    for mode in ("O0", "O1", "O2", "O3"):
        run(f"ir-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation, "-c", output / "mutex.ll",
                           "-o", output / f"mutex-{mode}.o"])
        if args.sanitize:
            symbols = run(f"instrumentation-{mode}", ["nm", "-u", output / f"mutex-{mode}.o"])
            marker = "__tsan_read" if args.sanitize == "thread" else "__asan_report_"
            if marker not in symbols:
                raise AssertionError(f"generated {mode} mutex access lacks {args.sanitize} instrumentation")
        program = output / f"mutex-{mode}"
        run(f"link-{mode}", [args.cxx, "-pthread", *instrumentation, output / "fixture.o", output / "runtime.o",
                             output / f"mutex-{mode}.o", "-o", program])
        run(f"execute-{mode}", [program])
        for fault in ("active-lock", "wrong-owner", "inactive-read"):
            run(f"{fault}-{mode}", [program, fault], death=True)


if __name__ == "__main__":
    main()
