#!/usr/bin/env python3
"""Immediate borrowed receivers: full-expression ownership and failure cleanup."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import is_cpp_mode, keep_cpp, native_modes


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def check_execution(binary, source):
    lines = source.read_text().splitlines()
    primary_line = next(index for index, line in enumerate(lines, 1)
                        if line.startswith("void checked("))
    cleanup_line = next(index for index, line in enumerate(lines, 1)
                        if line.startswith("void checked_cleanup("))
    for arguments in range(9):
        result = run([str(binary), *(["failure"] * arguments)])
        failure = arguments in range(1, 6) or arguments == 7
        output = ("100\n200\n100\n300\n100\n100\n200\n100\n100\ntoken\n100\n900\n"
                  if arguments == 0 else "900\n" if arguments == 8
                  else "100\n" if arguments == 4 else "100\n900\n")
        assert (result.returncode, result.stdout) == (70 if failure else 0, output), (
            binary.name, arguments, result.returncode, result.stdout, result.stderr
        )
        if failure:
            assert "GTI-R0002" in result.stderr, result.stderr
            assert ("GTI-R0014" in result.stderr) == (arguments == 4), result.stderr
            if arguments == 4:
                # A second failure terminates cleanup immediately, retaining
                # the first failure as primary; Resource is not destroyed.
                assert result.stderr.startswith(
                    f"{source.name}:{cleanup_line}: runtime error[GTI-R0014]"), result.stderr
                assert (f"primary [GTI-R0002] at {source.name}:{primary_line}; "
                        "secondary [GTI-R0002]") in result.stderr, result.stderr
            else:
                line = cleanup_line if arguments == 5 else primary_line
                assert result.stderr.startswith(
                    f"{source.name}:{line}: runtime error[GTI-R0002]"), result.stderr
        else:
            assert result.stderr == "", result.stderr


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures/ephemeral_carrier_receiver.gti"
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-ephemeral-carrier-") as temporary:
        for name, flags, native in modes:
            binary = pathlib.Path(temporary) / name
            command = [str(compiler), str(source), *flags, *keep_cpp(flags), "-o", str(binary)]
            if native:
                command += ["--", *native]
            result = run(command)
            assert result.returncode == 0, (command, result.stdout, result.stderr)
            if is_cpp_mode(flags):
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated
            check_execution(binary, source)


if __name__ == "__main__":
    main()
