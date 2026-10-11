#!/usr/bin/env python3
"""Class allocation: distinct storage, constructor, and owner lifetimes."""

import os
import pathlib
import subprocess
import sys
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS
from harness_timeouts import scaled

def run(command):
    environment = os.environ.copy()
    # Successful output cannot excuse a recoverable sanitizer report.
    for variable in ("ASAN_OPTIONS", "UBSAN_OPTIONS"):
        previous = environment.get(variable, "")
        environment[variable] = (previous + ":" if previous else "") + "halt_on_error=1"
    return subprocess.run(command, capture_output=True, text=True, timeout=scaled(120),
                          env=environment)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unique_class_allocation_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    source = fixtures / "unique_class_allocation.gti"
    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
    allocation_site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    constructor_site = next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                            if "result(9 / divisor" in line)
    field_site = next(i for i, line in enumerate(source.read_text().splitlines(), 1)
                      if "5 / field_divisor" in line)
    modes = [(f"{standard}-{level}", [f"-{level}", "--std", standard])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["-O1", "--std", "c++23"]))
    with tempfile.TemporaryDirectory(prefix="gti-class-allocation-") as directory:
        for mode, flags in modes:
            binary = pathlib.Path(directory) / mode
            native = [str(fixtures / "unique_scalar_allocation_fault.cpp")]
            if mode == "sanitized":
                native += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), "--", *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI verified-MIR body:" in generated
            assert "mir_object_allocation_v1<::__gti_program::Value>" in generated
            assert ".allocate();" in generated and ".release());" in generated
            assert "::backend::make_unique<::__gti_program::Value>" not in generated
            assert "mir_object_allocation_v1<::__gti_program::Implicit>" in generated
            assert "::backend::make_unique<::__gti_program::Implicit>" not in generated
            # Constructor parameters outlive completed fields on failure.
            # The enclosing Value destructor runs only after success.
            cases = [([], (0, "2\n6\n5\n4\n3\n1\n", "")),
                     (["oom"], (ABORT_STATUS, "",
                      f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                      "allocation_failure in unique_owner\n")),
                     (["constructor", "failure"], (70, "5\n4\n2\n3\n1\n",
                      f"{source.name}:{constructor_site}: runtime error[GTI-R0002]: "
                      "division_by_zero in integer_division\n")),
                     (["partial", "field", "construction", "failure"],
                      (70, "4\n2\n3\n1\n",
                       f"{source.name}:{field_site}: runtime error[GTI-R0002]: "
                       "division_by_zero in integer_division\n")),
                     (["secondary", "allocation", "failure"], (ABORT_STATUS, "cleanup\n",
                      f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                       "allocation_failure in unique_owner\n")),
                     (["implicit"] * 5, (ABORT_STATUS, "",
                      f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                      "allocation_failure in unique_owner\n")),
                     (["implicit"] * 6, (0, "88\n77\n", ""))]
            for arguments, expected in cases:
                result = run([str(binary), *arguments])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    mode, arguments, result, expected)
            print(f"unique_class_allocation: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
