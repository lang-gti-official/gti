#!/usr/bin/env python3
"""Guard the complete example-corpus LoweredProgram inventory."""

from __future__ import annotations

import argparse
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
    "thread_task_adapter",
    "concrete_instance_adapter",
    "thread_local_storage",
)


TARGETS = ("arm64-apple-macos", "x86_64-unknown-linux")


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
            "declarations": counts[0], "bodies": counts[1],
            "generated_items": counts[2],
            "generated_kinds": dict(zip(GENERATED_KINDS, counts[3:], strict=True)),
        }
    if sorted(actual) != names:
        raise ValueError("compiled census rows differ from the complete shipped corpus")
    return {"examples": actual, "totals": {
        key: sum(int(row[key]) for row in actual.values())
        for key in ("declarations", "bodies", "generated_items")
    }}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tool")
    parser.add_argument("root")
    parser.add_argument("baseline")
    parser.add_argument("--update", action="store_true")
    arguments = parser.parse_args()
    tool, root = Path(arguments.tool).resolve(), Path(arguments.root).resolve()
    baseline_path = Path(arguments.baseline).resolve()
    names = sorted(path.name for path in (root / "examples").glob("*.gti"))
    baseline = None
    if not arguments.update:
        baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
        if (baseline.get("target") != TARGETS[0] or
                set(baseline.get("variants", {})) != set(TARGETS[1:]) or
                sorted(baseline["examples"]) != names):
            raise ValueError("census target profiles or shipped corpus differ from the baseline")
    observed = {}
    for target in TARGETS:
        actual = collect(tool, root, names, target)
        observed[target] = actual
        if baseline is not None:
            expected = baseline
            if target != TARGETS[0]:
                variant = baseline["variants"][target]
                if not set(variant["examples"]).issubset(names):
                    raise ValueError(f"{target}: variant contains an unknown example")
                expected = {"examples": {**baseline["examples"], **variant["examples"]},
                            "totals": variant["totals"]}
            changed = [name for name in names
                       if actual["examples"][name] != expected["examples"][name]]
            if changed or actual["totals"] != expected["totals"]:
                print(f"FAIL: {target} LoweredProgram census changed:", file=sys.stderr)
                for name in changed:
                    print(f"  {name}: expected {expected['examples'][name]}, "
                          f"got {actual['examples'][name]}", file=sys.stderr)
                print(f"  totals: expected {expected['totals']}, got {actual['totals']}",
                      file=sys.stderr)
                return 1
        print(f"LoweredProgram census {target}: {actual['totals']} across {len(names)} examples",
              flush=True)
    if arguments.update:
        base = observed[TARGETS[0]]
        result = {**base, "target": TARGETS[0], "variants": {}}
        for target in TARGETS[1:]:
            actual = observed[target]
            result["variants"][target] = {
                "examples": {name: row for name, row in actual["examples"].items()
                             if row != base["examples"][name]},
                "totals": actual["totals"],
            }
        baseline_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
