#!/usr/bin/env python3
"""Standalone real-C bidirectional ABI matrix; retains all objects, IR and logs."""

import argparse
import json
import platform
from pathlib import Path
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent / "fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {"platform": platform.platform(), "machine": platform.machine(),
                "cc": args.cc, "steps": [], "production_admission": "closed"}
    evidence["source_head"] = subprocess.check_output(
        ["git", "-C", str(root.parents[2]), "rev-parse", "HEAD"], text=True).strip()

    def run(command, name):
        started = time.monotonic()
        command = [str(part) for part in command]
        result = subprocess.run(command, capture_output=True, text=True, timeout=180)
        log = output / (name + ".log")
        log.write_text(result.stdout + result.stderr)
        evidence["steps"].append({"argv": command, "exit": result.returncode,
                                  "seconds": time.monotonic() - started,
                                  "log": str(log)})
        (output / "evidence.json").write_text(json.dumps(evidence, indent=2) + "\n")
        if result.returncode:
            raise RuntimeError(f"{name} failed ({result.returncode}); see {log}")
        return result.stdout

    run([args.cc, "--version"], "c-version")
    executions = 0
    for level in range(4):
        obj = output / f"native-O{level}.o"
        run([args.emitter.resolve(), root / "records.gti", obj,
             output / f"native-O{level}.ll", level], f"emit-O{level}")
        for c_level in (0, 3):
            executable = output / f"oracle-native{level}-c{c_level}"
            run([args.cc, "-std=c17", "-Wall", "-Wextra", "-Werror",
                 f"-O{c_level}", root / "oracle.c", obj, "-o", executable],
                f"link-native{level}-c{c_level}")
            observed = run([executable], f"run-native{level}-c{c_level}")
            if observed != ("native C ABI: 25 records, 3 register-pressure "
                            "signatures, both directions\n"):
                raise RuntimeError(f"unexpected oracle output: {observed!r}")
            executions += 1
    print(f"PASS: {executions} bidirectional C/LLVM executions, native O0-O3, "
          f"C O0/O3, {platform.system()} {platform.machine()}; {output}")


if __name__ == "__main__":
    main()
