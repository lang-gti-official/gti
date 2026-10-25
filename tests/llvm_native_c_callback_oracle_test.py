#!/usr/bin/env python3
"""Native callback ABI/firewall oracle with explicit synthetic GTI test bodies."""

import argparse
import json
from pathlib import Path
import platform
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emitter", type=Path)
    parser.add_argument("--cxx", default="c++")
    parser.add_argument("--runtime", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    fixture = repo / "tests/fixtures/llvm_native_c_abi"
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    evidence = {"head": subprocess.check_output(
        ["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip(),
        "platform": platform.platform(), "cxx": args.cxx,
        "body_coverage": "synthetic private-ABI test bodies, not production GTI execution",
        "steps": []}

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

    run([args.cxx, "--version"], "cxx-version")
    executions = 0
    for level in range(4):
        obj = output / f"callbacks-O{level}.o"
        run([args.emitter.resolve(), fixture / "callbacks.gti", obj,
             output / f"callbacks-O{level}.ll", level, "--callbacks"], f"emit-O{level}")
        for host_level in (0, 3):
            executable = output / f"callbacks-native{level}-host{host_level}"
            tag = executable.name
            run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 f"-O{host_level}", fixture / "callback_oracle.cpp", obj,
                 args.runtime.resolve(), "-pthread", "-o", executable], "link-" + tag)
            success = run([executable, "success"], "success-" + tag)
            exception = run([executable, "exception"], "exception-" + tag, expected=86)
            failure = run([executable, "failure"], "failure-" + tag, expected=70)
            if success.stdout != "native callback boundary oracle\n" or success.stderr:
                raise RuntimeError(f"callback value transport mismatch: {tag}")
            if exception.stdout or exception.stderr != "foreign-cleanup\n":
                raise RuntimeError(f"native exception escaped or skipped native cleanup: {tag}")
            if (failure.stdout or "GTI-R" not in failure.stderr or
                    "division" not in failure.stderr.lower() or
                    "callbacks.gti" not in failure.stderr):
                raise RuntimeError(f"defined failure lost original attribution: {tag}")
            executions += 3
        thread_obj = output / f"thread-callbacks-O{level}.o"
        run([args.emitter.resolve(), fixture / "thread_callbacks.gti", thread_obj,
             output / f"thread-callbacks-O{level}.ll", level, "--thread-callbacks"],
            f"emit-thread-O{level}")
        for host_level in (0, 3):
            executable = output / f"thread-callbacks-native{level}-host{host_level}"
            tag = executable.name
            run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                 f"-O{host_level}", "-I", repo / "runtime/include",
                 fixture / "thread_callback_oracle.cpp", thread_obj,
                 args.runtime.resolve(), "-pthread", "-o", executable], "link-" + tag)
            for mode in ("success", "convert-before-begin", "invoke-foreign",
                         "convert-foreign", "worker-on-initial", "once-unregistered",
                         "invoke-after-end"):
                result = run([executable, mode], mode + "-" + tag,
                             expected=0 if mode == "success" else 86)
                if result.stdout or result.stderr:
                    raise RuntimeError(f"unexpected callback thread-policy output: {tag} {mode}")
                executions += 1
    print(f"PASS: {executions} isolated callback ABI/firewall executions; "
          f"synthetic GTI bodies, not production admission; {output}")


if __name__ == "__main__":
    main()
