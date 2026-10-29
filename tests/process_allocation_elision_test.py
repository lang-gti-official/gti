#!/usr/bin/env python3
"""Process-owner allocations can disappear; escaping owners still allocate."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def native_compiler_is_clang(build_output):
    """Whether the native command the driver printed under -v is Clang."""
    for line in build_output.splitlines():
        if line.startswith("+ "):
            native = line[2:].split()[0]
            version = subprocess.run([native, "--version"], capture_output=True,
                                     text=True, timeout=30)
            return "clang" in (version.stdout + version.stderr).lower()
    raise AssertionError("the driver did not print its native command")


def main():
    compiler = Path(sys.argv[1]).resolve()
    fixtures = Path(__file__).resolve().parent / "fixtures"
    with tempfile.TemporaryDirectory(prefix="gti-process-allocation-elision-") as directory:
        for standard in ("c++20", "c++23"):
            for level in ("O0", "O3"):
                binary = Path(directory) / f"{standard}-{level}"
                built = subprocess.run([
                    str(compiler), str(fixtures / "process_allocation_elision.gti"),
                    f"-{level}", "--std", standard, "--keep-cpp", "-v", "-o", str(binary),
                    "--", str(fixtures / "process_allocation_elision.cpp")],
                    capture_output=True, text=True, timeout=120)
                assert built.returncode == 0, built
                generated = binary.with_suffix(".gti.cpp").read_text()
                assert "T *storage = new (std::nothrow) T(args...);" in generated
                result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
                # The generated new-expression lets a native optimizer omit the
                # replaceable allocation of a non-escaping owner. Clang does at
                # O3; C++ permits but does not require it, and GCC keeps it. An
                # escaping owner must allocate under every compiler.
                if level == "O0":
                    accepted = {"1 1\n1 1\n"}
                elif native_compiler_is_clang(built.stdout + built.stderr):
                    accepted = {"0 0\n1 1\n"}
                else:
                    accepted = {"0 0\n1 1\n", "1 1\n1 1\n"}
                assert (result.returncode, result.stderr) == (0, "") and result.stdout in accepted, (
                    standard, level, result)
                print(f"Process allocation elision: {standard}/{level} passed", flush=True)
        if os.name == "nt":
            return
        # Native-backend owners allocate through the runtime's POSIX provider,
        # which the companion counts with the same probe. Elision of the
        # non-escaping owner is permitted but not required; the escaping owner
        # must allocate and release exactly once.
        for level in ("O0", "O2"):
            binary = Path(directory) / f"llvm-{level}"
            built = subprocess.run([
                str(compiler), str(fixtures / "process_allocation_elision.gti"),
                "--backend", "llvm", f"-{level}", "-o", str(binary),
                "--", str(fixtures / "process_allocation_elision.cpp")],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, built
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            accepted = {"1 1\n1 1\n"} if level == "O0" else {"0 0\n1 1\n", "1 1\n1 1\n"}
            assert (result.returncode, result.stderr) == (0, "") and result.stdout in accepted, (
                level, result)
            print(f"Process allocation elision: llvm/{level} passed", flush=True)


if __name__ == "__main__":
    main()
