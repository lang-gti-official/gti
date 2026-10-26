#!/usr/bin/env python3
"""Real LLVM atomic plans interoperate with the production parking service.

These C-shaped test adapters are not managed-worker or foreign ABI admission.
Public whole-program execution is separately covered by atomic.wait-ready.
"""

import argparse
import json
import os
from pathlib import Path
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
    root, emitter, output = args.root.resolve(), args.emitter.resolve(), args.output.resolve()
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

    def run(name, argv):
        start = time.monotonic()
        result = subprocess.run([str(part) for part in argv], capture_output=True,
                                text=True, timeout=60, env=environment)
        (evidence / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(part) for part in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - start})
        (evidence / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {evidence / (name + '.log')}")
        print(f"{name}: PASS", flush=True)
        return result.stdout

    sanitize = [args.sanitize] if args.sanitize else []
    run("emit-lanes", [emitter, root, "--emit-wait-lanes", output / "lanes.ll", *sanitize])
    run("emit-services", [emitter, root, "--emit-wait-services", output / "services.ll", *sanitize])
    flags = ["-O3", "-pthread", "-Wall", "-Wextra", "-Werror"]
    instrumentation = [f"-fsanitize={args.sanitize}", "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    flags += instrumentation
    run("runtime", [args.cxx, "-std=c++20", *flags, "-fno-strict-aliasing",
                    "-I", root / "runtime/include", "-c", root / "runtime/src/atomic_wait.cpp",
                    "-o", output / "runtime.o"])
    run("c-oracle", [args.cc, "-std=c17", *flags, "-c",
                     root / "tests/fixtures/llvm_atomic_wait_service.c", "-o", output / "c.o"])
    for mode in ("O0", "O1", "O2", "O3"):
        for component in ("lanes", "services"):
            run(f"{component}-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", *instrumentation, "-c",
                output / f"{component}.ll", "-o", output / f"{component}-{mode}.o"])
        if args.sanitize:
            symbols = run(f"instrumentation-{mode}", ["nm", "-u", output / f"lanes-{mode}.o"])
            marker = "__tsan_atomic" if args.sanitize == "thread" else "__asan_report_"
            if marker not in symbols:
                raise AssertionError(f"generated {mode} lanes lack real {args.sanitize} instrumentation")
        program = output / f"wait-{mode}"
        run(f"link-{mode}", [args.cxx, "-pthread", *instrumentation, output / "runtime.o", output / "c.o",
            output / f"lanes-{mode}.o", output / f"services-{mode}.o", "-o", program])
        run(f"execute-{mode}", [program])


if __name__ == "__main__":
    main()
