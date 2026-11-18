#!/usr/bin/env python3
"""Keep the old-style test families frozen: a change may not grow them.

New tests are `gti-test` files or `tests/unit` cases
(docs/plans/test-suite-redesign.md). The registered Python harnesses, C++ test
binaries and `add_test` registrations that remain are kept deliberately and
stay frozen as a standing rule. This check compares a candidate with the
commit it forked from and fails when the candidate adds a new
`tests/**/*_test.py`, a new `tests/**/*_tests.cpp` or a new `add_test(` line in
a CMake file under `tests/`. Modifying, renaming or deleting existing tests
stays allowed, and an `add_test(` line moved unchanged within the diff is not
new. The runner's directories (`tests/ui/`, `tests/lsp/`, `tests/project/`,
`tests/slow/`, `tests/unit/`, `tests/runner/`) and `tests/gti_test.cmake` are
exempt. The rule is diff-based, so it needs no shared registry file.

Usage: check_test_freeze.py <base> <head>
"""

from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess

EXEMPT_PREFIXES = ("tests/ui/", "tests/lsp/", "tests/project/", "tests/slow/", "tests/unit/", "tests/runner/")
EXEMPT_FILES = frozenset({"tests/gti_test.cmake"})
NEW_TEST_FILE = re.compile(r"tests/(?:.+/)?[^/]+(?:_test\.py|_tests\.cpp)")
CMAKE_FILE = re.compile(r"tests/(?:.+/)?(?:CMakeLists\.txt|[^/]+\.cmake)")
ADD_TEST = re.compile(r"\badd_test\s*\(")
GUIDANCE = (
    "The old test suite is frozen as a standing rule: add no new tests/*_test.py, tests/*_tests.cpp or add_test(.\n"
    "New tests are gti-test files in tests/ui/, tests/slow/, tests/lsp/ or tests/project/, or C++ unit cases in "
    "tests/unit/ (tests/ui/README.md); fix or convert a broken old case, never add to an old harness."
)


def exempt(path: str) -> bool:
    return path in EXEMPT_FILES or path.startswith(EXEMPT_PREFIXES)


def git(root: Path, *arguments: str) -> str:
    return subprocess.run(["git", "-C", str(root), *arguments], check=True,
                          capture_output=True, text=True).stdout


def added_lines(diff: str) -> dict[str, list[str]]:
    """Added and removed `add_test(` lines per file of a zero-context diff.

    Keys are the new path; removed lines sit under the key "-" so a line moved
    between files is still recognised as a move."""
    lines: dict[str, list[str]] = {"-": []}
    current = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            current = line[6:] if line.startswith("+++ b/") else None
            continue
        if line.startswith("--- "):
            continue
        if line.startswith("-") and ADD_TEST.search(line):
            lines["-"].append(" ".join(line[1:].split()))
        elif line.startswith("+") and current and ADD_TEST.search(line):
            lines.setdefault(current, []).append(" ".join(line[1:].split()))
    return lines


def violations(root: Path, base: str, head: str) -> list[str]:
    found = []
    created = git(root, "diff", "--name-only", "--diff-filter=A", "--find-renames",
                  base, head, "--", "tests").splitlines()
    for path in sorted(created):
        if not exempt(path) and NEW_TEST_FILE.fullmatch(path):
            found.append(f"{path}: new old-suite test file")
    cmake = [path for path in git(root, "diff", "--name-only", "--find-renames", base, head,
                                  "--", "tests").splitlines()
             if CMAKE_FILE.fullmatch(path) and not exempt(path)]
    if cmake:
        lines = added_lines(git(root, "diff", "-U0", "--find-renames", base, head, "--", *cmake))
        removed = Counter(lines.pop("-"))
        for path, added in sorted(lines.items()):
            for line in added:
                if removed[line]:
                    removed[line] -= 1
                    continue
                found.append(f"{path}: new add_test registration: {line}")
    return found


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("base")
    parser.add_argument("head")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    arguments = parser.parse_args(argv)
    found = violations(arguments.root, arguments.base, arguments.head)
    if found:
        for item in found:
            print(f"::error title=Test suite freeze::{item}" if os.environ.get("GITHUB_ACTIONS") == "true"
                  else f"test freeze: {item}")
        print(GUIDANCE)
        return 1
    print("test freeze: the candidate adds no old-suite tests")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
