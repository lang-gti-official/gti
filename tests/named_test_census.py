#!/usr/bin/env python3
"""Fail closed when a partitioned named-test registry changes its census."""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path


def fail(message: str) -> int:
    print(f"named-test census: {message}", file=sys.stderr)
    return 2


def main() -> int:
    if len(sys.argv) < 3:
        return fail("usage: named_test_census.py MANIFEST SUITE=EXECUTABLE...")
    manifest_path = Path(sys.argv[1])
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema_version") != 1 or not isinstance(
        manifest.get("tests"), list
    ):
        return fail(f"invalid manifest: {manifest_path}")

    executables: dict[str, Path] = {}
    for binding in sys.argv[2:]:
        suite, separator, executable = binding.partition("=")
        if not separator or not suite or not executable or suite in executables:
            return fail(f"invalid or duplicate suite binding: {binding}")
        executables[suite] = Path(executable)

    expected: dict[str, list[str]] = {}
    all_expected: list[str] = []
    for item in manifest["tests"]:
        if not isinstance(item, dict):
            return fail("manifest entries must be objects")
        name = item.get("name")
        suite = item.get("suite")
        if not isinstance(name, str) or not isinstance(suite, str):
            return fail("manifest entries require string name and suite")
        expected.setdefault(suite, []).append(name)
        all_expected.append(name)

    if set(executables) != set(expected):
        return fail(
            f"suite mismatch: expected {sorted(expected)}, "
            f"got {sorted(executables)}"
        )
    if len(all_expected) != len(set(all_expected)):
        return fail("manifest contains duplicate test names")

    discovered: list[str] = []
    for suite, names in expected.items():
        completed = subprocess.run(
            [str(executables[suite]), "--list"],
            check=False,
            capture_output=True,
            text=True,
        )
        if completed.returncode != 0:
            sys.stderr.write(completed.stdout)
            sys.stderr.write(completed.stderr)
            return fail(f"{suite} --list exited {completed.returncode}")
        actual = [line for line in completed.stdout.splitlines() if line]
        if actual != names:
            return fail(f"{suite} differs: expected {names!r}, got {actual!r}")
        discovered.extend(actual)

    if len(discovered) != len(set(discovered)) or set(discovered) != set(
        all_expected
    ):
        return fail("runtime registries lost or duplicated a named test")
    print(
        f"named-test census passed: {len(discovered)} tests across "
        f"{len(executables)} suites"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
