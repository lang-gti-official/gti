#!/usr/bin/env python3
"""Public specialized iterator aliases preserve MIR execution and cleanup."""

import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def main():
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-qualified-members-") as directory:
        for mode, flags, native in native_modes():
            output = pathlib.Path(directory) / f"example-{mode}"
            built = subprocess.run(
                [str(compiler), str(source), *flags, "-o", str(output)],
                capture_output=True, text=True, timeout=180,
            )
            assert built.returncode == 0, built.stdout + built.stderr
            executed = subprocess.run(
                [str(output)], capture_output=True, text=True, timeout=30,
            )
            assert executed.returncode == 0, executed.stderr
            assert executed.stdout == "15\nlayer released\nlayer released\n", executed
            assert executed.stderr == "", executed
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
