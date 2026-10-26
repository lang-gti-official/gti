"""Check nonthrowing worker/caller sinks, optionally rebuilding with sanitizers."""
import argparse
import json
import os
from pathlib import Path
import signal
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("program", type=Path)
    parser.add_argument("--sanitize", choices=("address,undefined", "thread"))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    args = parser.parse_args()
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    records = []
    output = args.output.resolve() if args.output else None
    if output:
        output.mkdir(parents=True, exist_ok=True)

    def run(name, command, expected=0):
        start = time.monotonic()
        command = [str(part) for part in command]
        result = subprocess.run(command, capture_output=True, text=True,
                                timeout=90, env=environment)
        records.append({"name": name, "argv": command, "exit": result.returncode,
                        "seconds": time.monotonic() - start})
        if output:
            (output / f"{name}.log").write_text(result.stdout + result.stderr)
            (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode != expected:
            raise AssertionError(f"{name}: {result.returncode}, expected {expected}\n"
                                 f"{result.stdout}{result.stderr}")

    program = args.program.resolve()
    if args.sanitize:
        if not output:
            parser.error("--sanitize requires --output for exact evidence")
        root = Path(__file__).resolve().parent.parent
        flags = ["-O1", "-g", "-pthread", f"-fsanitize={args.sanitize}",
                 "-fno-omit-frame-pointer", "-I", root / "runtime/include"]
        run("compile-c17", [args.cc, "-std=c17", *flags, "-c",
                            root / "tests/fixtures/runtime_thread_outcome_c.c",
                            "-o", output / "c17.o"])
        program = output / "outcome"
        run("compile-runtime", [args.cxx, "-std=c++20", *flags,
                                root / "runtime/src/cthread_state.cpp",
                                root / "runtime/src/thread.cpp",
                                root / "runtime/src/failure.cpp",
                                root / "runtime/src/execution.cpp",
                                root / "tests/runtime_thread_outcome_tests.cpp",
                                output / "c17.o", "-o", program])
    run("outcome-matrix", [program])
    for mode in ("no-sink", "wrong-thread", "out-of-order", "duplicate-enter",
                 "throwing-sink", "reentrant", "malformed", "successful-null-completion",
                 "bad-sink-version", "mutating-sink", "wrong-primary-envelope",
                 "third-failure"):
        run(mode, [program, mode], -signal.SIGABRT)
    print("nonthrowing worker/caller outcome matrix and twelve boundary rejections passed")


if __name__ == "__main__":
    main()
