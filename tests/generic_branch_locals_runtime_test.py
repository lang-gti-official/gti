#!/usr/bin/env python3
"""Compile selected dependent branches through verified MIR and native output."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent
    sources = [root / "fixtures" / "generic_branch_locals.gti",
               root / "coverage" / "reproducers" / "a15-dependent-if-constexpr-local.gti"]
    with tempfile.TemporaryDirectory(prefix="gti-branch-locals-") as tmp:
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources:
                    target = Path(tmp) / f"{source.stem}-{standard}-{optimization}"
                    built = subprocess.run([
                        str(compiler), str(source), "--std", standard,
                        f"-{optimization}", "--progress", "never", "-o", str(target),
                    ], capture_output=True, text=True, timeout=60)
                    if built.returncode:
                        print(built.stdout + built.stderr, file=sys.stderr)
                        return 1
                    ran = subprocess.run([str(target)], capture_output=True, text=True, timeout=10)
                    if ran.returncode or ran.stdout or ran.stderr:
                        print(f"FAIL {target.name}: {ran.returncode} {ran.stdout}{ran.stderr}", file=sys.stderr)
                        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
