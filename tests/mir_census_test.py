#!/usr/bin/env python3

"""Require the reviewed post-cutover MIR body census for every example.

The hard cutover has one executable backend route, so any body-count change is
a reviewed corpus-contract change rather than an incremental coverage gain or
loss. This test recomputes the per-example census and names every mismatch.

    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json>
    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json> --update

Refresh the baseline with --update only after the corpus oracle and suite are
green. The baseline is a claim about the complete reviewed corpus, not a way to
silence a change.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

MARKER = "GTI verified-MIR body"
MARKER_PATTERN = re.compile(
    rf"{re.escape(MARKER)}:[^\n]* "
    r"(module-instance|field-initializers-instance|"
    r"static-field-initializers-instance|function-instance|"
    r"constructor-instance|destructor-instance|lambda-instance|"
    r"hosted-startup-instance) (\d+)"
)

# Diagnostics from examples that failed to compile, for the FAIL report.
FAILURES: dict[str, str] = {}


def emitted_body_count(text: str) -> int:
    """Count unique MIR body addresses, not marker occurrences.

    One body may provide both an ordinary wrapper and a transformed failure
    sibling. Both carry evidence markers, but they represent one MIR owner and
    must advance the cutover census only once.
    """
    return len(set(MARKER_PATTERN.findall(text)))


def census(gti: Path, root: Path) -> dict[str, int | None]:
    """Per-example MIR-emitted body counts. None means the example failed."""
    counts: dict[str, int | None] = {}
    # A release-configured gti discovers only the installed stdlib layout;
    # point it at the repository stdlib so the census is build-flavor
    # independent.
    env = dict(os.environ)
    env.setdefault("GTI_STDLIB_PATH", str(root / "stdlib"))
    with tempfile.TemporaryDirectory(prefix="gti-census-") as scratch:
        out = Path(scratch) / "census.cpp"
        for source in sorted((root / "examples").glob("*.gti")):
            out.unlink(missing_ok=True)
            completed = subprocess.run(
                [str(gti), "--emit-cpp", str(source), "-o", str(out)],
                cwd=root,
                capture_output=True,
                timeout=180,
                check=False,
                env=env,
            )
            if completed.returncode != 0 or not out.exists():
                counts[source.name] = None
                FAILURES[source.name] = (
                    completed.stderr.decode("utf-8", errors="replace").strip()
                    or completed.stdout.decode("utf-8", errors="replace").strip()
                )
                continue
            counts[source.name] = emitted_body_count(
                out.read_text(encoding="utf-8", errors="replace")
            )
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
    changed = sorted(
        (name, baseline[name], current[name])
        for name in current
        if name in baseline
        and current[name] is not None
        and baseline[name] is not None
        and current[name] != baseline[name]
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
            diagnostic = FAILURES.get(name, "")
            for line in diagnostic.splitlines()[:5]:
                print(f"    {line}", file=sys.stderr)
        return 1

    if changed:
        print(f"FAIL: MIR body census changed across {len(changed)} examples:",
              file=sys.stderr)
        for name, before, after in changed:
            print(f"  {name}: {before} -> {after}  ({after - before:+d})",
                  file=sys.stderr)
        print(
            "Review the changed body inventory and run the corpus oracle before "
            "refreshing the baseline.",
            file=sys.stderr,
        )
        return 1

    if removed:
        print(f"FAIL: {len(removed)} baseline examples are missing from the "
              "corpus:", file=sys.stderr)
        for name in removed:
            print(f"  {name}", file=sys.stderr)
        return 1

    if added:
        print(f"FAIL: {len(added)} corpus examples are absent from the reviewed "
              "baseline:", file=sys.stderr)
        for name in added:
            print(f"  {name}", file=sys.stderr)
        return 1

    print("ok: census unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
