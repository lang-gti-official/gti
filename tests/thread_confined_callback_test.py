#!/usr/bin/env python3
"""Thread confinement, optimizer-visible binding and native failure firewall."""
import os
import pathlib
import subprocess
import sys
import tempfile
from cthread_mutex_test import run

def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    cxx = pathlib.Path(sys.argv[2]).absolute()
    root = pathlib.Path(__file__).resolve().parent.parent
    fixtures = root / "tests/fixtures"
    modes = [(std, opt, []) for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("c++23", "O1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-confined-") as temporary:
        for index, (std, opt, sanitizers) in enumerate(modes):
            native = pathlib.Path(temporary) / f"native-{index}"
            run([cxx, f"-std={std}", f"-{opt}", *sanitizers, "-pthread",
                 "-Wall", "-Wextra", "-Werror", "-I", root / "runtime/include",
                 "-I", root / "runtime/src", root / "runtime/src/cthread_state.cpp",
                 root / "tests/runtime_confined_callback_tests.cpp", "-o", native])
            run([native])
            for boundary in ("before-begin", "zero", "unbalanced", "foreign", "scope-ended", "after-end"):
                run([native, boundary], 86)
            binary = pathlib.Path(temporary) / f"callback-{index}"
            run([compiler, fixtures / "thread_confined_callbacks.gti", "--progress=never", "--std", std,
                 f"-{opt}", "--execution-profile", "concurrent", "--keep-cpp", "-o", binary,
                 "--", *sanitizers, fixtures / "thread_confined_context.cpp"])
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "__gti_bind_native_callback_" in generated
            assert "gti_rt_confined_callback_enter_v1" in generated
            for mode, status in ((0, 0), (1, 86), (2, 70), (3, 86), (4, 70), (5, 86)):
                env = dict(os.environ, GTI_CONFINED_TEST_MODE=str(mode),
                           ASAN_OPTIONS="halt_on_error=1:abort_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                result = subprocess.run([binary], env=env, capture_output=True, text=True, timeout=30)
                assert result.returncode == status, (std, opt, mode, result)
                assert result.stdout == ("callback-cleaned\n" if status == 70 else ""), result
                assert ("GTI-R0002" in result.stderr) if status == 70 else not result.stderr, result
            print(f"thread-confined callbacks: {std} {opt} {sanitizers} passed", flush=True)

if __name__ == "__main__":
    main()
