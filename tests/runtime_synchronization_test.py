#!/usr/bin/env python3
"""Exercise synchronization C services and deterministic pthread controls."""

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
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--sanitize", choices=("address,undefined", "thread"))
    parser.add_argument("--component", choices=("all", "wait", "mutex"), default="all")
    parser.add_argument("--evidence-dir", type=Path)
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = args.evidence_dir.resolve() if args.evidence_dir else output
    evidence.mkdir(parents=True, exist_ok=True)
    records = []
    environment = dict(os.environ)
    if args.sanitize == "address,undefined":
        leaks = 0 if sys.platform == "darwin" else 1
        environment["ASAN_OPTIONS"] = f"detect_leaks={leaks}:halt_on_error=1"
        environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        print(f"Address/undefined sanitizers; leak detection={leaks}", flush=True)
    if args.sanitize == "thread":
        environment["TSAN_OPTIONS"] = "halt_on_error=1"

    def run(name, argv, death=False):
        started = time.monotonic()
        result = subprocess.run([str(arg) for arg in argv], capture_output=True,
                                text=True, timeout=60, env=environment)
        (evidence / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(arg) for arg in argv],
                        "exit": result.returncode,
                        "seconds": time.monotonic() - started})
        (evidence / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        expected = -signal.SIGABRT if death else 0
        if result.returncode != expected:
            raise AssertionError(f"{name}: exit {result.returncode}, expected {expected}; "
                                 f"see {evidence / (name + '.log')}")
        print(f"{name}: PASS", flush=True)
        return result.stdout

    flags = ["-O3", "-fno-strict-aliasing", "-pthread",
             "-I", root / "runtime/include", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer", "-g"]
    if args.component != "wait":
        run("mutex-runtime", [args.cxx, "-std=c++20", *flags, "-c",
            root / "runtime/src/mutex.cpp", "-o", output / "mutex.o"])
        run("mutex-c-header", [args.cc, "-std=c17", *flags, "-c",
            root / "tests/fixtures/runtime_mutex_c.c", "-o", output / "mutex-c.o"])
        run("mutex-c-link", [args.cxx, *flags, output / "mutex.o", output / "mutex-c.o",
            "-o", output / "mutex-c"])
        run("mutex-c-execution", [output / "mutex-c"])
        run("mutex-controlled", [args.cxx, "-std=c++20", *flags,
            "-Dpthread_mutex_init=gti_test_mutex_init",
            "-Dpthread_mutex_lock=gti_test_mutex_lock",
            "-Dpthread_mutex_unlock=gti_test_mutex_unlock",
            "-Dpthread_mutex_destroy=gti_test_mutex_destroy", "-c",
            root / "runtime/src/mutex.cpp", "-o", output / "mutex-controlled.o"])
        run("mutex-controlled-link", [args.cxx, *flags, output / "mutex-controlled.o",
            output / "mutex-c.o", "-o", output / "mutex-controlled"])
        run("mutex-controlled-execution", [output / "mutex-controlled"])
        run("mutex-lock-status", [output / "mutex-controlled", "lock"])
        for fault in ("init", "unlock", "destroy"):
            run(f"mutex-reject-{fault}", [output / "mutex-controlled", fault], death=True)
    if args.component == "mutex":
        return
    runtime = root / "runtime/src/atomic_wait.cpp"
    run("runtime", [args.cxx, "-std=c++20", *flags, "-c", runtime,
                    "-o", output / "runtime.o"])
    symbols = run("symbols", ["nm", "-u", output / "runtime.o"])
    assert "__atomic_" not in symbols and "libatomic" not in symbols, symbols
    run("c-header", [args.cc, "-std=c17", *flags, "-c",
                     root / "tests/fixtures/runtime_atomic_wait_c.c",
                     "-o", output / "c.o"])
    run("c-link", [args.cxx, *flags, output / "runtime.o", output / "c.o",
                   "-o", output / "c-oracle"])
    run("c-execution", [output / "c-oracle"])
    run("controlled-runtime", [args.cxx, "-std=c++20", *flags,
        "-Dpthread_mutex_init=gti_test_wait_init",
        "-Dpthread_mutex_lock=gti_test_wait_lock",
        "-Dpthread_cond_broadcast=gti_test_wait_wake",
        "-Dpthread_cond_wait=gti_test_wait_sleep", "-c", runtime,
        "-o", output / "controlled.o"])
    run("controlled-link", [args.cxx, "-std=c++20", *flags,
        root / "tests/runtime_atomic_wait_tests.cpp", output / "controlled.o",
        "-o", output / "controlled"])
    run("controlled-execution", [output / "controlled"])
    run("late-first-use", [output / "controlled", "late"])
    for fault in ("null", "width", "width-zero", "width-wide", "alignment",
                  "alignment-2", "alignment-4", "order", "bits", "notify",
                  "init", "lock", "wait"):
        run(f"reject-{fault}", [output / "controlled", fault], death=True)


if __name__ == "__main__":
    main()
