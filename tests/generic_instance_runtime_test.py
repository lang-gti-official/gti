#!/usr/bin/env python3
"""Concrete generics and nominal-field defaults through optimized-host GTI."""
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    compiler = Path(sys.argv[1]).resolve()
    root = Path(__file__).resolve().parent
    sources = [
        root / "coverage/reproducers/a03-generic-call-in-global-initializer.gti",
        root / "coverage/reproducers/a23-recursive-value-generic-layout.gti",
        root / "fixtures/nominal_field_defaults.gti",
        root.parent / "examples/23-direct-initialization.gti",
    ]
    failures = [root / "fixtures/nominal_field_default_failure.gti",
                root / "fixtures/nominal_field_explicit_failure.gti"]
    cells = 0
    with tempfile.TemporaryDirectory(prefix="gti-generic-instances-") as tmp:
        for standard in ("c++20", "c++23"):
            for optimization in ("O0", "O1", "O3"):
                for source in sources + failures:
                    target = Path(tmp) / f"{source.stem}-{standard}-{optimization}"
                    built = subprocess.run([
                        str(compiler), str(source), "--std", standard,
                        f"-{optimization}", "--progress", "never", "-o", str(target),
                    ], capture_output=True, text=True, timeout=90)
                    if built.returncode:
                        print(built.stdout + built.stderr, file=sys.stderr)
                        return 1
                    ran = subprocess.run([str(target)], capture_output=True, text=True, timeout=10)
                    expected_failure = source in failures
                    valid = (ran.returncode == 70 and
                             ran.stdout == "earlier constructed\nearlier dropped\n" and
                             "GTI-R0007" in ran.stderr) if expected_failure else (
                             ran.returncode == 0 and not ran.stdout and not ran.stderr)
                    if not valid:
                        print(f"FAIL {target.name}: exit {ran.returncode}\n"
                              f"stdout: {ran.stdout}\nstderr: {ran.stderr}", file=sys.stderr)
                        return 1
                    cells += 1
    print(f"generic instance runtime: {cells} optimized native cells passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
