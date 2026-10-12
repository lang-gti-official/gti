#!/usr/bin/env python3
"""Differential evidence for frontend text constants and verified-MIR execution."""

import pathlib
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = pathlib.Path(sys.argv[1]).resolve()
    source = pathlib.Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory(prefix="gti-constexpr-text-") as temporary:
        root = pathlib.Path(temporary)
        for optimization in ("O0", "O1", "O3"):
            for standard in ("c++20", "c++23"):
                executable = root / f"text-{optimization}-{standard}"
                for command in (
                    [str(compiler), str(source), f"-{optimization}", "--std",
                     standard, "-o", str(executable)],
                    [str(executable)],
                ):
                    result = subprocess.run(command, capture_output=True, text=True)
                    if result.returncode != 0 or (
                        len(command) == 1 and (result.stdout or result.stderr)
                    ):
                        print(f"failed ({result.returncode}): {command}\n{result.stdout}{result.stderr}",
                              file=sys.stderr)
                        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
