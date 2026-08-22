#!/usr/bin/env python3
"""Guard the complete example-corpus LoweredProgram inventory."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


GENERATED_KINDS = (
    "program_initialization",
    "hosted_entry",
    "structural_operator_adapter",
    "callable_adapter",
    "lifecycle_cleanup",
    "native_interop_adapter",
    "concrete_instance_adapter",
)


def main() -> int:
    if len(sys.argv) != 4:
        print(
            "usage: lowered_program_census_test.py <tool> <repo-root> <baseline>",
            file=sys.stderr,
        )
        return 2
    tool = Path(sys.argv[1]).resolve()
    root = Path(sys.argv[2]).resolve()
    baseline_path = Path(sys.argv[3]).resolve()
    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    expected: dict[str, dict[str, object]] = baseline["examples"]
    names = sorted(expected)

    shipped = sorted(path.name for path in (root / "examples").glob("*.gti"))
    if shipped != names:
        print("FAIL: lowered-program census and shipped examples differ", file=sys.stderr)
        print(f"  baseline only: {sorted(set(names) - set(shipped))}", file=sys.stderr)
        print(f"  corpus only: {sorted(set(shipped) - set(names))}", file=sys.stderr)
        return 1

    completed = subprocess.run(
        [str(tool), str(root), *names],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        print(completed.stdout, end="", file=sys.stderr)
        print(completed.stderr, end="", file=sys.stderr)
        return 1

    actual: dict[str, dict[str, object]] = {}
    for line in completed.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 4 + len(GENERATED_KINDS):
            print(f"FAIL: malformed census row: {line}", file=sys.stderr)
            return 1
        counts = [int(value) for value in fields[1:]]
        actual[fields[0]] = {
            "declarations": counts[0],
            "bodies": counts[1],
            "generated_items": counts[2],
            "generated_kinds": dict(zip(GENERATED_KINDS, counts[3:], strict=True)),
        }

    changed = [name for name in names if actual.get(name) != expected[name]]
    if changed:
        print(
            f"FAIL: LoweredProgram census changed across {len(changed)} examples:",
            file=sys.stderr,
        )
        for name in changed:
            print(f"  {name}: expected {expected[name]}, got {actual.get(name)}", file=sys.stderr)
        return 1

    totals = {
        "declarations": sum(int(row["declarations"]) for row in actual.values()),
        "bodies": sum(int(row["bodies"]) for row in actual.values()),
        "generated_items": sum(int(row["generated_items"]) for row in actual.values()),
    }
    if totals != baseline["totals"]:
        print(
            f"FAIL: LoweredProgram census totals differ: expected {baseline['totals']}, got {totals}",
            file=sys.stderr,
        )
        return 1
    print(
        "LoweredProgram census: "
        f"{totals['declarations']} declarations, {totals['bodies']} bodies, "
        f"{totals['generated_items']} generated items across {len(names)} examples"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
