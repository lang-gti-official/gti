#!/usr/bin/env python3
"""Assemble changelog fragments into a release: next VERSION, CHANGELOG section, consumed fragments.

Pull requests describe shipped changes in changes/<name>.<kind>.md instead of
editing VERSION or CHANGELOG.md. A release cut runs this script, reviews the
result and commits it with the printed `Release-Cut:` trailer; the commit goes
through the merge queue like any other change.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

KINDS = ("breaking", "feature", "fix", "internal")
FRAGMENT = re.compile(r"([a-z0-9][a-z0-9-]*)\.(breaking|feature|fix|internal)\.md")
VERSION = re.compile(r"(\d+)\.(\d+)\.(\d+)")


class CutError(ValueError):
    pass


def fragments(root: Path) -> list[tuple[str, str, Path]]:
    directory = root / "changes"
    rows = []
    for path in sorted(directory.glob("*.md")) if directory.is_dir() else []:
        if path.name == "README.md":
            continue
        match = FRAGMENT.fullmatch(path.name)
        if match is None:
            raise CutError(f"{path.name}: name a fragment <name>.<{'|'.join(KINDS)}>.md")
        text = path.read_text(encoding="utf-8").strip()
        if not text:
            raise CutError(f"{path.name}: a changelog fragment needs its user-facing text")
        rows.append((match.group(2), text, path))
    return rows


def next_version(current: str, kinds: set[str]) -> str:
    """Pre-1.0 (ADR 011): breaking changes and features take a minor release."""
    match = VERSION.fullmatch(current.strip())
    if match is None:
        raise CutError(f"VERSION is not a semantic version: {current!r}")
    major, minor, patch = (int(part) for part in match.groups())
    if kinds & {"breaking", "feature"}:
        return f"{major}.{minor + 1}.0"
    if "fix" in kinds:
        return f"{major}.{minor}.{patch + 1}"
    raise CutError("Only internal fragments are pending; there is nothing user-visible to release")


def bullet(text: str, breaking: bool) -> str:
    lines = text.splitlines()
    first = ("**Breaking:** " if breaking else "") + lines[0].lstrip("- ").strip()
    rest = [("  " + line) if line.strip() else "" for line in lines[1:]]
    return "\n".join(["- " + first, *rest])


def section(version: str, title: str, rows: list[tuple[str, str, Path]]) -> str:
    order = {kind: index for index, kind in enumerate(KINDS)}
    entries = [bullet(text, kind == "breaking")
               for kind, text, _ in sorted(rows, key=lambda row: (order[row[0]], row[2].name))
               if kind != "internal"]
    return f"## {version} - {title}\n\n" + "\n".join(entries) + "\n\n"


def cut(root: Path, title: str, apply: bool) -> str:
    if not title.strip() or "\n" in title:
        raise CutError("Give the release a one-line title")
    rows = fragments(root)
    if not rows:
        raise CutError("No changelog fragments are pending")
    current = (root / "VERSION").read_text(encoding="utf-8").strip()
    version = next_version(current, {kind for kind, _, _ in rows})
    changelog = (root / "CHANGELOG.md").read_text(encoding="utf-8")
    marker = changelog.find("\n## ")
    if marker < 0:
        raise CutError("CHANGELOG.md has no release section to precede")
    updated = changelog[:marker + 1] + section(version, title.strip(), rows) + changelog[marker + 1:]
    if apply:
        (root / "VERSION").write_text(version + "\n", encoding="utf-8")
        (root / "CHANGELOG.md").write_text(updated, encoding="utf-8")
        for _, _, path in rows:
            path.unlink()
    return version


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--title", required=True, help="one-line release title")
    parser.add_argument("--dry-run", action="store_true", help="report the next version only")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    args = parser.parse_args(argv)
    try:
        version = cut(Path(args.root), args.title, not args.dry_run)
    except CutError as error:
        print("cut_release: " + str(error), file=sys.stderr)
        return 1
    print(f"Next release: {version}")
    if not args.dry_run:
        print(f"Commit VERSION, CHANGELOG.md and the consumed fragments with the trailer:\n"
              f"  Release-Cut: {version}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
