#!/usr/bin/env python3
"""Cross-process output determinism gate.

Runs the same compilation in separate processes and requires
byte-identical emitted C++ and MIR. Separate processes randomize heap addresses, so
any observable output drawn from address-keyed container iteration fails
here even when a single-process comparison would pass. This is the standing
guard for ADR 006's determinism rule.
"""

import argparse
import subprocess
import tempfile
from pathlib import Path


def emit(gti: str, source: Path, output: Path, mode: str) -> None:
    result = subprocess.run(
        [gti, str(source), mode, "--progress", "never", "-o", str(output)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"emit failed ({result.returncode}):\n{result.stdout}{result.stderr}"
        )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("gti")
    parser.add_argument("source", type=Path)
    parser.add_argument("--mir-runs", type=int, default=2)
    args = parser.parse_args()
    if args.mir_runs < 2:
        parser.error("--mir-runs must be at least two")
    gti, source = args.gti, args.source
    if not source.is_file():
        raise SystemExit(f"missing example source: {source}")

    with tempfile.TemporaryDirectory(prefix="gti-determinism-") as scratch:
        for mode, suffix, runs in (("--emit-cpp", "cpp", 2),
                                   ("--emit-mir", "mir", args.mir_runs)):
            first = Path(scratch) / f"first.{suffix}"
            emit(gti, source, first, mode)
            expected = first.read_bytes()
            for index in range(1, runs):
                following = Path(scratch) / f"following-{index}.{suffix}"
                emit(gti, source, following, mode)
                if following.read_bytes() != expected:
                    raise SystemExit(
                        f"emitted {suffix} differs between identical compilations; "
                        "an address-keyed container is leaking iteration order into output"
                    )
    print(f"output determinism: identical C++ and {args.mir_runs} MIR emissions across processes")


if __name__ == "__main__":
    main()
