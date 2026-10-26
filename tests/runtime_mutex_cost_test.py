#!/usr/bin/env python3
"""Observe mutex boundary costs without asserting public-lifecycle parity."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-object", type=Path)
    parser.add_argument("--runtime-library", type=Path)
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []

    def run(name, argv, timed=False):
        if timed:
            argv = ["/usr/bin/time", "-l" if sys.platform == "darwin" else "-v", *argv]
        started = time.monotonic()
        result = subprocess.run([str(value) for value in argv], capture_output=True,
                                text=True, timeout=120)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(value) for value in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - started})
        (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {output / (name + '.log')}")
        return result.stdout

    run("compiler", [args.cxx, "--version"])
    flags = ["-std=c++20", "-O3", "-pthread", "-I", root / "runtime/include"]
    obj, program = output / "mutex.o", output / "cost"
    run("runtime-build", [args.cxx, *flags, "-c", root / "runtime/src/mutex.cpp",
                          "-o", obj], timed=True)
    extra, emitted = [], None
    if args.llvm_object:
        selected = args.llvm_object.resolve()
        extra = ["-DGTI_COST_LLVM", selected]
        if args.runtime_library:
            extra.append(args.runtime_library.resolve())
        emitted = {"path": str(selected), "bytes": selected.stat().st_size,
                   "sha256": hashlib.sha256(selected.read_bytes()).hexdigest()}
        run("emitter-size", ["size", selected])
        emitted["undefined_symbols"] = run("emitter-symbols", ["nm", "-u", selected])
    run("cost-build", [args.cxx, *flags, root / "tests/runtime_mutex_cost.cpp",
                       obj, *extra, "-o", program], timed=True)
    symbols = run("runtime-symbols", ["nm", "-u", obj])
    run("runtime-size", ["size", obj])
    measurements = run("measurements", [program], timed=True)
    report = {"runtime_object_file_bytes": obj.stat().st_size,
              "benchmark_file_bytes": program.stat().st_size,
              "emitted_object": emitted,
              "direct_allocation_symbol_present": any(
                  name in symbols for name in ("malloc", "calloc", "realloc", "_Zn")),
              "allocation_scope": "direct runtime dependencies; libc internal allocation not measured",
              "measurements": measurements.splitlines(),
              "acceptance": "uncontended observations; C service and checked token adapter have distinct contracts; public lifecycle and contention acceptance remain open"}
    (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
