#!/usr/bin/env python3
"""Cross-process output determinism gate.

Runs the same compilation twice in separate processes and requires
byte-identical emitted C++. Separate processes randomize heap addresses, so
any observable output drawn from address-keyed container iteration fails
here even when a single-process comparison would pass. This is the standing
guard for ADR 006's determinism rule.
"""

import subprocess
import sys
import tempfile
from pathlib import Path


def emit(gti: str, source: Path, output: Path) -> None:
    result = subprocess.run(
        [gti, str(source), "--emit-cpp", "-o", str(output)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"emit failed ({result.returncode}):\n{result.stdout}{result.stderr}"
        )


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: determinism_test.py /path/to/gti example.gti")
    gti, source = sys.argv[1], Path(sys.argv[2])
    if not source.is_file():
        raise SystemExit(f"missing example source: {source}")

    with tempfile.TemporaryDirectory(prefix="gti-determinism-") as scratch:
        first = Path(scratch) / "first.cpp"
        second = Path(scratch) / "second.cpp"
        emit(gti, source, first)
        emit(gti, source, second)
        if first.read_bytes() != second.read_bytes():
            raise SystemExit(
                "emitted C++ differs between two identical compilations; "
                "an address-keyed container is leaking iteration order "
                "into output"
            )
    print("output determinism: identical emitted C++ across processes")


if __name__ == "__main__":
    main()
