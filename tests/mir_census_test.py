#!/usr/bin/env python3

"""Hold every example's MIR body census within its reviewed budget.

The test recomputes the per-example census. Every example must still
compile. A recorded example whose count leaves the census budget
(tests/census_budget.py: the larger of five bodies or five percent) is an
advisory finding (tests/advisory_checks.py): it prints a `GTI-ADVISORY:` line
and passes unless GTI_ADVISORY_CHECKS=strict. Drift inside the budget,
examples not yet recorded and recorded examples that were removed are
reported, never failed, so pull requests do not rewrite each other's rows.

    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json>
    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json> --update
    python3 tests/mir_census_test.py <gti> <repo-root> <baseline.json> --refresh-all

--update re-records only the examples outside the budget; --refresh-all
re-records the whole host profile, including added and removed examples. Do
either only after the corpus oracle and suite are green: a refresh is a claim
about the reviewed corpus, not a way to silence a change.
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

import advisory_checks as advisory
import census_budget as budget
import census_store as store
import example_case_context as examples
import mir_census_baseline as reviewed

MARKER = "GTI verified-MIR body"
MARKER_PATTERN = re.compile(
    rf"{re.escape(MARKER)}:[^\n]* "
    r"(module-instance|field-initializers-instance|"
    r"static-field-initializers-instance|function-instance|"
    r"constructor-instance|destructor-instance|lambda-instance|"
    r"hosted-startup-instance|thread-local-initializer) (\d+)"
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


def census(gti: Path, root: Path, lowered_census: Path | None = None) -> dict[str, int | None]:
    """Per-example MIR-emitted body counts. None means the example failed."""
    counts: dict[str, int | None] = {}
    target = reviewed.host_target()
    # A release-configured gti discovers only the installed stdlib layout;
    # point it at the repository stdlib so the census is build-flavor
    # independent.
    env = examples.environment(root, os.environ)
    with tempfile.TemporaryDirectory(prefix="gti-census-") as scratch:
        out = Path(scratch) / "census.cpp"
        for case in examples.cases(root):
            source = root / case.source
            out.unlink(missing_ok=True)
            if source.name in examples.LLVM_ONLY_EXAMPLES:
                examples.require_cpp_limitation(gti, source, out, env)
                # Inventory comes from the sealed LoweredProgram, independently
                # of target emission. Native execution remains the corpus oracle.
                inventory = subprocess.run(
                    [str(lowered_census or gti.parent / "gti_lowered_program_census"), str(root),
                     "--target", target, source.name],
                    capture_output=True, text=True, env=env, timeout=examples.CENSUS_TIMEOUT)
                if inventory.returncode != 0:
                    counts[source.name] = None
                    FAILURES[source.name] = inventory.stderr
                else:
                    rows = inventory.stdout.splitlines()
                    fields = rows[1].split("\t")
                    assert fields[0] == source.name and len(rows) == 2, rows
                    counts[source.name] = int(fields[2])
                continue
            completed = subprocess.run(
                examples.census_command(case, root, gti, out),
                cwd=root,
                capture_output=True,
                timeout=examples.CENSUS_TIMEOUT,
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
    parser.add_argument("--lowered-census", type=Path,
                        help="explicit build dependency for LLVM-only example inventories")
    parser.add_argument("--target", choices=reviewed.TARGETS,
                        help="assert the native host target; cross-target execution is not supported")
    refresh = parser.add_mutually_exclusive_group()
    refresh.add_argument(
        "--update",
        action="store_true",
        help="re-record only the examples outside the census budget",
    )
    refresh.add_argument(
        "--refresh-all",
        action="store_true",
        help="re-record the whole host profile, including added and removed examples",
    )
    arguments = parser.parse_args()

    gti = Path(arguments.gti).resolve()
    root = Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline)

    try:
        document = store.load(baseline_path, "mir")
        target = reviewed.host_target()
        if arguments.target is not None and arguments.target != target:
            parser.error(f"--target {arguments.target} does not match executing host {target}")
        baseline = reviewed.select(document, target)
    except (OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    current = census(gti, root, arguments.lowered_census)
    if not current:
        print("FAIL: no examples/*.gti found", file=sys.stderr)
        return 1

    broken = sorted(name for name, value in current.items() if value is None)
    if broken:
        print("FAIL: examples no longer compile, so their census is unknown:",
              file=sys.stderr)
        for name in broken:
            print(f"  {name}", file=sys.stderr)
            diagnostic = FAILURES.get(name, "")
            for line in diagnostic.splitlines()[:5]:
                print(f"    {line}", file=sys.stderr)
        return 1

    observed = {name: value for name, value in current.items() if value is not None}
    recorded = sorted(name for name in observed if name in baseline)
    outside = [(name, baseline[name], observed[name]) for name in recorded
               if not budget.within(baseline[name], observed[name])]
    drifted = [name for name in recorded if observed[name] != baseline[name]]
    added = sorted(name for name in observed if name not in baseline)
    removed = sorted(name for name in baseline if name not in observed)

    if arguments.update or arguments.refresh_all:
        names = None if arguments.refresh_all else {name for name, _, _ in outside}
        try:
            document = reviewed.refreshed(document, target, observed, names)
        except ValueError as error:
            print(f"FAIL: {error}", file=sys.stderr)
            return 1
        store.save(baseline_path, "mir", document)
        scope = "every example" if names is None else f"{len(names)} examples outside the budget"
        print(f"baseline updated ({target}): {scope}")
        return 0

    print(f"MIR census ({target}): {total(current)} bodies across {len(current)} examples "
          f"(baseline {sum(v for v in baseline.values() if v)}); "
          f"{len(drifted) - len(outside)} within budget, {len(added)} unrecorded, "
          f"{len(removed)} removed")
    for name in added:
        print(f"  unrecorded: {name} ({observed[name]} bodies; recorded at the next --refresh-all)")
    for name in removed:
        print(f"  removed: {name} (dropped at the next --refresh-all)")

    if outside:
        print(f"MIR body census left its budget in {len(outside)} examples (advisory): "
              "review the changed body inventory and run the corpus oracle, then "
              "re-record these rows with --update.")
        status = advisory.report("mir_census_regression", [
            f"{name}: {before} -> {after} MIR bodies ({after - before:+d}, "
            f"budget +/-{budget.allowance(before)}); re-record with --update"
            for name, before, after in outside])
        if status:
            return status
        print("ok: every example compiles; census drift reported as advisory")
        return 0

    print("ok: census within budget")
    return 0


if __name__ == "__main__":
    with advisory.leading_findings():
        status = main()
    sys.exit(status)
