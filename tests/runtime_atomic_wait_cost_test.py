#!/usr/bin/env python3
"""Retain successful-path wait service cost observations, not a parity gate."""

import argparse
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
    args = parser.parse_args()
    root, output = args.root.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []

    def run(name, argv):
        start = time.monotonic()
        if name in ("runtime-build", "cost-build", "measurements"):
            argv = ["/usr/bin/time", "-l" if sys.platform == "darwin" else "-v", *argv]
        result = subprocess.run([str(value) for value in argv], capture_output=True,
                                text=True, timeout=120)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(value) for value in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - start})
        (output / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {output / (name + '.log')}")
        return result.stdout

    run("compiler", [args.cxx, "--version"])
    flags = ["-std=c++20", "-O3", "-DNDEBUG", "-pthread", "-fno-strict-aliasing",
             "-I", root / "runtime/include"]
    obj, program = output / "wait.o", output / "cost"
    run("runtime-build", [args.cxx, *flags, "-c", root / "runtime/src/atomic_wait.cpp", "-o", obj])
    run("cost-build", [args.cxx, *flags, root / "tests/runtime_atomic_wait_cost.cpp", obj, "-o", program])
    symbols = run("runtime-symbols", ["nm", "-u", obj])
    run("object-size", ["size", obj])
    measurements = run("measurements", [program])
    report = {"runtime_object_file_bytes": obj.stat().st_size,
              "benchmark_file_bytes": program.stat().st_size,
              "direct_allocation_symbol_present": any(name in symbols for name in ("malloc", "calloc", "realloc", "_Zn")),
              "allocation_scope": "direct service dependencies only; pthread/libc internal allocation not measured",
              "measurements": measurements.splitlines(),
              "acceptance": "observations only; no performance parity or whole-milestone acceptance"}
    (output / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
