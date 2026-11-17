#!/usr/bin/env python3
"""Native condition runtime unit checks; the GTI clients are tests/ui/concurrency/cthread."""

import pathlib
import sys
import tempfile

from cthread_mutex_test import run
from sanitizer_support import thread_sanitizer_enabled

from native_symbol_spelling import native_spelling
from backend_modes import cpp_modes, cpp_variant_mode, is_cpp_mode, keep_cpp, native_modes, optimization_level


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: cthread_condition_test.py <gti> <cxx>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cxx = pathlib.Path(sys.argv[2]).absolute()
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    modes = native_modes()
    # The thread-sanitized variant instruments generated C++, so it runs only
    # when the policy selects the C++ backend.
    if thread_sanitizer_enabled() and cpp_modes():
        modes.append(cpp_variant_mode("tsan", native=["-fsanitize=thread", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-cthread-condition-") as directory:
        output = pathlib.Path(directory)
        for mode, flags, sanitizers in modes:
            native = output / f"native-{mode}"
            run([cxx, "-std=c++23", f"-{optimization_level(flags)}", *sanitizers,
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", root / "runtime/include", "-I", root / "runtime/src",
                 root / "runtime/src/cthread_mutex.cpp",
                 root / "tests/unit/runtime_cthread_condition_tests.cpp", "-o", native])
            run([native])
            run([native, "destroy-waiting"], 86)
            run([native, "reacquire-failure"], 86)
            print(f"C-thread condition: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
