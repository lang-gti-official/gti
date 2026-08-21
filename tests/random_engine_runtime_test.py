#!/usr/bin/env python3

"""Build and run the <std/random> behavioural fixture.

The fixture asserts reference vectors for splitmix64 seeding followed by
xoshiro256++. Both are fully specified, so a correct port reproduces a fixed
sequence for a fixed seed; that is what catches the mistakes this code is
exposed to — a rotate in the wrong direction, a shift constant off by one,
seeding one state word instead of four. Statistical checks do not reliably
catch any of those, and a duplicate search catches none of them.

    python3 tests/random_engine_runtime_test.py <gti> <fixture.gti>
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

EXPECTED = "random: all checks passed"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    gti = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-random-") as scratch:
        binary = pathlib.Path(scratch) / "random_engine"
        build = subprocess.run(
            [str(gti), str(fixture), "-o", str(binary)],
            capture_output=True,
            text=True,
            timeout=300,
            check=False,
        )
        if build.returncode != 0:
            print("FAIL: fixture did not build", file=sys.stderr)
            print(build.stdout + build.stderr, file=sys.stderr)
            return 1

        run = subprocess.run(
            [str(binary)], capture_output=True, text=True, timeout=300, check=False
        )
        output = run.stdout + run.stderr
        if run.returncode != 0 or EXPECTED not in output:
            print(f"FAIL: fixture exited {run.returncode}", file=sys.stderr)
            print(output, file=sys.stderr)
            return 1

    print(EXPECTED)
    return 0


if __name__ == "__main__":
    sys.exit(main())
