#!/usr/bin/env python3
"""Scalar make_unique initialization, ownership, and injected exhaustion."""

import pathlib
import subprocess
import sys
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS
from backend_modes import is_cpp_mode, keep_cpp, native_modes

# The fault provider (tests/fixtures/unique_scalar_allocation_fault.cpp) replaces
# the standard nothrow operator new. Generated C++ and the runtime owner pair
# used by the LLVM backend both allocate owners through it, so every backend
# observes the injected failure (lang-gti-official/gti#200).

def expected_result(suffix, site):
    failure = (f"<std/detail/memory_core>:{site}: runtime error[GTI-R0011]: "
               "allocation_failure in unique_owner\n")
    if suffix == "_failure":
        return (ABORT_STATUS, "argument\n", failure)
    if suffix == "_cleanup_failure":
        return (ABORT_STATUS, "cleanup\n", failure)
    return (0, "", "")


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unique_scalar_allocation_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    memory = fixtures.parent.parent / "stdlib/std/detail/memory_core.gti"
    site = next(i for i, line in enumerate(memory.read_text().splitlines(), 1)
                if "return unique_ptr<T>(gti_internal::allocate_unique_owner" in line)
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-scalar-allocation-") as temporary:
        for mode, flags, native_flags in modes:
            suffixes = ("", "_failure", "_cleanup_failure")
            for suffix in suffixes:
                name = "unique_scalar_allocation" + suffix
                binary = pathlib.Path(temporary) / f"{name}-{mode}"
                command = [str(compiler), str(fixtures / f"{name}.gti"), *flags,
                           *keep_cpp(flags), "-o", str(binary)]
                native = [str(fixtures / "unique_scalar_allocation_fault.cpp")]
                if mode == "sanitized":
                    native += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if native:
                    command += ["--", *native]
                built = run(command)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                if is_cpp_mode(flags):
                    generated = binary.with_suffix(".gti.cpp").read_text()
                    assert "// GTI verified-MIR body:" in generated
                    assert "::gti_internal::backend::mir_allocate_unique_scalar_v1<" in generated
                    assert "GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1" in generated
                result = run([str(binary)])
                expected = expected_result(suffix, site)
                if is_cpp_mode(flags):
                    if suffix == "":
                        # Both source types must coexist as distinct native generic
                        # arguments; separate translation units hide this collision.
                        assert "unique_ptr<char8_t>" in generated
                        assert "unique_ptr<std::uint8_t>" in generated
                        # A source raw pointer remains a semantic scalar while its
                        # native representation is an inert address carrier class.
                        assert "raw_address<std::int32_t>" in generated
                        assert "std::is_scalar_v<T> || is_raw_address_v<T>" in generated
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, mode, result, expected)
                print(f"{name}: {mode} passed", flush=True)
        # The LLVM backend also owns null pointers, native function pointers and
        # integral enums; its O2 pipeline is checked here beside the O0/O3
        # modes above, through the same operator-new fault seam.
        for suffix in ("", "_failure", "_cleanup_failure"):
            name = "unique_scalar_allocation" + suffix
            binary = pathlib.Path(temporary) / f"{name}-llvm-O2"
            command = [str(compiler), str(fixtures / f"{name}.gti"), "--backend", "llvm",
                       "-O2", "-o", str(binary), "--",
                       str(fixtures / "unique_scalar_allocation_fault.cpp")]
            built = run(command)
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            result = run([str(binary)])
            assert (result.returncode, result.stdout, result.stderr) == expected_result(
                suffix, site), (name, "O2", result)
            print(f"{name}: llvm-O2 passed", flush=True)

if __name__ == "__main__":
    main()
