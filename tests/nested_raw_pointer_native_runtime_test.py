#!/usr/bin/env python3
"""Independent C retention, slot aliasing and native pointer-array ABI."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


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
    modes = [(standard, level, [])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes += [("c++23", "O0", ["-fno-elide-constructors"]),
              ("c++23", "O1", ["-fsanitize=address,undefined",
                               "-fno-omit-frame-pointer"])]
    with tempfile.TemporaryDirectory(prefix="gti-pointer-slots-") as temporary:
        work = Path(temporary)
        run([compiler, source, "--emit-native-header", "-o", work / "native.h"],
            environment)
        for index, (standard, level, flags) in enumerate(modes):
            obj = work / f"native-{index}.o"
            instrumentation = [flag for flag in flags
                               if flag != "-fno-elide-constructors"]
            run([os.environ.get("CC", "cc"), "-std=c17", f"-{level}",
                 "-Wall", "-Wextra", "-Werror", "-I", work, "-I",
                 root / "runtime/include", *instrumentation,
                 "-c", native, "-o", obj], environment)
            binary = work / f"pointer-slots-{index}"
            run([compiler, source, "--std", standard, f"-{level}", "-o", binary,
                 "--", obj, *flags], environment)
            result = run([binary], environment)
            assert result.stdout == "" and result.stderr == "", result
            print(f"native pointer slots: {standard}/{level}/{flags} passed",
                  flush=True)


if __name__ == "__main__":
    main()
