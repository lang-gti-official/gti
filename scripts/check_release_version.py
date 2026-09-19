#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import sys


RELEASE_FILES = {
    ".gitea/workflows/release.yml",
    "CMakeLists.txt",
    "LICENSE",
    "VERSION",
    "build.lua",
    "lazy.lua",
}

RELEASE_PREFIXES = (
    "cmake/",
    "ftdetect/",
    "ftplugin/",
    "include/gti/",
    "lsp/",
    "lua/gti/",
    "plugin/",
    "queries/gti/",
    "runtime/",
    "src/",
    "stdlib/",
    "syntax/",
    "tree-sitter-gti/",
    "vendor/",
)

VERSION_PATTERN = re.compile(r"^(\d+)\.(\d+)\.(\d+)$")


def git(*arguments: str) -> str:
    result = subprocess.run(
        ["git", *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "git command failed")
    return result.stdout


def parse_version(value: str, source: str) -> tuple[int, int, int]:
    match = VERSION_PATTERN.fullmatch(value.strip())
    if match is None:
        raise ValueError(f"{source} does not contain a semantic version: {value!r}")
    return tuple(int(part) for part in match.groups())


def version_at(revision: str) -> tuple[str, tuple[int, int, int]]:
    value = git("show", f"{revision}:VERSION").strip()
    return value, parse_version(value, f"{revision}:VERSION")


def is_release_file(path: str) -> bool:
    return path in RELEASE_FILES or path.startswith(RELEASE_PREFIXES)


def changed_paths(base: str, head: str) -> list[str]:
    # Deleting or moving a shipped file out of a shipped directory can change
    # the toolchain just as much as adding it. Disable rename folding so both
    # sides of a move participate in the classification.
    return [path for path in git(
        "diff", "--no-renames", "--name-only", "-z", base, head
    ).split("\0") if path]


def internal_change_reason(revision: str) -> str | None:
    impact = git(
        "show", "-s", "--format=%(trailers:key=Release-Impact,valueonly,unfold)",
        revision,
    ).strip()
    reasons = git(
        "show", "-s", "--format=%(trailers:key=Release-Reason,valueonly,unfold)",
        revision,
    ).strip().splitlines()
    if impact != "internal" or len(reasons) != 1 or not reasons[0].strip():
        return None
    return reasons[0].strip()


def unclassified_changes(base: str, head: str) -> list[str]:
    # Check each delivered first-parent change, not only the final message:
    # a later documentation commit cannot excuse an earlier shipped change.
    # A merge is judged by its full delta from the destination parent.
    revisions = git("rev-list", "--reverse", "--first-parent", f"{base}..{head}")
    missing = []
    for revision in revisions.splitlines():
        parents = git("rev-list", "--parents", "-n", "1", revision).split()[1:]
        if not parents:
            raise ValueError("the checked range unexpectedly includes a root commit")
        paths = changed_paths(parents[0], revision)
        if not any(is_release_file(path) for path in paths):
            continue
        # An internal-only assertion cannot authorize version churn or undo a
        # stable-version change within the delivered range.
        if "VERSION" in paths or internal_change_reason(revision) is None:
            missing.append(revision)
    return missing


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate stable versions and explicit internal-only GTI changes."
    )
    parser.add_argument("base", help="base Git revision")
    parser.add_argument("head", nargs="?", default="HEAD", help="head Git revision")
    arguments = parser.parse_args()

    if git("merge-base", arguments.base, arguments.head).strip() != git(
        "rev-parse", f"{arguments.base}^{{commit}}"
    ).strip():
        raise ValueError("base must be an ancestor of head")
    changed = changed_paths(arguments.base, arguments.head)
    shipped = [path for path in changed if is_release_file(path)]
    if not shipped:
        print("No shipped compiler or editor tooling changed.")
        return 0

    base_text, base_version = version_at(arguments.base)
    head_text, head_version = version_at(arguments.head)
    if head_version < base_version:
        print(f"VERSION cannot decrease from {base_text} to {head_text}.", file=sys.stderr)
        return 1
    if head_version == base_version:
        missing = unclassified_changes(arguments.base, arguments.head)
        if not missing:
            print(
                f"Internal-only changes retain VERSION {head_text}; nightly "
                "publishes them without a stable release."
            )
            return 0
        print(
            "Release-sensitive files changed without advancing VERSION ",
            f"beyond {base_text}:",
            sep="",
            file=sys.stderr,
        )
        for path in shipped:
            print(f"  {path}", file=sys.stderr)
        print("Commits without an internal-only declaration:", file=sys.stderr)
        for revision in missing:
            print(f"  {revision}", file=sys.stderr)
        print(
            "User-visible changes must advance VERSION. For internal-only work, "
            "each relevant commit instead needs 'Release-Impact: internal' and "
            "a nonempty 'Release-Reason:' Git trailer. This is a reviewed impact "
            "declaration, not permission to omit a user-visible version change.",
            file=sys.stderr,
        )
        return 1

    print(
        f"Release-sensitive changes advance VERSION from {base_text} to {head_text}."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as error:
        print(f"release version check failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
