#!/usr/bin/env python3
"""Native confined-callback runtime unit checks; the GTI client is
tests/ui/concurrency/thread_confined_callbacks.gti."""
import os
import pathlib
import subprocess
import sys
import tempfile
from cthread_mutex_test import run
from backend_modes import is_cpp_mode, keep_cpp, native_modes, optimization_level

def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cxx = pathlib.Path(sys.argv[2]).absolute()
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-confined-") as temporary:
        for index, (label, flags, sanitizers) in enumerate(modes):
            native = pathlib.Path(temporary) / f"native-{index}"
            run([cxx, "-std=c++23", f"-{optimization_level(flags)}", *sanitizers, "-pthread",
                 "-Wall", "-Wextra", "-Werror", "-I", root / "runtime/include",
                 "-I", root / "runtime/src", root / "runtime/src/cthread_state.cpp",
                 root / "runtime/src/thread.cpp",
                 root / "runtime/src/failure.cpp",
                 root / "tests/runtime_confined_callback_tests.cpp", "-o", native])
            run([native])
            for boundary in ("before-begin", "zero", "unbalanced", "foreign", "scope-ended", "after-end"):
                run([native, boundary], 86)
            print(f"thread-confined callbacks: {label} passed", flush=True)

if __name__ == "__main__":
    main()
