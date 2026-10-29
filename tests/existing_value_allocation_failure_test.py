#!/usr/bin/env python3
"""Exhaustion aborts without dropping the prepared existing value or caller."""

import pathlib
import subprocess
import sys
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS

def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    site = next(index for index, line in enumerate(memory.read_text().splitlines(), 1)
                if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    with tempfile.TemporaryDirectory(prefix="gti-existing-value-allocation-") as temporary:
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O3"):
                binary = pathlib.Path(temporary) / (standard + level)
                command = [str(compiler), str(fixtures / "existing_value_allocation_failure.gti"),
                           "--std", standard, "-" + level, "--keep-cpp", "-o", str(binary),
                           "--", str(fixtures / "unique_scalar_allocation_fault.cpp")]
                built = subprocess.run(command, capture_output=True, text=True, timeout=120)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                representation = binary.with_suffix(".gti.cpp").read_text()
                assert "mir_object_allocation_v1<::__gti_program::Owner>" in representation
                assert "::backend::make_unique<::__gti_program::Owner>" not in representation
                for arguments, expected in (
                        ((), (0, "7\n7\n9\n", "")),
                        (("oom",), (ABORT_STATUS, "",
                         f"<std/detail/memory_core>:{site}: runtime error[GTI-R0011]: "
                         "allocation_failure in unique_owner\n"))):
                    observed = subprocess.run([str(binary), *arguments],
                                              capture_output=True, text=True, timeout=30)
                    assert (observed.returncode, observed.stdout, observed.stderr) == expected, (
                        standard, level, observed, expected)
    print("Existing-value allocation: 8 exact success/failure cleanup observations passed")


if __name__ == "__main__":
    main()
