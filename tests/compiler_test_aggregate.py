#!/usr/bin/env python3
"""Validate and run the exact R-STRUCT-01 compiler-test partition."""

from __future__ import annotations

import json
import subprocess
import sys
import time
from pathlib import Path


def fail(message: str) -> int:
    print(f"compiler test aggregate: {message}", file=sys.stderr)
    return 2


def main() -> int:
    if len(sys.argv) < 3:
        return fail("usage: compiler_test_aggregate.py MANIFEST SUITE=EXECUTABLE...")

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

    expected_by_suite: dict[str, list[str]] = {}
    expected_order: list[str] = []
    for item in manifest["tests"]:
        if not isinstance(item, dict):
            return fail("manifest test entries must be objects")
        name = item.get("name")
        suite = item.get("suite")
        if not isinstance(name, str) or not isinstance(suite, str):
            return fail("manifest test entries require string name and suite")
        expected_by_suite.setdefault(suite, []).append(name)
        expected_order.append(name)

    if set(executables) != set(expected_by_suite):
        return fail(
            "suite bindings differ from manifest: "
            f"expected {sorted(expected_by_suite)}, got {sorted(executables)}"
        )
    if len(expected_order) != len(set(expected_order)):
        return fail("manifest contains duplicate test names")

    discovered: list[str] = []
    for suite, expected in expected_by_suite.items():
        listed = subprocess.run(
            [str(executables[suite]), "--list"],
            check=False,
            capture_output=True,
            text=True,
        )
        if listed.returncode != 0:
            sys.stderr.write(listed.stdout)
            sys.stderr.write(listed.stderr)
            return fail(f"{suite} --list exited {listed.returncode}")
        actual = [line for line in listed.stdout.splitlines() if line]
        if actual != expected:
            return fail(
                f"{suite} census mismatch: expected {expected!r}, got {actual!r}"
            )
        discovered.extend(actual)

    if len(discovered) != len(set(discovered)) or set(discovered) != set(
        expected_order
    ):
        return fail("runtime census lost or duplicated a compiler test")

    for suite, executable in executables.items():
        started = time.monotonic()
        print(f"compiler test aggregate: starting {suite} "
              f"({len(expected_by_suite[suite])} tests)", flush=True)
        completed = subprocess.run([str(executable)], check=False)
        print(f"compiler test aggregate: {suite} exited {completed.returncode} "
              f"after {time.monotonic() - started:.1f}s", flush=True)
        if completed.returncode != 0:
            return fail(f"{suite} suite exited {completed.returncode}")

    print(
        f"compiler test aggregate passed: {len(discovered)} tests across "
        f"{len(executables)} suites"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
