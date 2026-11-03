#!/usr/bin/env python3
"""Differential evidence for frontend text constants and verified-MIR execution."""

import argparse
import pathlib
import subprocess
import sys
import tempfile
from backend_modes import native_modes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("compiler", type=pathlib.Path)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--ownership-checks", action="store_true")
    arguments = parser.parse_args()
    compiler = arguments.compiler.resolve()
    source = arguments.source.resolve()
    # --ownership-checks formerly added C++ sanitized and no-elide
    # configurations; the policy runs the C++ backend in one profile.
    modes = native_modes()
    with tempfile.TemporaryDirectory(prefix="gti-constexpr-text-") as temporary:
        root = pathlib.Path(temporary)
        for name, flags, native in modes:
            executable = root / f"text-{name}"
            for command in (
                [str(compiler), str(source), *flags, "-o", str(executable), *native],
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
