#!/usr/bin/env python3
"""Owning constructor arguments of transient receivers/read-borrowed values."""

import pathlib
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def require_build(command):
    result = run(command)
    if result.returncode:
        raise AssertionError(
            f"{command!r}: exit {result.returncode}\n{result.stdout}{result.stderr}"
        )


def check_execution(binary):
    cases = (
        (0, 0, "11\n12\n13\n/foo/bar.txt\n"),
        (1, 70, ""),       # Inner construction fails; no receiver exists.
        (2, 70, ""),       # Outer construction fails; roll back fields only.
        (3, 70, "11\n"),   # Selected method fails; receiver drops once.
        (4, 70, "11\n"),   # Later argument fails before method invocation.
        (5, 70, "11\n"),   # Read-borrowed callee fails; caller drops receiver.
    )
    for arguments, status, output in cases:
        result = run([str(binary), *(["failure"] * arguments)])
        expected_error = (
            "temporary_constructor_argument.gti:5: runtime error[GTI-R0002]: "
            "division_by_zero in integer_division\n" if status else ""
        )
        if (result.returncode, result.stdout, result.stderr) != (
            status, output, expected_error
        ):
            raise AssertionError(
                f"{binary.name}, {arguments} arguments: "
                f"exit={result.returncode}, stdout={result.stdout!r}, "
                f"stderr={result.stderr!r}"
            )


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/temporary_constructor_argument.gti"
    with tempfile.TemporaryDirectory(prefix="gti-temporary-constructor-") as temporary:
        root = pathlib.Path(temporary)
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                binary = root / f"temporary-{standard}-{optimization}"
                require_build([
                    str(compiler), str(source), f"-{optimization}",
                    "--std", standard, "--keep-cpp", "-o", str(binary)
                ])
                check_execution(binary)
                generated = binary.with_suffix(".gti.cpp").read_text()
                if "end transferred argument representation" not in generated:
                    raise AssertionError("missing constructor transfer retirement")
                if "// GTI verified-MIR body:" not in generated:
                    raise AssertionError("missing verified-MIR emission witness")

        binary = root / "temporary-sanitized"
        require_build([
            str(compiler), str(source), "-O1", "--std", "c++23", "-o", str(binary),
            "--", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"
        ])
        check_execution(binary)


if __name__ == "__main__":
    main()
