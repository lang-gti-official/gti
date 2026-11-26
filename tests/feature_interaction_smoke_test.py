#!/usr/bin/env python3
"""Real LLVM compiler matrix for the bounded feature-interaction generator.

Three cases, both metamorphic variants, LLVM O0 and O3: twelve native outcomes.
The second argument is only the native link driver forwarded to `gti --cxx`.
"""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: feature_interaction_smoke_test.py <gti> <link-driver> <repo-root>", file=sys.stderr)
        return 2
    gti, link_driver, repository = sys.argv[1:]
    repository = Path(repository).resolve()
    script = repository / "scripts/debug/feature_interactions.py"
    # Keep failure evidence; only passing CTest runs release their temporary
    # directory. A CI artifact collector can use the printed path on failure.
    output = Path(tempfile.mkdtemp(prefix="gti-feature-interaction-smoke-"))
    print(f"matrix evidence: {output}", flush=True)
    completed = subprocess.run(
        [sys.executable, str(script), "run", "--smoke", "--keep-going", "--seed", "20261003",
         "--gti", str(Path(gti).absolute()), "--cxx", link_driver, "--out", str(output / "run")],
        capture_output=True, text=True, timeout=540, check=False,
    )
    print(completed.stdout, end="")
    print(completed.stderr, end="", file=sys.stderr)
    if completed.returncode != 0:
        print(f"retained failing matrix: {output}", file=sys.stderr)
        return 1
    report = json.loads((output / "run/report.json").read_text(encoding="utf-8"))
    if (report["verified_cases"] != 3 or report["native_outcomes"] != 12
            or report["negative_controls"] != 2 or report["failures"]
            or report["configurations"] != [["O0", "llvm"], ["O3", "llvm"]]):
        raise RuntimeError(f"incomplete compiler matrix: {report}")
    # The full constrained coverage belongs to the opt-in campaign. Do not
    # advertise these three deliberately bounded cases as a pairwise proof.
    if not report["verified_coverage"]["missing"]:
        raise RuntimeError("smoke unexpectedly claimed complete pairwise coverage")
    # The campaign's reduced historical findings stay executable regressions.
    sys.path.insert(0, str(repository / "tests"))
    from backend_modes import llvm_modes
    regressions = sorted((repository / "tests/fixtures").glob("feature_interaction_*.gti"))
    for fixture in regressions:
        for label, flags, _ in llvm_modes(levels=("O0", "O3")):
            executable = output / f"{fixture.stem}-{label}"
            built = subprocess.run([str(Path(gti).absolute()), str(fixture), *flags, "--cxx", link_driver,
                                    "-o", str(executable)], capture_output=True, text=True,
                                   timeout=120, check=False)
            ran = (subprocess.run([str(executable)], capture_output=True, timeout=60, check=False)
                   if built.returncode == 0 else None)
            if ran is None or ran.returncode != 0:
                raise RuntimeError(f"{fixture.name} {label}: {built.stderr}{ran and ran.returncode}")
    if len(regressions) != 3:
        raise RuntimeError(f"expected three reduced regression fixtures: {regressions}")
    import shutil
    shutil.rmtree(output)
    print("feature interaction smoke: 12 LLVM O0/O3 native outcomes, two admission controls "
          "and three reduced regressions at O0/O3 passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
