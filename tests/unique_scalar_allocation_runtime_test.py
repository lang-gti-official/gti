#!/usr/bin/env python3
"""Scalar make_unique initialization, ownership, and injected exhaustion."""

import os
import pathlib
import subprocess
import sys
import tempfile

from allocation_abort_runtime_test import ABORT_STATUS

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
    modes = [(f"{standard}-{level}", [f"-{level}", "--std", standard])
             for standard in ("c++20", "c++23") for level in ("O0", "O1", "O3")]
    modes.append(("sanitized", ["-O1", "--std", "c++23"]))
    with tempfile.TemporaryDirectory(prefix="gti-scalar-allocation-") as temporary:
        for mode, flags in modes:
            for suffix in ("", "_failure", "_cleanup_failure"):
                name = "unique_scalar_allocation" + suffix
                binary = pathlib.Path(temporary) / f"{name}-{mode}"
                command = [str(compiler), str(fixtures / f"{name}.gti"), *flags,
                           "--keep-cpp", "-o", str(binary)]
                native = [str(fixtures / "unique_scalar_allocation_fault.cpp")]
                if mode == "sanitized":
                    native += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                if native:
                    command += ["--", *native]
                built = run(command)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "// GTI verified-MIR body:" in generated
                assert "::gti_internal::backend::mir_allocate_unique_scalar_v1<" in generated
                assert "GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1" in generated
                result = run([str(binary)])
                expected = expected_result(suffix, site)
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
        # The native backend owns the same scalar pointees, including a null
        # pointer, a native function pointer and an integral enum. Its owners
        # allocate through the runtime's POSIX provider rather than C++ operator
        # new. Its own companion fails that provider through the same switch,
        # so exhaustion and failure during cleanup are checked on both
        # backends. That provider is POSIX-only.
        suffixes = ("",) if os.name == "nt" else ("", "_failure", "_cleanup_failure")
        companion = "unique_scalar_allocation_fault.cpp" if os.name == "nt" else \
            "unique_scalar_runtime_allocation_fault.cpp"
        for suffix in suffixes:
            name = "unique_scalar_allocation" + suffix
            for level in ("O0", "O2"):
                binary = pathlib.Path(temporary) / f"{name}-llvm-{level}"
                command = [str(compiler), str(fixtures / f"{name}.gti"), "--backend", "llvm",
                           f"-{level}", "-o", str(binary), "--",
                           str(fixtures / companion)]
                built = run(command)
                assert built.returncode == 0, (command, built.stdout, built.stderr)
                result = run([str(binary)])
                assert (result.returncode, result.stdout, result.stderr) == expected_result(
                    suffix, site), (name, level, result)
                print(f"{name}: llvm-{level} passed", flush=True)


if __name__ == "__main__":
    main()
