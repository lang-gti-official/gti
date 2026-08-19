#!/usr/bin/env python3

"""Fail when a change reduces MIR emission for any example.

The differential oracle proves the two emission paths agree. It says nothing
about how many bodies MIR actually emits, so a change can be completely
correct, pass the whole suite and the oracle, and still lose coverage — a
widening that flips one body's admission and drops its callers through the
fixpoint is the recurring shape. Those losses were being found by diffing the
census by hand, one window at a time, after the fact.

This test makes the loss the failure. It recomputes the per-example census and
compares it against a committed baseline, naming the exact examples that went
backwards and by how much. Coverage gains are not failures; they print a
reminder to refresh the baseline.

    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json>
    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json> --update

Refresh the baseline with --update only from a clean tree, and only once the
oracle and suite are green — the baseline is a claim that the count is
legitimately higher, not a way to silence a regression.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

MARKER = "GTI verified-MIR body"


def census(gti: Path, root: Path) -> dict[str, int | None]:
    """Per-example MIR-emitted body counts. None means the example failed."""
    counts: dict[str, int | None] = {}
    with tempfile.TemporaryDirectory(prefix="gti-census-") as scratch:
        out = Path(scratch) / "census.cpp"
        for source in sorted((root / "examples").glob("*.gti")):
            completed = subprocess.run(
                [str(gti), "--emit-cpp", str(source), "-o", str(out)],
                cwd=root,
                capture_output=True,
                timeout=180,
                check=False,
            )
            if completed.returncode != 0 or not out.exists():
                counts[source.name] = None
                continue
            counts[source.name] = out.read_text(
                encoding="utf-8", errors="replace"
            ).count(MARKER)
    return counts


def total(counts: dict[str, int | None]) -> int:
    return sum(value for value in counts.values() if value)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gti")
    parser.add_argument("root")
    parser.add_argument("baseline")
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite the baseline from the current tree",
    )
    arguments = parser.parse_args()

    gti = Path(arguments.gti).resolve()
    root = Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline)

    current = census(gti, root)
    if not current:
        print("FAIL: no examples/*.gti found", file=sys.stderr)
        return 1

    if arguments.update:
        baseline_path.write_text(
            json.dumps(
                {"total": total(current), "examples": current},
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"baseline updated: {total(current)} bodies across {len(current)} examples")
        return 0

    if not baseline_path.is_file():
        print(f"FAIL: missing baseline {baseline_path}; run with --update from a "
              "clean tree", file=sys.stderr)
        return 1

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))["examples"]

    broken = sorted(name for name, value in current.items() if value is None)
    regressed = sorted(
        (name, baseline[name], current[name])
        for name in current
        if name in baseline
        and current[name] is not None
        and baseline[name] is not None
        and current[name] < baseline[name]
    )
    gained = sorted(
        (name, baseline[name], current[name])
        for name in current
        if name in baseline
        and current[name] is not None
        and baseline[name] is not None
        and current[name] > baseline[name]
    )
    added = sorted(name for name in current if name not in baseline)
    removed = sorted(name for name in baseline if name not in current)

    print(f"MIR census: {total(current)} bodies across {len(current)} examples "
          f"(baseline {sum(v for v in baseline.values() if v)})")

    if broken:
        print("FAIL: examples no longer compile, so their census is unknown:",
              file=sys.stderr)
        for name in broken:
            print(f"  {name}", file=sys.stderr)
        return 1

    if regressed:
        lost = sum(before - after for _, before, after in regressed)
        print(f"FAIL: MIR emission regressed by {lost} bodies across "
              f"{len(regressed)} examples:", file=sys.stderr)
        for name, before, after in regressed:
            print(f"  {name}: {before} -> {after}  ({after - before})",
                  file=sys.stderr)
        print(
            "A change that is correct can still lose coverage: admitting a body "
            "through one form can remove it from another, and the fixpoint then "
            "drops its callers. Find the body whose admission changed rather "
            "than refreshing the baseline.",
            file=sys.stderr,
        )
        return 1

    if removed:
        print(f"FAIL: {len(removed)} baseline examples are missing from the "
              "corpus:", file=sys.stderr)
        for name in removed:
            print(f"  {name}", file=sys.stderr)
        return 1

    if gained or added:
        net = sum(after - before for _, before, after in gained)
        print(f"ok: no regression; +{net} bodies across {len(gained)} examples"
              + (f", {len(added)} new examples" if added else ""))
        for name, before, after in gained:
            print(f"  {name}: {before} -> {after}  (+{after - before})")
        print("Refresh with --update so later changes are measured against this.")
    else:
        print("ok: census unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
