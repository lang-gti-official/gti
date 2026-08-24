#!/usr/bin/env python3

"""Check the wrong-code reproducers by building and *running* them.

The sibling `tests/known_failures/check.py` classifies by whether the compiler
fails. That is the wrong question here: these programs compile cleanly and
then compute the wrong answer, so a compile-only check calls every one of them
fixed. This builds each reproducer and compares its exit status against the
`// expected exit status: N` header on its first line.

    python3 tests/known_failures/wrong-code/check.py build/gti

A reproducer that now exits with its expected status is FIXED and should
graduate into the owning suite as a runtime regression test.
"""

from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HEADER = re.compile(r"//\s*expected exit status:\s*(\d+)")
LEVELS = ("O0", "O1", "O2", "O3")


def expected_status(source: Path) -> int | None:
    """The status declared on the reproducer's first line, if any."""
    with source.open(encoding="utf-8") as handle:
        match = HEADER.match(handle.readline().strip())
    return int(match.group(1)) if match else None


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(f"usage: {argv[0]} <path-to-gti> [c++]", file=sys.stderr)
        return 2
    gti = Path(argv[1]).resolve()
    if not gti.is_file():
        print(f"error: {gti} not found", file=sys.stderr)
        return 2
    cxx = argv[2] if len(argv) > 2 else (shutil.which("c++") or "c++")

    directory = Path(__file__).resolve().parent
    sources = sorted(directory.glob("*.gti"))
    if not sources:
        print("no wrong-code reproducers")
        return 0

    scratch = Path(tempfile.mkdtemp(prefix="gti-wrong-code-"))
    fixed = 0
    for source in sources:
        expected = expected_status(source)
        if expected is None:
            print(f"{source.name:<44} SKIPPED (no expected-status header)")
            continue

        # Every level, not just -O0. These reproduce at all of them, so a fix
        # that only lands on one would otherwise read as complete.
        statuses: dict[str, str] = {}
        for level in LEVELS:
            output = scratch / f"{source.stem}-{level}"
            built = subprocess.run(
                [str(gti), str(source), f"-{level}", "--std", "c++23",
                 "-o", str(output), "--cxx", cxx],
                capture_output=True, text=True, timeout=600)
            if built.returncode != 0:
                # A reproducer that stopped compiling is a change worth
                # seeing, not silently a pass.
                statuses[level] = "no-build"
                continue
            try:
                ran = subprocess.run([str(output)], capture_output=True,
                                     timeout=120)
            except subprocess.TimeoutExpired:
                statuses[level] = "hang"
                continue
            statuses[level] = str(ran.returncode)

        correct = {lvl for lvl, got in statuses.items() if got == str(expected)}
        if len(correct) == len(LEVELS):
            print(f"{source.name:<44} FIXED (exits {expected} at every level)")
            fixed += 1
        elif correct:
            # A partial fix is the outcome most worth shouting about.
            spread = ", ".join(f"{lvl}={statuses[lvl]}" for lvl in LEVELS)
            print(f"{source.name:<44} PARTIAL (expected {expected}; {spread})")
        else:
            spread = ", ".join(f"{lvl}={statuses[lvl]}" for lvl in LEVELS)
            print(f"{source.name:<44} STILL-WRONG (expected {expected}; "
                  f"{spread})")

    if fixed:
        print(f"\n{fixed} reproducer(s) now behave correctly — graduate them "
              "into the owning suite as runtime regressions and remove them "
              "here.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
