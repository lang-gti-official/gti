#!/usr/bin/env python3

"""Require a surface review for every shipped stdlib source a change edits.

`tests/stdlib_surface_manifest.json` classifies the shipped standard-library
surface. A pull request that adds or modifies a shipped source must show that
someone reviewed its classification, and the evidence comes from the change
itself, the way an API-baseline check reads a diff, rather than from a stored
hash of every source that any two library changes would both rewrite.

For each `stdlib/prelude.gti`, `stdlib/std.gti` or `stdlib/std/**/*.gti` that
differs between `git merge-base <base> <head>` and `<head>` (added or
modified; a deletion is judged by the manifest's structural census), one of
the following must hold over the same range:

* the manifest adds, changes or removes an entry, compared by `id`, whose
  `source` names that file; or
* a commit in the range ends its message with a trailer block carrying
  `Stdlib-Surface: unchanged` and one nonempty `Stdlib-Surface-Reason:`,
  declaring that the edit leaves the classified surface as recorded.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from typing import Any


MANIFEST = "tests/stdlib_surface_manifest.json"
TRAILER = "Stdlib-Surface"
TRAILER_VALUE = "unchanged"
REASON_TRAILER = "Stdlib-Surface-Reason"


def git(*arguments: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        ["git", *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if check and result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or f"git {' '.join(arguments)} failed")
    return result


def is_surface_source(path: str) -> bool:
    """The shipped source census, as `stdlib_surface_test.stdlib_sources` lists it."""
    if path in {"stdlib/prelude.gti", "stdlib/std.gti"}:
        return True
    return path.startswith("stdlib/std/") and path.endswith(".gti")


def changed_sources(fork: str, head: str) -> list[str]:
    # Without rename folding a move is a deletion plus an addition, and the
    # added path needs its own classification.
    output = git(
        "diff", "--no-renames", "--name-status", "-z", fork, head, "--", "stdlib/"
    ).stdout.split("\0")
    sources = []
    for status, path in zip(output[0::2], output[1::2]):
        if status != "D" and is_surface_source(path):
            sources.append(path)
    return sorted(sources)


def manifest_entries(revision: str) -> dict[str, Any]:
    shown = git("show", f"{revision}:{MANIFEST}", check=False)
    if shown.returncode != 0:
        return {}
    try:
        manifest = json.loads(shown.stdout)
    except json.JSONDecodeError as error:
        raise ValueError(f"{revision}:{MANIFEST} is not valid JSON: {error}") from error
    entries = manifest.get("entries", []) if isinstance(manifest, dict) else []
    if not isinstance(entries, list):
        raise ValueError(f"{revision}:{MANIFEST} entries must be an array")
    return {
        entry["id"]: entry
        for entry in entries
        if isinstance(entry, dict) and isinstance(entry.get("id"), str)
    }


def reviewed_sources(fork: str, head: str) -> set[str]:
    """Sources named by any entry the range adds, changes or removes."""
    before, after = manifest_entries(fork), manifest_entries(head)
    reviewed = set()
    for entry_id in before.keys() | after.keys():
        old, new = before.get(entry_id), after.get(entry_id)
        if old == new:
            continue
        for entry in (old, new):
            if entry is not None and isinstance(entry.get("source"), str):
                reviewed.add(entry["source"])
    return reviewed


def trailer_values(revision: str, key: str) -> list[str]:
    # Git parses only the message's final trailer block, so two keys read from
    # the same revision are, by construction, in the same contiguous block.
    output = git(
        "show", "-s", f"--format=%(trailers:key={key},valueonly,unfold)", revision
    ).stdout
    return [line.strip() for line in output.splitlines() if line.strip()]


def unchanged_declaration(revision: str) -> str | None:
    """The reason a commit declares the surface unchanged, if it validly does."""
    values = trailer_values(revision, TRAILER)
    reasons = trailer_values(revision, REASON_TRAILER)
    if values != [TRAILER_VALUE] or len(reasons) != 1:
        return None
    return reasons[0]


def declarations(fork: str, head: str) -> dict[str, str]:
    revisions = git("rev-list", f"{fork}..{head}").stdout.split()
    found = {}
    for revision in revisions:
        reason = unchanged_declaration(revision)
        if reason is not None:
            found[revision] = reason
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("base", help="base Git revision")
    parser.add_argument("head", nargs="?", default="HEAD", help="head Git revision")
    arguments = parser.parse_args()

    head = git("rev-parse", f"{arguments.head}^{{commit}}").stdout.strip()
    fork = git("merge-base", arguments.base, head).stdout.strip()
    sources = changed_sources(fork, head)
    if not sources:
        print("No shipped stdlib source was added or modified; no surface review is required.")
        return 0

    reviewed = reviewed_sources(fork, head)
    unreviewed = [source for source in sources if source not in reviewed]
    if not unreviewed:
        print(f"Surface review covers every changed stdlib source: {', '.join(sources)}.")
        return 0

    declared = declarations(fork, head)
    if declared:
        revision, reason = next(iter(sorted(declared.items())))
        print(
            f"Commit {revision[:12]} declares the stdlib surface unchanged for "
            f"{', '.join(unreviewed)}: {reason}"
        )
        return 0

    print(
        "Stdlib sources changed without a surface review "
        f"(range {fork[:12]}..{head[:12]}):",
        file=sys.stderr,
    )
    for source in unreviewed:
        print(f"  {source}", file=sys.stderr)
    print(
        f"Review each source's classification in {MANIFEST}: add, change or "
        "remove at least one entry whose \"source\" names it (its operations, "
        "status, cases or evidence). If the edit leaves the classified surface "
        "exactly as recorded, end a commit in the range with the trailers\n"
        f"  {TRAILER}: {TRAILER_VALUE}\n"
        f"  {REASON_TRAILER}: <why the classified surface is unchanged>\n"
        "in one contiguous trailer block.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as error:
        print(f"stdlib surface review check failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
