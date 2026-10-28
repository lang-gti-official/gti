#!/usr/bin/env python3
"""Shared exhaustion aborts; ordinary final-release failure obeys MIR cleanup."""

import argparse
from pathlib import Path
import tempfile

from unique_class_allocation_runtime_test import run
from allocation_abort_runtime_test import ABORT_STATUS


def main():
    modes = [(f"{std}-{opt}", ["--std", std, f"-{opt}"], [])
             for std in ("c++20", "c++23") for opt in ("O0", "O1", "O3")]
    modes += [("no-elide", ["--std", "c++23", "-O0"], ["-fno-elide-constructors"]),
              ("sanitized", ["--std", "c++23", "-O1"],
               ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])]
    modes += [(f"llvm-{level}", ["--backend", "llvm", f"-{level}"], [])
              for level in ("O0", "O1", "O2", "O3")]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--mode", action="append", choices=[mode[0] for mode in modes])
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    source = fixtures / "shared_ownership_failure.gti"
    if arguments.mode:
        modes = [mode for mode in modes if mode[0] in arguments.mode]
    with tempfile.TemporaryDirectory(prefix="gti-shared-failure-") as temporary:
        for name, flags, native in modes:
            binary = Path(temporary) / name
            cpp_limitation = "--std" in flags
            if cpp_limitation:
                binary.write_bytes(b"preserved unsupported output")
            built = run([str(compiler), str(source), *flags, "-o", str(binary),
                         "--", str(fixtures / "shared_ownership_allocation_fault.cpp"), *native])
            if cpp_limitation:
                assert (built.returncode == 65 and "GTI-B0003" in built.stderr
                        and "wrapping atomic read-modify-write" in built.stderr
                        and binary.read_bytes() == b"preserved unsupported output"), (name, built)
                print(f"RECORDED C++ LIMITATION shared ownership failure: {name}", flush=True)
                continue
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            cases = [
                (70, "99\n", "empty_owner_access in member_access"),
                (ABORT_STATUS, "", "allocation_failure in private_storage"),
                (ABORT_STATUS, "", "allocation_failure in private_storage"),
                (70, "1\n2\n7\n99\n", "division_by_zero"),
                (0, "1\n2\n7\n12\n99\n", ""),
            ]
            for phase, (status, output, diagnostic) in enumerate(cases):
                result = run([str(binary), *(["phase"] * phase)])
                assert result.returncode == status and result.stdout == output, (
                    name, phase, result.returncode, result.stdout, result.stderr)
                assert diagnostic in result.stderr, (name, phase, result.stderr)
                code = ("GTI-R0008", "GTI-R0011", "GTI-R0011", "GTI-R0002", "")[phase]
                if code:
                    assert f"runtime error[{code}]" in result.stderr, (name, phase, result.stderr)
                if phase == 2:
                    assert "GTI-R0014" not in result.stderr, result.stderr
                    assert "GTI-R0002" not in result.stderr, result.stderr
                if not diagnostic:
                    assert not result.stderr, (name, phase, result.stderr)
            print(f"shared ownership failure: {name} passed", flush=True)
            constructor_binary = Path(temporary) / f"constructor-{name}"
            built = run([str(compiler), str(fixtures / "shared_ownership_constructor_failure.gti"),
                         *flags, "-o", str(constructor_binary)])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(constructor_binary)])
            assert result.returncode == 70 and result.stdout == "1\n7\n99\n", (name, result)
            assert "runtime error[GTI-R0002]" in result.stderr, (name, result)
            print(f"shared ownership constructor rollback: {name} passed", flush=True)
            weak_binary = Path(temporary) / f"weak-{name}"
            built = run([str(compiler), str(fixtures / "weak_ownership_failure.gti"),
                         *flags, "-o", str(weak_binary)])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            for args, status, output, code in [
                    ([], 70, "7\n99\n", "GTI-R0002"),
                    (["overflow-weak"], ABORT_STATUS, "", "GTI-R0011"),
                    (["overflow-lock", "overflow"], ABORT_STATUS, "", "GTI-R0011")]:
                result = run([str(weak_binary), *args])
                assert result.returncode == status and result.stdout == output, (name, args, result)
                assert f"runtime error[{code}]" in result.stderr, (name, args, result)
            print(f"weak ownership cleanup and overflow: {name} passed", flush=True)
            thread_binary = Path(temporary) / f"thread-{name}"
            built = run([str(compiler), str(fixtures / "shared_ownership_thread_cleanup_failure.gti"),
                         *flags, "--execution-profile", "concurrent", "-o", str(thread_binary),
                         "--", *native])
            assert built.returncode == 0, (name, built.stdout, built.stderr)
            result = run([str(thread_binary)])
            assert result.returncode == 70 and result.stdout == "7\n", (
                name, result.returncode, result.stdout, result.stderr)
            assert "runtime error[GTI-R0014]" in result.stderr, result.stderr
            assert "primary [GTI-R0003]" in result.stderr, result.stderr
            assert "secondary [GTI-R0002]" in result.stderr, result.stderr
            print(f"shared ownership thread cleanup: {name} passed", flush=True)


if __name__ == "__main__":
    main()
