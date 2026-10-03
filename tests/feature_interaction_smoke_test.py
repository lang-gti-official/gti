#!/usr/bin/env python3
"""Real compiler matrix for the bounded feature-interaction generator."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: feature_interaction_smoke_test.py <gti> <cxx> <repo-root>", file=sys.stderr)
        return 2
    gti, cxx, repository = sys.argv[1:]
    repository = Path(repository).resolve()
    script = repository / "scripts/debug/feature_interactions.py"
    # Keep failure evidence; only passing CTest runs release their temporary
    # directory. A CI artifact collector can use the printed path on failure.
    output = Path(tempfile.mkdtemp(prefix="gti-feature-interaction-smoke-"))
    completed = subprocess.run(
        [sys.executable, str(script), "run", "--smoke", "--seed", "20261003",
         "--gti", str(Path(gti).absolute()), "--cxx", cxx, "--out", str(output / "run")],
        capture_output=True, text=True, timeout=540, check=False,
    )
    print(completed.stdout, end="")
    print(completed.stderr, end="", file=sys.stderr)
    if completed.returncode != 0:
        print(f"retained failing matrix: {output}", file=sys.stderr)
        return 1
    report = json.loads((output / "run/report.json").read_text(encoding="utf-8"))
    if (report["verified_cases"] != 3 or report["native_outcomes"] != 24
            or report["negative_controls"] != 2 or report["failures"]):
        raise RuntimeError(f"incomplete compiler matrix: {report}")
    # The full constrained coverage belongs to the opt-in campaign. Do not
    # advertise these three deliberately bounded cases as a pairwise proof.
    if not report["verified_coverage"]["missing"]:
        raise RuntimeError("smoke unexpectedly claimed complete pairwise coverage")
    import shutil
    shutil.rmtree(output)
    print("feature interaction smoke: 24 native outcomes and two admission controls passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
