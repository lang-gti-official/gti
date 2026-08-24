#!/usr/bin/env python3

"""Report whether each captured known-failure reproducer still fails.

    python3 tests/known_failures/check.py <gti>

Every .gti file here reproduces an open compiler bug. This helper compiles
each one and distinguishes crashes/internal errors, classified backend
limitations, clean diagnostics, and successful compilation. It is deliberately
not a CTest: these inputs describe open bugs and may still crash the compiler.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    gti = pathlib.Path(sys.argv[1]).resolve()
    directory = pathlib.Path(__file__).resolve().parent

    fixed = []
    for source in sorted(directory.glob("*.gti")):
        result = subprocess.run(
            [str(gti), "--emit-cpp", str(source), "-o", "/dev/null"],
            capture_output=True,
            text=True,
            timeout=120,
        )
        output = result.stderr + result.stdout
        if result.returncode < 0:
            status = f"STILL-FAILING (signal {-result.returncode}, crash)"
        elif "GTI-B0001" in output:
            status = "STILL-FAILING (GTI-B0001 internal error)"
        elif "GTI-B0003" in output:
            status = "CHANGED (GTI-B0003 classified backend limitation)"
        elif result.returncode != 0:
            # A clean diagnostic is the intended fixed state for the parser
            # crash; for the loan ICE the intended fixed state is a clean
            # compile. Either way, no crash and no B0001 means worth a look.
            status = "CHANGED (clean non-zero exit — review; may be fixed)"
        else:
            status = "FIXED (compiled cleanly)"
            fixed.append(source.name)
        print(f"{source.name:<42} {status}")

    if fixed:
        print(
            f"\n{len(fixed)} reproducer(s) now pass — graduate them into the "
            "owning suite and remove them here."
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
