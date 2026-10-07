#!/usr/bin/env python3
"""Replay the generic discovery/alias/range issue batch across native modes."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent
    sources = [root / "coverage" / "reproducers" / name for name in (
        "a54-alias-constrained-generic-class-argument.gti",
        "a54-alias-constrained-generic-user-class.gti",
        "a57-generic-member-field-range-for-silent.gti",
        "a57-generic-nondependent-range-for-silent.gti",
    )]
    sources.append(root / "fixtures" / "generic_discovery_boundaries.gti")
    with tempfile.TemporaryDirectory(prefix="gti-generic-discovery-") as tmp:
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O2", "O3"):
                for source in sources:
                    target = Path(tmp) / f"{source.stem}-{standard}-{optimization}"
                    built = subprocess.run([
                        str(compiler), str(source), "--std", standard,
                        f"-{optimization}", "--progress", "never", "-o", str(target),
                    ], capture_output=True, text=True, timeout=60)
                    if built.returncode != 0:
                        print(f"FAIL build {target.name}: {built.stdout}{built.stderr}", file=sys.stderr)
                        return 1
                    ran = subprocess.run([str(target)], capture_output=True, text=True, timeout=10)
                    if ran.returncode or ran.stdout or ran.stderr:
                        print(f"FAIL run {target.name}: {ran.returncode} {ran.stdout}{ran.stderr}", file=sys.stderr)
                        return 1
        rejected = subprocess.run([
            str(compiler), str(root / "coverage" / "reproducers" / "a06-polymorphic-recursion-hang.gti"),
            "--emit-mir", "--progress", "never", "-o", str(Path(tmp) / "invalid.mir"),
        ], capture_output=True, text=True, timeout=20)
        if rejected.returncode != 65 or "error[GTI-S2105]" not in rejected.stderr:
            print(f"FAIL unbounded recursion: {rejected.stdout}{rejected.stderr}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
