#!/usr/bin/env python3
"""Node-owner algorithms and deterministic cleanup through verified MIR."""

import pathlib
import re
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: forward_list_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    modes = [
        (f"{standard}-{level}", [f"-{level}", "--std", standard])
        for standard in ("c++20", "c++23")
        for level in ("O0", "O1", "O3")
    ]
    modes.append(("sanitized", ["-O1", "--std", "c++23"]))
    with tempfile.TemporaryDirectory(prefix="gti-forward-list-") as temporary:
        for name, flags in modes:
            for fixture in (
                "forward_list_algorithms", "forward_list_cleanup", "owned_field_restore"
            ):
                binary = pathlib.Path(temporary) / f"{fixture}-{name}"
                command = [
                    str(compiler), str(fixtures / f"{fixture}.gti"),
                    *flags, "--keep-cpp", "-o", str(binary),
                ]
                if name == "sanitized":
                    command += [
                        "--", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer",
                    ]
                built = run(command)
                if built.returncode != 0:
                    raise AssertionError(
                        f"{fixture}, {name} build failed:\n{built.stdout}{built.stderr}"
                    )
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing verified-MIR emission witness")
                executions = 3 if fixture.endswith("cleanup") else (
                    2 if fixture == "owned_field_restore" else 1
                )
                for arguments in range(executions):
                    result = run([str(binary), *(["fail"] * arguments)])
                    expected_output = ""
                    valid = (result.returncode, result.stdout, result.stderr) == (0, "", "")
                    if fixture.endswith("cleanup"):
                        expected_output = "3\n2\n1\n0\n0\n5\n4\n" if arguments == 0 else "1\n2\n3\n"
                        valid = result.stdout == expected_output and (
                            (result.returncode == 0 and result.stderr == "")
                            if arguments == 0 else (
                                result.returncode == 70 and re.fullmatch(
                                    r".*forward_list_cleanup\.gti:\d+: runtime error"
                                    r"\[GTI-R0002\]: division_by_zero in integer_division\n",
                                    result.stderr,
                                ) is not None
                            )
                        )
                    elif fixture == "owned_field_restore":
                        expected_output = "2\n3\n1\n" if arguments == 0 else "4\n3\n1\n2\n"
                        valid = result.stdout == expected_output and (
                            (result.returncode == 0 and result.stderr == "")
                            if arguments == 0 else (
                                result.returncode == 70 and re.fullmatch(
                                    r".*owned_field_restore\.gti:\d+: runtime error"
                                    r"\[GTI-R0002\]: division_by_zero in integer_division\n",
                                    result.stderr,
                                ) is not None
                            )
                        )
                    if not valid:
                        raise AssertionError(
                            f"{fixture}, {name}, args={arguments}: "
                            f"exit={result.returncode}, stdout={result.stdout!r}, "
                            f"stderr={result.stderr!r}; expected output={expected_output!r}"
                        )
                print(f"{fixture}: {name} passed", flush=True)


if __name__ == "__main__":
    main()
