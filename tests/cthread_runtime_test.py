#!/usr/bin/env python3
"""C-style yield: real host calls, managed workers, and an exact-call oracle."""

import pathlib
import subprocess
import sys
import tempfile


def execute(command, *, quiet=False):
    result = subprocess.run(command, capture_output=True, text=True, timeout=120)
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
        for name, flags in modes:
            for fixture in (
                "cthread_yield", "cthread_yield_concurrent", "cthread_yield_call_count",
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
                if name == "sanitized":
                    native.extend([
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    ])
                if native:
                    command.extend(["--", *native])
                execute(command)
                execute([str(binary)], quiet=True)
                emitted = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in emitted:
                    raise AssertionError("missing verified-MIR production witness")
                if emitted.count("gti_rt_cthread_yield_v1(") < 2:
                    raise AssertionError("yield lost its native call or declaration")
            print(f"cthread yield: {name} passed", flush=True)


if __name__ == "__main__":
    main()
