#!/usr/bin/env python3
"""Hold the example-corpus LoweredProgram inventory within its reviewed budget.

Each example records bodies, generated items and generated items by kind for
both target profiles. A recorded example fails when any of those leaves the
census budget (tests/census_budget.py). Declarations are not recorded: one
more stdlib function is one more declaration in every importing example, so
that count moved in nearly every row of every stdlib pull request without
saying anything about what the example lowers to. Examples not yet recorded
and recorded examples that were removed are reported, not failed.

``--update`` re-records only the examples outside the budget;
``--refresh-all`` re-records every example.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import census_budget as budget


GENERATED_KINDS = (
    "program_initialization",
    "hosted_entry",
    "structural_operator_adapter",
    "callable_adapter",
    "lifecycle_cleanup",
    "native_interop_adapter",
    "thread_task_adapter",
    "concrete_instance_adapter",
    "thread_local_storage",
)


TARGETS = ("arm64-apple-macos", "x86_64-unknown-linux")
METRICS = ("bodies", "generated_items")


def collect(tool: Path, root: Path, names: list[str], target: str) -> dict:
    completed = subprocess.run(
        [str(tool), str(root), "--target", target, *names],
        check=False, capture_output=True, text=True,
    )
    if completed.returncode != 0:
        raise ValueError(completed.stdout + completed.stderr)
    lines = completed.stdout.splitlines()
    if not lines or tuple(lines[0].split("\t")) != ("generated_kinds", *GENERATED_KINDS):
        raise ValueError("compiled generated-item columns differ from census schema")
    actual = {}
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) != 4 + len(GENERATED_KINDS) or fields[0] in actual:
            raise ValueError(f"malformed or duplicate census row: {line}")
        counts = [int(value) for value in fields[1:]]
        actual[fields[0]] = {
            "bodies": counts[1],
            "generated_items": counts[2],
            "generated_kinds": dict(zip(GENERATED_KINDS, counts[3:], strict=True)),
        }
    if sorted(actual) != names:
        raise ValueError("compiled census rows differ from the complete shipped corpus")
    return actual


def totals(rows: dict) -> dict:
    return {key: sum(int(row[key]) for row in rows.values()) for key in METRICS}


def metrics(row: dict) -> dict[str, int]:
    """The budgeted counts of one row; a legacy ``declarations`` field is ignored."""
    values = {key: int(row[key]) for key in METRICS}
    for kind, count in row["generated_kinds"].items():
        values[f"generated_kinds.{kind}"] = int(count)
    return values


def outside_budget(recorded: dict, observed: dict) -> list[str]:
    """The metrics of ``observed`` that left the budget around ``recorded``."""
    before, after = metrics(recorded), metrics(observed)
    return [f"{key} {before.get(key, 0)} -> {after[key]}"
            for key in after if not budget.within(before.get(key, 0), after[key])]


def profile(baseline: dict, target: str) -> dict:
    rows = baseline["examples"]
    if target != TARGETS[0]:
        variant = baseline["variants"][target]
        if not set(variant["examples"]).issubset(rows):
            raise ValueError(f"{target}: variant contains an unknown example")
        rows = {**rows, **variant["examples"]}
    return rows


def refreshed(baseline: dict | None, observed: dict, names: set[str] | None) -> dict:
    """Re-record ``names`` (every example when None) for both targets."""
    previous = baseline or {"examples": {}, "variants": {t: {"examples": {}} for t in TARGETS[1:]}}
    rows = {target: dict(profile(previous, target)) for target in TARGETS} if baseline else \
        {target: {} for target in TARGETS}
    for target in TARGETS:
        if names is None:
            rows[target] = dict(observed[target])
        else:
            for name in names:
                rows[target][name] = observed[target][name]
    base = rows[TARGETS[0]]
    result = {"examples": budget.ordered(previous["examples"], base),
              "target": TARGETS[0], "variants": {}}
    for target in TARGETS[1:]:
        result["variants"][target] = {
            "examples": budget.ordered(previous["variants"][target]["examples"],
                                       {name: row for name, row in rows[target].items()
                                        if row != base[name]}),
        }
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tool")
    parser.add_argument("root")
    parser.add_argument("baseline")
    refresh = parser.add_mutually_exclusive_group()
    refresh.add_argument("--update", action="store_true",
                         help="re-record only the examples outside the census budget")
    refresh.add_argument("--refresh-all", action="store_true",
                         help="re-record every example, including added and removed ones")
    arguments = parser.parse_args()
    tool, root = Path(arguments.tool).resolve(), Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline).resolve()
    names = sorted(path.name for path in (root / "examples").glob("*.gti"))
    baseline = None
    if baseline_path.exists():
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        if (baseline.get("target") != TARGETS[0] or
                set(baseline.get("variants", {})) != set(TARGETS[1:])):
            raise ValueError("census target profiles differ from the baseline")
    elif not arguments.refresh_all:
        raise ValueError(f"missing census baseline {baseline_path}; create it with --refresh-all")
    observed = {target: collect(tool, root, names, target) for target in TARGETS}
    failed = False
    outside: set[str] = set()
    for target in TARGETS:
        actual = observed[target]
        print(f"LoweredProgram census {target}: {totals(actual)} across {len(names)} examples",
              flush=True)
        if baseline is None:
            continue
        expected = profile(baseline, target)
        unrecorded = [name for name in names if name not in expected]
        removed = sorted(set(expected) - set(names))
        changes = {name: outside_budget(expected[name], actual[name])
                   for name in names if name in expected}
        changes = {name: change for name, change in changes.items() if change}
        drifted = sum(1 for name in names if name in expected
                      and metrics(expected[name]) != metrics(actual[name]))
        print(f"  {drifted - len(changes)} within budget, {len(unrecorded)} unrecorded, "
              f"{len(removed)} removed", flush=True)
        for name in unrecorded:
            print(f"  unrecorded: {name} (recorded at the next --refresh-all)")
        outside |= set(changes)
        if changes and not (arguments.update or arguments.refresh_all):
            failed = True
            print(f"FAIL: {target} LoweredProgram census left its budget:", file=sys.stderr)
            for name, change in changes.items():
                print(f"  {name}: {'; '.join(change)}", file=sys.stderr)
    if failed:
        print("Review the lowered inventory, then re-record these rows with --update.",
              file=sys.stderr)
        return 1
    if arguments.update or arguments.refresh_all:
        document = refreshed(baseline, observed, None if arguments.refresh_all else outside)
        baseline_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        scope = "every example" if arguments.refresh_all else f"{len(outside)} examples outside the budget"
        print(f"baseline updated: {scope}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
