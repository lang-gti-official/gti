#!/usr/bin/env python3
"""Validate tests/ownership.toml, the shared test-ownership map.

Without arguments this runs the checks that need no build: the schema, that
every path pattern matches a tracked file, that every tracked file under the
covered roots has an owning area or an explicit [[unowned]] reason, and that
the SHARED validation floor is marked as shared or system risk.

With --ctest-inventory <json> (the output of `ctest --show-only=json-v1`), or
with --ctest <ctest> --build <dir> to produce it, it also requires every owner
and consumer to name at least one registered test. --inventory-only skips the
Git-based checks, for running inside CTest.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / ".codex/skills/verify-change/scripts"))
import plan_checks  # noqa: E402
from validation_checks import SHARED  # noqa: E402

COVERED_ROOTS = ("src/", "include/", "runtime/", "stdlib/", "scripts/", "lua/")
REGISTRATION_FILES = ("tests/CMakeLists.txt", "tests/TestThroughput.cmake")


def tracked_files(root: Path) -> list[str]:
    output = subprocess.run(["git", "ls-files", "-z"], cwd=root, check=True,
                            capture_output=True).stdout
    return sorted(item.decode() for item in output.split(b"\0") if item)


def textual_registrations(root: Path) -> set[str]:
    names = set()
    for relative in REGISTRATION_FILES:
        path = root / relative
        if path.is_file():
            names.update(re.findall(r'add_test\(\s*NAME\s+"?([A-Za-z0-9_.-]+)',
                                    path.read_text(encoding="utf-8")))
    return names


def tracked_errors(ownership: plan_checks.OwnershipMap, files: list[str]) -> list[str]:
    """Patterns must match tracked files; covered roots must be owned."""
    errors = []
    for area in ownership.areas:
        for pattern in area.paths:
            regex = plan_checks.pattern_regex(pattern)
            if not any(regex.fullmatch(path) for path in files):
                errors.append(f"area {area.area!r}: path {pattern!r} matches no tracked file")
    for entry in ownership.unowned:
        for pattern in entry["paths"]:
            regex = plan_checks.pattern_regex(pattern)
            if not any(regex.fullmatch(path) for path in files):
                errors.append(f"unowned path {pattern!r} matches no tracked file")
    for path in files:
        if not path.startswith(COVERED_ROOTS):
            continue
        owned = plan_checks.areas_for(path, ownership)
        exempt = plan_checks.unowned_for(path, ownership)
        if not owned and exempt is None:
            errors.append(f"{path} has no owning area and no [[unowned]] reason")
        if owned and exempt is not None:
            errors.append(f"{path} is both owned and listed as unowned")
        if path.startswith(SHARED) and not any(area.risk in {"shared", "system"} for area in owned):
            errors.append(f"{path} is in the SHARED validation floor but no matching area "
                          "has risk shared or system")
    return errors


def registration_errors(ownership: plan_checks.OwnershipMap, registered: set[str]) -> list[str]:
    return [f"configuration-dependent test {name!r} is not registered in "
            f"{' or '.join(REGISTRATION_FILES)}"
            for name in ownership.configuration_dependent if name not in registered]


def inventory_errors(ownership: plan_checks.OwnershipMap, inventory: list[str],
                     registered: set[str]) -> list[str]:
    """Every owner and consumer must select at least one registered test."""
    errors = []
    names = set(inventory)
    dependent = set(ownership.configuration_dependent)
    for area in ownership.areas:
        for key in ("owners", "consumers"):
            for entry in getattr(area, key):
                if any(plan_checks.test_matches(entry, name) for name in names):
                    continue
                if entry in dependent and entry in registered:
                    continue
                errors.append(f"area {area.area!r}: {key[:-1]} {entry!r} matches no registered test")
    return errors


def load_inventory(args: argparse.Namespace) -> list[str] | None:
    if args.ctest_inventory:
        document = json.loads(Path(args.ctest_inventory).read_text(encoding="utf-8"))
    elif args.ctest:
        result = subprocess.run([args.ctest, "--test-dir", args.build, "--show-only=json-v1"],
                                check=True, capture_output=True, text=True, timeout=300)
        document = json.loads(result.stdout)
    else:
        return None
    return [test["name"] for test in document.get("tests", [])]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=str(ROOT), help="Repository root (default: this checkout).")
    parser.add_argument("--ctest-inventory", help="ctest --show-only=json-v1 output to check against.")
    parser.add_argument("--ctest", help="CTest executable used to produce the inventory.")
    parser.add_argument("--build", help="Build directory for --ctest.")
    parser.add_argument("--inventory-only", action="store_true",
                        help="Check the schema and inventory only, without Git.")
    args = parser.parse_args(argv)
    if bool(args.ctest) != bool(args.build):
        parser.error("--ctest and --build go together")
    if args.ctest and args.ctest_inventory:
        parser.error("pass either --ctest-inventory or --ctest/--build")
    if args.inventory_only and not (args.ctest or args.ctest_inventory):
        parser.error("--inventory-only needs an inventory")

    root = Path(args.root).resolve()
    with (root / plan_checks.MAP_PATH).open("rb") as handle:
        try:
            document = tomllib.load(handle)
        except tomllib.TOMLDecodeError as error:
            print(f"ownership map: {plan_checks.MAP_PATH} is not valid TOML: {error}", file=sys.stderr)
            return 1
    errors = plan_checks.schema_errors(document)
    if errors:
        print("ownership map: schema errors:\n  " + "\n  ".join(errors), file=sys.stderr)
        return 1
    ownership = plan_checks.parse_map(document)
    registered = textual_registrations(root)
    errors += registration_errors(ownership, registered)
    checked = ["schema"]
    if not args.inventory_only:
        errors += tracked_errors(ownership, tracked_files(root))
        checked.append("tracked-file coverage")
    inventory = load_inventory(args)
    if inventory is not None:
        errors += inventory_errors(ownership, inventory, registered)
        checked.append(f"inventory of {len(inventory)} tests")
    if errors:
        print("ownership map: " + str(len(errors)) + " error(s):\n  " + "\n  ".join(errors),
              file=sys.stderr)
        return 1
    print(f"ownership map: {len(ownership.areas)} areas pass ({', '.join(checked)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
