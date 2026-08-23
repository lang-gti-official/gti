#!/usr/bin/env python3

"""Build and run the <std/filesystem> path conformance fixture.

    python3 tests/filesystem_path_runtime_test.py <gti> <fixture.gti>

The fixture asserts the documented C++ std::filesystem::path results for the
same inputs, so it pins the decomposition, query, modifier, concatenation,
comparison, and normalization rules that a plausible-looking implementation
gets subtly wrong.
"""

from __future__ import annotations

import pathlib
import subprocess
import sys
import tempfile

EXPECTED = "filesystem path: all checks passed"


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    gti = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(sys.argv[2]).resolve()

    with tempfile.TemporaryDirectory(prefix="gti-filesystem-path-") as scratch:
        binary = pathlib.Path(scratch) / "filesystem_path"
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
            [str(binary)], capture_output=True, text=True, timeout=300,
            check=False
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
