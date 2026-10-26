#!/usr/bin/env python3
"""Real LLVM atomic plans interoperate with the production parking service.

These C-shaped test adapters are not managed-worker or foreign ABI admission.
Public whole-program execution is separately covered by atomic.wait-ready.
"""

import argparse
import json
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--llvm-cc", default="clang")
    args = parser.parse_args()
    root, emitter, output = args.root.resolve(), args.emitter.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    records = []

    def run(name, argv):
        start = time.monotonic()
        result = subprocess.run([str(part) for part in argv], capture_output=True,
                                text=True, timeout=60)
        (output / f"{name}.log").write_text(result.stdout + result.stderr)
        records.append({"name": name, "argv": [str(part) for part in argv],
                        "exit": result.returncode, "seconds": time.monotonic() - start})
        (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
        if result.returncode:
            raise AssertionError(f"{name}: see {output / (name + '.log')}")
        print(f"{name}: PASS", flush=True)

    run("emit-lanes", [emitter, root, "--emit-wait-lanes", output / "lanes.ll"])
    run("emit-services", [emitter, root, "--emit-wait-services", output / "services.ll"])
    flags = ["-O3", "-pthread", "-Wall", "-Wextra", "-Werror"]
    run("runtime", [args.cxx, "-std=c++20", *flags, "-fno-strict-aliasing",
                    "-I", root / "runtime/include", "-c", root / "runtime/src/atomic_wait.cpp",
                    "-o", output / "runtime.o"])
    run("c-oracle", [args.cc, "-std=c17", *flags, "-c",
                     root / "tests/fixtures/llvm_atomic_wait_service.c", "-o", output / "c.o"])
    for mode in ("O0", "O1", "O2", "O3"):
        for component in ("lanes", "services"):
            run(f"{component}-{mode}", [args.llvm_cc, "-x", "ir", f"-{mode}", "-c",
                output / f"{component}.ll", "-o", output / f"{component}-{mode}.o"])
        program = output / f"wait-{mode}"
        run(f"link-{mode}", [args.cxx, "-pthread", output / "runtime.o", output / "c.o",
            output / f"lanes-{mode}.o", output / f"services-{mode}.o", "-o", program])
        run(f"execute-{mode}", [program])


if __name__ == "__main__":
    main()
