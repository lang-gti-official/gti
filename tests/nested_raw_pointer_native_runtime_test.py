#!/usr/bin/env python3
"""Independent C retention, slot aliasing and native pointer-array ABI."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from backend_modes import native_modes, optimization_level


def run(command, environment):
    result = subprocess.run([str(value) for value in command], capture_output=True,
                            text=True, timeout=120, env=environment)
    assert result.returncode == 0, (command, result.stdout, result.stderr)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    root = Path(__file__).resolve().parents[1]
    source = root / "tests/fixtures/nested_raw_pointer_slots.gti"
    native = root / "tests/fixtures/nested_raw_pointer_slots.c"
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-pointer-slots-") as temporary:
        work = Path(temporary)
        run([compiler, source, "--emit-native-header", "-o", work / "native.h"],
            environment)
        for index, (mode, flags, instrumentation) in enumerate(modes):
            obj = work / f"native-{index}.o"
            run([os.environ.get("CC", "cc"), "-std=c17", f"-{optimization_level(flags)}",
                 "-Wall", "-Wextra", "-Werror", "-I", work, "-I",
                 root / "runtime/include", *instrumentation,
                 "-c", native, "-o", obj], environment)
            binary = work / f"pointer-slots-{index}"
            run([compiler, source, *flags, "-o", binary,
                 "--", obj, *instrumentation], environment)
            result = run([binary], environment)
            assert result.stdout == "" and result.stderr == "", result
            print(f"native pointer slots: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
