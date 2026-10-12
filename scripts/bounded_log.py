#!/usr/bin/env python3
"""Display a bounded saved-log tail without changing the original evidence."""

from __future__ import annotations

import argparse
import stat
import sys
from pathlib import Path


MAX_LINES = 200
MAX_BYTES = 16 * 1024


def bounded_integer(low: int, high: int):
    def parse(value: str) -> int:
        try:
            number = int(value)
        except ValueError:
            raise argparse.ArgumentTypeError(f"must be an integer from {low} to {high}") from None
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f"must be an integer from {low} to {high}")
        return number

    return parse


def excerpt(path: Path, lines: int, max_bytes: int) -> bytes:
    if not 2 <= lines <= MAX_LINES or not 256 <= max_bytes <= MAX_BYTES:
        raise ValueError("excerpt limits outside permitted range")
    if not stat.S_ISREG(path.stat().st_mode):
        raise ValueError("log must be a regular file")
    with path.open("rb") as stream:
        size = stream.seek(0, 2)
        stream.seek(max(0, size - max_bytes))
        data = stream.read(max_bytes)
    header = (
        f"[bounded tail; original={size} bytes; limits={lines} lines/{max_bytes} bytes; "
        "full file unchanged]\n"
    ).encode("ascii")
    # Decode before selecting lines: replacement characters and Unicode line
    # separators must also obey the final displayed byte/line limits.
    records = data.decode("utf-8", errors="replace").splitlines()[-(lines - 1):]
    text = "\n".join(records)
    budget = max_bytes - len(header) - 1
    body = text.encode("utf-8")[-budget:].decode("utf-8", errors="ignore").encode("utf-8")
    body = body.rstrip(b"\r\n")
    return header + (body + b"\n" if body else b"")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--lines", type=bounded_integer(2, MAX_LINES), default=40)
    parser.add_argument("--max-bytes", type=bounded_integer(256, MAX_BYTES), default=8 * 1024)
    args = parser.parse_args()
    try:
        output = excerpt(args.log, args.lines, args.max_bytes)
    except (OSError, ValueError) as error:
        # A malformed or excessively long path must not defeat bounded output.
        print(f"bounded_log: {str(error)[:240]}", file=sys.stderr)
        return 1
    sys.stdout.buffer.write(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
