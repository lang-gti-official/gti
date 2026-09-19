#!/usr/bin/env python3
"""Scalar make_unique initialization, ownership, and injected exhaustion."""

import pathlib
import subprocess
import sys
import tempfile


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=120)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: unique_scalar_allocation_runtime_test.py <gti>")
    compiler = pathlib.Path(sys.argv[1]).resolve()
    fixtures = pathlib.Path(__file__).parent / "fixtures"
    memory = fixtures.parent.parent / "stdlib/std/memory.gti"
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
                expected = (0, "", "")
                if suffix == "_failure":
                    expected = (70, "argument\n2\n3\n1\n",
                                f"<std/memory>:{site}: runtime error[GTI-R0011]: "
                                "allocation_failure in unique_owner\n")
                elif suffix == "_cleanup_failure":
                    primary = next(i for i, line in enumerate(
                        (fixtures / f"{name}.gti").read_text().splitlines(), 1)
                        if "return 1 / divisor;" in line)
                    expected = (70, "cleanup\n",
                                f"<std/memory>:{site}: runtime error[GTI-R0014]: "
                                "failure_during_cleanup; primary [GTI-R0002] at "
                                f"{name}.gti:{primary}; secondary [GTI-R0011]\n")
                else:
                    # Both source types must coexist as distinct native generic
                    # arguments; separate translation units hide this collision.
                    assert "unique_ptr<char8_t>" in generated
                    assert "unique_ptr<std::uint8_t>" in generated
                assert (result.returncode, result.stdout, result.stderr) == expected, (
                    name, mode, result, expected)
                print(f"{name}: {mode} passed", flush=True)


if __name__ == "__main__":
    main()
