#!/usr/bin/env python3
"""Public specialized iterator aliases preserve MIR execution and cleanup."""

import pathlib
import subprocess
import sys
import tempfile
from harness_timeouts import scaled


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-qualified-members-") as directory:
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                output = pathlib.Path(directory) / f"example-{standard}-{optimization}"
                built = subprocess.run(
                    [str(compiler), str(source), "--std", standard,
                     f"-{optimization}", "-o", str(output)],
                    capture_output=True, text=True, timeout=scaled(180),
                )
                assert built.returncode == 0, built.stdout + built.stderr
                executed = subprocess.run(
                    [str(output)], capture_output=True, text=True, timeout=scaled(30),
                )
                assert executed.returncode == 0, executed.stderr
                assert executed.stdout == "15\nlayer released\nlayer released\n", executed
                assert executed.stderr == "", executed
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
