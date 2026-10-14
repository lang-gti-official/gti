#!/usr/bin/env python3
"""Protocol propagation keeps exact results and cleanup in every mode."""

import argparse
import os
import pathlib
import subprocess
import tempfile

from sanitizer_support import SANITIZED_BUILD_TIMEOUT

# Each exit ends only the temporaries created before it, newest first, then
# the lexical locals; moved-from shells are never destroyed twice.
EXPECTED = "\n".join([
    "100", "20", "10", "1", "0", "30",
    "200", "910", "1", "10", "910",
    "300", "920", "10", "1", "20", "920",
    "400", "30", "3", "2", "0", "31",
    "500", "3", "930", "2", "30", "930",
    "600", "42", "1", "1", "0",
    "700", "1", "0", "31", "0", "42", "50", "0", "65", "0", "0",
    "800", "42", "0", "405", "9",
]) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    arguments = parser.parse_args()
    root = pathlib.Path(__file__).parent
    fixture = root / "fixtures/propagation_protocol.gti"
    example = root.parent / "examples/156-propagation-protocol.gti"
    lines = fixture.read_text().splitlines()
    panic_site = (f"{fixture.name}:"
                  f"{next(i + 1 for i, line in enumerate(lines) if 'std::panic(' in line)}:")

    modes = [(standard, level, []) for standard in ("20", "23")
             for level in ("0", "1", "3")]
    modes.append(("23", "1", ["-fsanitize=address,undefined",
                              "-fno-omit-frame-pointer"]))
    if arguments.quick:
        modes = [("23", "0", [])]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-propagation-protocol-") as temporary:
        for standard, level, native in modes:
            binary = pathlib.Path(temporary) / f"propagation-cpp{standard}-O{level}"
            command = [str(arguments.compiler.resolve()), str(fixture), "--std",
                       f"c++{standard}", f"-O{level}", "-o", str(binary)]
            if native:
                command += ["--", *native]
            built = subprocess.run(command, text=True, capture_output=True,
                                   env=environment,
                                   timeout=SANITIZED_BUILD_TIMEOUT if native else 300)
            assert built.returncode == 0, (command, built.stdout, built.stderr)

            result = subprocess.run([str(binary)], text=True, capture_output=True,
                                    env=environment, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (
                0, EXPECTED, ""), (standard, level, native, result)

            failed = subprocess.run([str(binary), "panic"], text=True,
                                    capture_output=True, env=environment, timeout=30)
            assert (failed.returncode, failed.stdout) == (70, "13\n4\n"), (
                standard, level, native, failed)
            assert "GTI-R0017" in failed.stderr, failed
            assert "unlucky protocol value" in failed.stderr, failed
            assert panic_site in failed.stderr, (panic_site, failed)

            example_binary = binary.with_name(binary.name + "-example")
            example_command = [str(arguments.compiler.resolve()), str(example),
                               "--std", f"c++{standard}", f"-O{level}", "-o",
                               str(example_binary)]
            if native:
                example_command += ["--", *native]
            built = subprocess.run(example_command, text=True, capture_output=True,
                                   env=environment,
                                   timeout=SANITIZED_BUILD_TIMEOUT if native else 300)
            assert built.returncode == 0, (example_command, built.stdout,
                                           built.stderr)
            result = subprocess.run([str(example_binary)], text=True,
                                    capture_output=True, env=environment, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (
                0, "25\nno point\nempty\n48\n2\n", ""), result
            print(f"propagation protocol: C++{standard}/O{level}"
                  f"{' sanitized' if native else ''}", flush=True)


if __name__ == "__main__":
    main()
