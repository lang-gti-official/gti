#!/usr/bin/env python3
"""Exercise native indirect calls against an independently compiled C fixture."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
from backend_modes import keep_cpp, native_modes


def run(command):
    result = subprocess.run([str(part) for part in command], capture_output=True,
                            text=True, timeout=180)
    if result.returncode != 0:
        raise RuntimeError(f"{command}\n{result.stdout}\n{result.stderr}")
    return result


def main():
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent.parent
    source = root / "tests/fixtures/native_indirect_calls.gti"
    example = root / "examples/122-native-function-invocation.gti"
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-native-indirect-") as directory:
        work = Path(directory)
        header = work / "native_indirect_calls.h"
        run([compiler, source, "--emit-native-header", "-o", header])
        for name, flags, native_flags in modes:
            native_object = work / f"native-{name}.o"
            run([os.environ.get("CC", "cc"), "-std=c17", "-I", work,
                 "-I", root / "runtime/include", "-c", source.with_suffix(".c"),
                 "-o", native_object, *native_flags])
            binary = work / name
            run([compiler, source, *flags,
                 *keep_cpp(flags), "-o", binary, "--", native_object, *native_flags])
            executed = run([binary])
            assert executed.stdout == "" and executed.stderr == "", executed
            example_binary = work / f"example-{name}"
            run([compiler, example, *flags,
                 "-o", example_binary,
                 *native_flags])
            run([example_binary])
            failure_binary = work / f"failure-{name}"
            run([compiler, source.with_name("native_indirect_failure.gti"),
                 *flags, "-o", failure_binary, *native_flags])
            failure = subprocess.run([str(failure_binary)], capture_output=True,
                                     text=True, timeout=180)
            assert failure.returncode == 70, failure
            assert failure.stdout == "target cleanup\n", failure
            assert "native callback failure" in failure.stderr, failure
            print(f"native indirect calls: {name} passed", flush=True)


if __name__ == "__main__":
    main()
