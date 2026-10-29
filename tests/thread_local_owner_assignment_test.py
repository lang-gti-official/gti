#!/usr/bin/env python3
"""Assigning to thread-local owners destroys each replaced value once.

A direct store to a thread-local owner once failed MIR verification because the
store's first-use failure outcomes, which belong to its EnsureThreadLocal
guard, suppressed the replacement's paired cleanup. The retained C++ backend
does not admit this shape and is not exercised.
"""

import pathlib
import subprocess
import sys
import tempfile


# Main thread: two assignments retire tokens 1 and 11. The worker retires its
# own initial token and 101 by assignment and 102 at thread exit. Process exit
# retires the main thread's final token 12.
EXPECTED = "1\n11\n1\n101\n102\n12\n"


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(__file__).parent / "fixtures" / "thread_local_owner_assignment.gti"
    with tempfile.TemporaryDirectory(prefix="gti-thread-local-assignment-") as temporary:
        for level in ("O0", "O1", "O2", "O3"):
            binary = pathlib.Path(temporary) / f"assignment-{level}"
            built = subprocess.run(
                [str(compiler), str(source), "--backend", "llvm", f"-{level}",
                 "--execution-profile", "concurrent", "--progress=never", "-o", str(binary)],
                capture_output=True, text=True, timeout=120)
            assert built.returncode == 0, (level, built.stdout, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            assert (result.returncode, result.stdout, result.stderr) == (0, EXPECTED, ""), (
                level, result)
            print(f"thread-local owner assignment: llvm-{level} passed", flush=True)


if __name__ == "__main__":
    main()
