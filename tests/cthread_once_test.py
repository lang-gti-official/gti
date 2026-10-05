#!/usr/bin/env python3
"""Source atomic once arbitration and exact native initializer entry in eight modes."""

import pathlib
import os
import subprocess
import sys
import tempfile

from cthread_mutex_test import run
from sanitizer_support import thread_sanitizer_enabled


def execute(binary, status, stdout):
    environment = os.environ.copy()
    environment.update(ASAN_OPTIONS="halt_on_error=1:abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                       TSAN_OPTIONS="halt_on_error=1:abort_on_error=1")
    result = subprocess.run([str(binary)], capture_output=True, text=True,
                            timeout=120, env=environment)
    if (result.returncode != status or result.stdout != stdout or
            (status == 70 and ("GTI-R0002" not in result.stderr or
                              "cthread_once_failure" not in result.stderr)) or
            (status != 70 and result.stderr)):
        raise AssertionError(f"{binary}: {result.returncode}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: cthread_once_test.py <gti> <cxx>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cxx = pathlib.Path(sys.argv[2]).absolute()
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    modes = [(f"{std}-{opt}", std, opt, []) for std in ("c++20", "c++23")
             for opt in ("O0", "O1", "O3")]
    modes.append(("asan-ubsan", "c++23", "O1",
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    if thread_sanitizer_enabled():
        modes.append(("tsan", "c++23", "O1",
                      ["-fsanitize=thread", "-fno-omit-frame-pointer"]))
    cases = [("immobile_fresh_construction", 0, ""),
             ("cthread_once", 0, ""),
             ("cthread_once_failure", 70, "once-cleaned\n"),
             ("cthread_once_failure_managed", 70, "once-cleaned\n"),
             ("cthread_once_failure_after_return", 70, "once-cleaned\nowner-cleaned\n"),
             ("cthread_once_entry_rejected", 86, ""),
             ("cthread_once_foreign_rejected", 86, ""),
             ("cthread_once_null_flag", 86, ""),
             ("cthread_once_null_initializer", 86, "")]
    with tempfile.TemporaryDirectory(prefix="gti-cthread-once-") as directory:
        output = pathlib.Path(directory)
        for mode, std, opt, sanitizers in modes:
            native = output / f"native-{mode}"
            run([cxx, f"-std={std}", f"-{opt}", *sanitizers,
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", root / "runtime/include", "-I", root / "runtime/src",
                 root / "runtime/src/cthread_state.cpp",
                 root / "tests/runtime_cthread_once_tests.cpp", "-o", native])
            run([native])
            for terminal in ("throws", "incomplete", "null"):
                run([native, terminal], 86)
            for fixture, status, stdout in cases:
                binary = output / f"{fixture}-{mode}"
                run([compiler, fixtures / f"{fixture}.gti", "--std", std,
                     f"-{opt}", "--progress=never", "--keep-cpp", "--execution-profile", "concurrent", "-o", binary,
                     "--", *sanitizers, fixtures / "cthread_once_context.cpp"])
                execute(binary, status, stdout)
                generated = binary.with_suffix(".gti.cpp").read_text()
                markers = ["// GTI verified-MIR body:"]
                if fixture not in ("cthread_once_null_initializer", "immobile_fresh_construction"):
                    markers += ["gti_rt_once_enter_v1(&__gti_native_callback_",
                                "gti_rt_once_leave_v1(&__gti_native_callback_"]
                for marker in markers:
                    if marker not in generated:
                        raise AssertionError(f"{fixture} lost {marker!r}")
            print(f"C-thread once: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
