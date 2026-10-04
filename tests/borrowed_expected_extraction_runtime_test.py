#!/usr/bin/env python3
"""Borrowed propagation/extraction preserves loans and exact cleanup in every mode."""

import argparse
import os
import pathlib
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("--quick", action="store_true")
    arguments = parser.parse_args()
    fixture = pathlib.Path(__file__).parent / "fixtures/borrowed_expected_extraction.gti"
    example = pathlib.Path(__file__).parent.parent / "examples/99-borrowed-expected.gti"
    lines = fixture.read_text().splitlines()

    def site(fragment):
        return f"{fixture.name}:{next(i + 1 for i, line in enumerate(lines) if fragment in line)}:"

    modes = [(standard, level, ["-fno-elide-constructors"])
             for standard in ("20", "23") for level in ("0", "1", "3")]
    modes.append(("23", "1", ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]))
    if arguments.quick:
        modes = [("23", "0", ["-fno-elide-constructors"])]
    cases = [
        (0, 0, "1\n2\n7\n1\n2\n7\n1\n80\n1\n80\n8\n", None),
        (1, 0, "1\n80\n3\n9\n7\n", None),
        (2, 0, "1\n80\n3\n9\n7\n", None),
        (3, 70, "1\n2\n9\n90\n7\n", site('lookup(owner, true).expect')),
        (4, 70, "1\n2\n9\n90\n7\n", site('Resource& alias = std::move(carrier).expect')),
        (5, 70, "1\n2\n9\n90\n7\n", site('Resource& alias = required(owner, true)')),
    ]
    environment = dict(os.environ, ASAN_OPTIONS="halt_on_error=1",
                       UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    with tempfile.TemporaryDirectory(prefix="gti-borrowed-extraction-") as temporary:
        for standard, level, native in modes:
            binary = pathlib.Path(temporary) / f"borrowed-cpp{standard}-O{level}"
            command = [str(arguments.compiler.resolve()), str(fixture), "--std",
                       f"c++{standard}", f"-O{level}", "-o", str(binary), "--", *native]
            built = subprocess.run(command, text=True, capture_output=True,
                                   env=environment, timeout=120)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            for mode, status, output, location in cases:
                result = subprocess.run([str(binary), *(["case"] * mode)], text=True,
                                        capture_output=True, env=environment, timeout=30)
                assert (result.returncode, result.stdout) == (status, output), (
                    standard, level, mode, result)
                if location is None:
                    assert result.stderr == "", result
                else:
                    assert "GTI-R0017" in result.stderr, result
                    assert "required borrowed resource" in result.stderr, result
                    assert location in result.stderr, (location, result)
                    assert "GTI-R0009" not in result.stderr, result
            print(f"borrowed expected: C++{standard}/O{level}, {len(cases)} outcomes", flush=True)
            example_binary = binary.with_name(binary.name + "-example")
            example_command = [str(arguments.compiler.resolve()), str(example), "--std",
                               f"c++{standard}", f"-O{level}", "-o", str(example_binary),
                               "--", *native]
            built = subprocess.run(example_command, text=True, capture_output=True,
                                   env=environment, timeout=120)
            assert built.returncode == 0, (example_command, built.stdout, built.stderr)
            result = subprocess.run([str(example_binary)], text=True, capture_output=True,
                                    env=environment, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, "login\n", ""), result


if __name__ == "__main__":
    main()
