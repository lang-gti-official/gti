#!/usr/bin/env python3
"""C-style yield and identity on initial, managed, and manual workers."""

import pathlib
import subprocess
import sys
import tempfile

from sanitizer_support import thread_sanitizer_enabled

from native_symbol_spelling import native_spelling
from harness_timeouts import scaled


def execute(command, *, quiet=False):
    result = subprocess.run(command, capture_output=True, text=True, timeout=scaled(120))
    if result.returncode != 0 or (quiet and (result.stdout or result.stderr)):
        raise AssertionError(
            f"{command!r}: exit={result.returncode}\n"
            f"stdout={result.stdout!r}\nstderr={result.stderr!r}"
        )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cthread_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-cthread-") as temporary:
        root = pathlib.Path(temporary)
        modes = [
            (f"{standard}-{optimization}", ["--std", standard, f"-{optimization}"])
            for standard in ("c++20", "c++23")
            for optimization in ("O0", "O1", "O3")
        ]
        modes.append(("sanitized", ["--std", "c++23", "-O1"]))
        if thread_sanitizer_enabled():
            modes.append(("tsan", ["--std", "c++23", "-O1"]))
        for name, flags in modes:
            for fixture in (
                "cthread_yield", "cthread_yield_concurrent", "cthread_yield_call_count",
                "cthread_identity", "cthread_identity_concurrent",
            ):
                suffix = ".exe" if sys.platform == "win32" else ""
                binary = root / f"{fixture}-{name}{suffix}"
                command = [
                    str(compiler), str(fixtures / f"{fixture}.gti"),
                    *flags, "--keep-cpp", "-o", str(binary),
                ]
                if fixture.endswith("concurrent"):
                    command.extend(["--execution-profile", "concurrent"])
                native = []
                if fixture.endswith("call_count"):
                    native.append(str(fixtures / "cthread_yield_call_count.cpp"))
                    if sys.platform.startswith("linux"):
                        # ELF can extract the real runtime object before the
                        # trailing oracle input. Wrap the call instead of
                        # defining the runtime symbol a second time. Generated
                        # GTI C++ deliberately undefines user macros, so this
                        # define renames only the separate oracle definition.
                        native.extend([
                            "-Dgti_rt_cthread_yield_v1=__wrap_gti_rt_cthread_yield_v1",
                            "-Wl,--wrap=gti_rt_cthread_yield_v1",
                        ])
                if name == "sanitized":
                    native.extend([
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    ])
                if name == "tsan":
                    native.extend(["-fsanitize=thread", "-fno-omit-frame-pointer"])
                if native:
                    command.extend(["--", *native])
                execute(command)
                execute([str(binary)], quiet=True)
                emitted = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in emitted:
                    raise AssertionError("missing verified-MIR production witness")
                yield_call = native_spelling(emitted, "gti_rt_cthread_yield_v1")
                if emitted.count(f"{yield_call}(") < 2:
                    raise AssertionError("yield lost its native call or declaration")
                if fixture.startswith("cthread_identity"):
                    current_call = native_spelling(emitted, "gti_rt_cthread_current_v1")
                    if emitted.count(f"{current_call}(") < 2:
                        raise AssertionError("identity lost its native call or declaration")
            print(f"cthread yield/identity: {name} passed", flush=True)


if __name__ == "__main__":
    main()
