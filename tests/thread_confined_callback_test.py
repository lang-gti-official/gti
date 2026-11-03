#!/usr/bin/env python3
"""Thread confinement, optimizer-visible binding and native failure firewall."""
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
            binary = pathlib.Path(temporary) / f"callback-{index}"
            run([compiler, fixtures / "thread_confined_callbacks.gti", "--progress=never", *flags,
                 "--execution-profile", "concurrent", *keep_cpp(flags), "-o", binary,
                 "--", *sanitizers, fixtures / "thread_confined_context.cpp"])
            if is_cpp_mode(flags):
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "__gti_bind_native_callback_" in generated
                assert "gti_rt_confined_callback_enter_v1" in generated
            for mode, status in ((0, 0), (1, 86), (2, 70), (3, 86), (4, 70), (5, 86)):
                env = dict(os.environ, GTI_CONFINED_TEST_MODE=str(mode),
                           ASAN_OPTIONS="halt_on_error=1:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                result = subprocess.run([binary], env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == status, (label, mode, result)
                assert result.stdout == ("callback-cleaned\n" if status == 70 else ""), result
                assert ("GTI-R0002" in result.stderr) if status == 70 else not result.stderr, result
            print(f"thread-confined callbacks: {label} passed", flush=True)

if __name__ == "__main__":
    main()
