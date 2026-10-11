#!/usr/bin/env python3
"""MIR-emitted manual C-thread execution, attachment and failure boundaries."""

import os
import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import thread_sanitizer_enabled

from native_symbol_spelling import native_spelling
from harness_timeouts import scaled


def execute(command, expected=0, stdout=None, failure=False):
    environment = os.environ.copy()
    environment["ASAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
    environment["TSAN_OPTIONS"] = "halt_on_error=1:abort_on_error=1"
    result = subprocess.run(command, capture_output=True, text=True, timeout=scaled(120),
                            env=environment)
    if (result.returncode != expected or (stdout is not None and result.stdout != stdout) or
            (failure and ("GTI-R0002" not in result.stderr or
                          "cthread_failure.gti" not in result.stderr)) or
            (not failure and result.stderr)):
        raise AssertionError(f"{command!r}: {result.returncode}\n"
                             f"stdout={result.stdout!r}\nstderr={result.stderr!r}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cthread_create_join_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    cases = [
        ("cthread_create_join", [], 0, "", False, False),
        ("cthread_context", [], 0, "", False, True),
        ("cthread_nested", [], 0, "", False, True),
        ("cthread_owned_arguments", [], 0, "", False, False),
        ("cthread_failure", [], 70, "worker-cleaned\n", True, True),
        ("cthread_entry_rejected", [], 86, "", False, True),
        ("cthread_foreign_rejected", [], 86, "", False, True),
        ("cthread_outstanding", [], 86, "", False, True),
    ]
    modes = [(f"{standard}-{opt}", ["--std", standard, f"-{opt}"], [])
             for standard in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes.append(("asan-ubsan", ["--std", "c++23", "-O1"],
                  ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    if thread_sanitizer_enabled():
        modes.append(("tsan", ["--std", "c++23", "-O1"],
                      ["-fsanitize=thread", "-fno-omit-frame-pointer"]))
    with tempfile.TemporaryDirectory(prefix="gti-cthread-public-") as directory:
        root = pathlib.Path(directory)
        for mode, flags, native in modes:
            for index, (case, options, status, output, failure, helper) in enumerate(cases):
                binary = root / f"{mode}-{index}"
                command = [str(compiler), str(fixtures / f"{case}.gti"), "--progress=never",
                           "--execution-profile", "concurrent", *flags, *options,
                           "--keep-cpp", "-o", str(binary)]
                extra = [*native]
                if helper:
                    extra.append(str(fixtures / "cthread_context.cpp"))
                if extra:
                    command.extend(["--", *extra])
                execute(command)
                execute([str(binary)], status, output, failure)
                generated = binary.with_suffix(".gti.cpp").read_text()
                spell = lambda symbol: native_spelling(generated, symbol)
                for marker in ("// GTI verified-MIR body:",
                               f"{spell('gti_rt_cthread_begin_v1')}();",
                               f"{spell('gti_rt_cthread_end_v1')}();",
                               f"{spell('gti_rt_cthread_enter_v1')}(&__gti_native_callback_"):
                    if marker not in generated:
                        raise AssertionError(f"{case} lost {marker!r}")
            print(f"C-thread create/join: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
