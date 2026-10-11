#!/usr/bin/env python3
"""Exercise native indirect calls against an independently compiled C fixture."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True,
                            text=True, timeout=scaled(180))
    if result.returncode != 0:
        raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    source = root / "tests/fixtures/native_indirect_calls.gti"
    example = root / "examples/122-native-function-invocation.gti"
    modes = [(f"{standard}-{level}", standard, level, [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", "c++23", "O1",
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-native-indirect-") as directory:
        work = Path(directory)
        header = work / "native_indirect_calls.h"
        run([compiler, source, "--emit-native-header", "-o", header])
        for name, standard, level, sanitizers in modes:
            native_object = work / f"native-{name}.o"
            run([os.environ.get("CC", "cc"), "-std=c17", "-I", work,
                 "-I", root / "runtime/include", "-c", source.with_suffix(".c"),
                 "-o", native_object, *sanitizers])
            binary = work / name
            run([compiler, source, "--std", standard, f"-{level}",
                 "--keep-cpp", "-o", binary, "--", native_object,
                 "-fno-elide-constructors", *sanitizers])
            executed = run([binary])
            assert executed.stdout == "" and executed.stderr == "", executed
            example_binary = work / f"example-{name}"
            run([compiler, example, "--std", standard, f"-{level}",
                 "-o", example_binary, "--", "-fno-elide-constructors",
                 *sanitizers])
            run([example_binary])
            failure_binary = work / f"failure-{name}"
            run([compiler, source.with_name("native_indirect_failure.gti"),
                 "--std", standard, f"-{level}", "-o", failure_binary,
                 "--", "-fno-elide-constructors", *sanitizers])
            failure = subprocess.run([str(failure_binary)], capture_output=True,
                                     text=True, timeout=scaled(180))
            assert failure.returncode == 70, failure
            assert failure.stdout == "target cleanup\n", failure
            assert "native callback failure" in failure.stderr, failure
            print(f"native indirect calls: {name} passed", flush=True)


if __name__ == "__main__":
    main()
