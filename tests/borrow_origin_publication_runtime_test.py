#!/usr/bin/env python3
"""Borrowed text, field reborrows and returned carriers preserve native values."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

from case_jobs import configured_jobs, positive_jobs, run_cases
from sanitizer_support import scaled_timeout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=Path)
    parser.add_argument("--jobs", type=positive_jobs, default=configured_jobs())
    args = parser.parse_args()
    compiler = args.compiler.resolve()
    fixtures = Path(__file__).parent / "fixtures"
    environment = dict(os.environ)
    environment["GTI_STDLIB_PATH"] = str(fixtures.parent.parent / "stdlib")
    # Keep the direct stores under LLVM and the separate C++ read companion
    # explicit: #70's existing local reference-field store bug is not evidence
    # against or a substitute for the complete MIR publication contract.
    profiles = (("borrowed_text_field_reborrow", ("llvm", "cpp")),
                ("receiver_field_carriers", ("llvm",)),
                ("receiver_field_carriers_read", ("cpp",)))
    with tempfile.TemporaryDirectory(prefix="gti-borrow-origin-") as temporary:
        root = Path(temporary)

        def compile_case(case):
            name, backend, level = case
            binary = root / f"{name}-{backend}-{level}"
            command = [str(compiler), str(fixtures / f"{name}.gti"),
                       "--backend", backend, f"-{level}", "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True,
                                   env=environment, timeout=scaled_timeout(120))
            assert built.returncode == 0, (command, built.stdout, built.stderr)
            return binary

        def execute(binary):
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env=environment, timeout=scaled_timeout(30))
            assert (result.returncode, result.stdout, result.stderr) == (0, "", ""), (
                binary, result)

        cases = [(f"{name}/{backend}-{level}", (name, backend, level))
                 for name, backends in profiles for backend in backends
                 for level in ("O0", "O3")]
        run_cases(cases, compile_case, args.jobs, execute)


if __name__ == "__main__":
    main()
