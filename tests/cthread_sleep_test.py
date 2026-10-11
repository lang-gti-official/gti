#!/usr/bin/env python3
"""Relative sleep, exact interruption publication, aliasing, and host boundary."""

import os
import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import thread_sanitizer_enabled

from native_symbol_spelling import native_spelling
from harness_timeouts import scaled


def run(command):
    environment = os.environ.copy()
    environment.update(ASAN_OPTIONS="halt_on_error=1:abort_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1",
                       TSAN_OPTIONS="halt_on_error=1:abort_on_error=1")
    result = subprocess.run([str(part) for part in command], capture_output=True,
                            text=True, timeout=scaled(120), env=environment)
    if result.returncode != 0 or result.stderr:
        raise AssertionError(f"{command!r}: {result.returncode}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: cthread_sleep_test.py <gti> <cxx>")
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
    with tempfile.TemporaryDirectory(prefix="gti-cthread-sleep-") as directory:
        output = pathlib.Path(directory)
        for mode, std, opt, sanitizers in modes:
            native = output / f"native-{mode}"
            run([cxx, f"-std={std}", f"-{opt}", *sanitizers,
                 "-Wall", "-Wextra", "-Werror", "-pthread",
                 "-I", root / "runtime/include", "-I", root / "runtime/src",
                 root / "runtime/src/cthread_sleep.cpp",
                 root / "tests/runtime_cthread_sleep_tests.cpp", "-o", native])
            run([native])
            for fixture in ("cthread_sleep", "cthread_sleep_oracle", "cthread_sleep_concurrent"):
                binary = output / f"{fixture}-{mode}"
                command = [compiler, fixtures / f"{fixture}.gti", "--progress=never", "--std", std,
                           f"-{opt}", "--keep-cpp", "-o", binary]
                if fixture.endswith("concurrent"):
                    command += ["--execution-profile", "concurrent"]
                extra = [*sanitizers]
                if fixture.endswith("oracle"):
                    extra += [fixtures / "cthread_sleep_oracle.cpp"]
                if extra:
                    command += ["--", *extra]
                run(command)
                run([binary])
                generated = binary.with_suffix(".gti.cpp").read_text()
                if ("// GTI verified-MIR body:" not in generated or
                        generated.count(
                            f"{native_spelling(generated, 'gti_rt_cthread_sleep_v1')}(") < 2):
                    raise AssertionError(f"{fixture} lost its MIR/native-call witness")
            print(f"C-thread sleep: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
