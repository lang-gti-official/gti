#!/usr/bin/env python3
"""Panic/try formatting twins: ownership, caller blame, cleanup and output errors."""

import argparse
import os
import pathlib
import subprocess
import tempfile
from harness_timeouts import scaled


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    args = parser.parse_args()
    fixtures = pathlib.Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "panic_adoption.gti"
    lines = source.read_text().splitlines()
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")

    def site(marker):
        numbers = [n for n, line in enumerate(lines, 1)
                   if line.endswith("// " + marker)]
        assert len(numbers) == 1, marker
        return f'panic_adoption.gti:{numbers[0]}'

    def failure(marker, message):
        return (f'{site(marker)}: runtime error[GTI-R0017]: '
                f'panic: "{message}"\n')

    cleanup = "inner\nouter\n"
    invalid = "Formatting failed: invalid format"
    unsupported = "Formatting failed: unsupported argument; convert the value to a supported type"
    output = "Output failed: stdout write failed"
    cases = [(0, 0, "{raw}{{raw}}\n1 2 three\n", "")]
    for mode, marker, message in (
            (1, "invalid", invalid),
            (2, "count", "Formatting failed: argument count mismatch"),
            (3, "unsupported", unsupported),
            (4, "append-zero", invalid), (5, "append-spec", invalid),
            (6, "println-format", unsupported), (7, "wrapper", invalid)):
        cases.append((mode, 70, "", cleanup + failure(marker, message)))
    cases.append((8, 0, "", cleanup))
    for mode, marker in ((9, "println-io"), (10, "print-io"), (11, "raw-io"),
                         (12, "char-io"), (13, "integer-io"),
                         (14, "string-io"), (15, "float-io")):
        cases.append((mode, 70, "", cleanup + failure(marker, output)))
    cases.append((16, 0, "", cleanup))
    cases.append((17, 70, "",
                  f"{site('cleanup-output')}: runtime error[GTI-R0014]: "
                  f"failure_during_cleanup; primary [GTI-R0017] at {site('cleanup-primary')} "
                  f'message="{invalid}"; secondary [GTI-R0017] message="{output}"\n'))
    modes = [(f"cpp{standard}-O{level}", f"c++{standard}", f"-O{level}", [])
             for standard in (20, 23) for level in (0, 2, 3)]
    modes += [("no-elide", "c++23", "-O0", ["-fno-elide-constructors"]),
              ("sanitized", "c++23", "-O1", ["-fsanitize=address,undefined",
                                              "-fno-omit-frame-pointer"])]
    if args.quick:
        modes = modes[:1]
    with tempfile.TemporaryDirectory(prefix="gti-panic-adoption-") as temporary:
        for name, standard, level, native in modes:
            binary = pathlib.Path(temporary) / name
            command = [str(args.compiler.resolve()), str(source), "--std", standard,
                       level, "--keep-cpp", "-o", str(binary), "--",
                       str(fixtures / "panic_adoption_io.cpp"), *native]
            built = subprocess.run(command, capture_output=True, text=True,
                                   env=environment, timeout=scaled(180))
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            assert "// GTI verified-MIR body:" in binary.with_suffix(".gti.cpp").read_text()
            for mode, status, stdout, stderr in cases:
                command = [str(binary), *(["x"] * mode)]
                if mode >= 8:
                    # The only pipe reader is closed before launch. Use the
                    # platform pipe itself rather than replacing GTI runtime I/O.
                    read_fd, write_fd = os.pipe()
                    os.close(read_fd)
                    try:
                        ran = subprocess.run(command, stdout=write_fd,
                                             stderr=subprocess.PIPE, text=True,
                                             env=environment, timeout=scaled(20))
                    finally:
                        os.close(write_fd)
                    actual_stdout = ""
                else:
                    ran = subprocess.run(command, capture_output=True, text=True,
                                         env=environment, timeout=scaled(20))
                    actual_stdout = ran.stdout
                assert (ran.returncode, actual_stdout, ran.stderr) == (status, stdout, stderr), (
                    name, mode, ran.returncode, actual_stdout, ran.stderr, stderr)
            print(f"{name}: {len(cases)} panic adoption outcomes passed", flush=True)


if __name__ == "__main__":
    main()
