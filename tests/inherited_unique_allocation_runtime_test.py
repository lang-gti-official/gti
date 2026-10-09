#!/usr/bin/env python3
"""Observe base initialization, failure propagation, and unpublished cleanup."""

import pathlib
import sys
import tempfile

from unique_class_allocation_runtime_test import run
from allocation_abort_runtime_test import ABORT_STATUS


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: inherited_unique_allocation_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    source = fixtures / "inherited_unique_allocation.gti"
    lines = source.read_text().splitlines()
    base_site = next(i for i, line in enumerate(lines, 1) if "7 / divisor(1)" in line)
    derived_site = next(i for i, line in enumerate(lines, 1) if "9 / divisor(2)" in line)
    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
    allocation_site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                           if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    modes = [(f"{standard}-{level}", [f"-{level}", "--std", standard])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["-O1", "--std", "c++23"]))
    with tempfile.TemporaryDirectory(prefix="gti-inherited-allocation-") as directory:
        root = pathlib.Path(directory)
        mir = root / "inherited.mir"
        emitted = run([str(compiler), str(source), "--emit-mir", "-o", str(mir)])
        assert emitted.returncode == 0, emitted
        assert "initialize-base" in mir.read_text()
        for mode, flags in modes:
            binary = root / mode
            native = [str(fixtures / "unique_scalar_allocation_fault.cpp")]
            if mode == "sanitized":
                native += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            command = [str(compiler), str(source), *flags, "--keep-cpp",
                       "-o", str(binary), "--", *native]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            generated = binary.with_suffix(".gti.cpp").read_text()
            assert "// GTI MIR base initialization " in generated
            assert " = *__gti_mir_constructor_success;" in generated
            assert "mir_object_allocation_v1<::__gti_program::Derived>" in generated
            cases = [([], (0, "10\n20\n1\n9\n", "")),
                     (["base"], (70, "10\n1\n9\n",
                      f"{source.name}:{base_site}: runtime error[GTI-R0002]: "
                      "division_by_zero in integer_division\n")),
                     (["derived", "failure"], (70, "10\n20\n1\n9\n",
                      f"{source.name}:{derived_site}: runtime error[GTI-R0002]: "
                      "division_by_zero in integer_division\n")),
                     (["allocation", "before", "base"], (ABORT_STATUS, "",
                      f"<std/memory>:{allocation_site}: runtime error[GTI-R0011]: "
                      "allocation_failure in unique_owner\n"))]
            for arguments, expected in cases:
                result = run([str(binary), *arguments])
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    mode, arguments, result, expected)
            print(f"inherited_unique_allocation: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
