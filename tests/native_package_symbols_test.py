#!/usr/bin/env python3
"""Check exposed C functions against smoke executables and native archives."""

import argparse
from pathlib import Path
import platform
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / "packages"
    darwin = platform.system() == "Darwin"
    command = ["nm", "-gU"] if darwin else ["nm", "-g", "--defined-only"]
    result = subprocess.run(command + list(map(str, args.inputs)), check=True, capture_output=True, text=True)
    symbols = {line.split()[-1] for line in result.stdout.splitlines() if line.split()}
    if darwin:
        symbols = {name.removeprefix("_") for name in symbols}
    missing = []
    for package in ("glfw", "glad", "sqlite3", "cimgui", "zlib", "libpng", "assimp", "libgit2", "cli11"):
        source = (root / package / "src" / f"{package}.gti").read_text()
        # Public units are deliberately declarations only. Require at least one
        # signature so a formatting/parser mismatch cannot give a vacuous pass.
        names = set(re.findall(
            r"^[ \t]+(?:\[\[[^\n]+\]\]\s*)?[^;\n{}=]*?\b(\w+)\s*\([^;{}]*\)\s*;",
            source,
            re.M,
        ))
        if not names:
            raise SystemExit(f"no C declarations found in {package}")
        absent = sorted(names - symbols)
        if absent:
            missing.append(f"{package}: {', '.join(absent)}")
        print(f"{package}: {len(names)} functions, {len(absent)} missing")
    if missing:
        raise SystemExit("Unlinked public functions:\n" + "\n".join(missing))


if __name__ == "__main__":
    main()
