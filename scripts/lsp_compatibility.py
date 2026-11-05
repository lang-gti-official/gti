#!/usr/bin/env python3
"""Exact shared-input identity for independently rebuilt GTI language servers."""
import argparse
import hashlib
from pathlib import Path
import subprocess


def identity(source, revision="HEAD"):
    rows = subprocess.check_output(
        ["git", "-C", str(source), "ls-tree", "-rz", revision])
    excluded = ("src/lsp/", "tests/", "examples/", "docs/", "changes/", ".codex/",
                ".claude/", ".agents/", ".gti-validation/", "lua/", "lsp/", "plugin/")
    names = {"AGENTS.md", "CLAUDE.md", "README.md", "CHANGELOG.md"}
    included = []
    for row in rows.split(b"\0"):
        if not row:
            continue
        name = row.split(b"\t", 1)[1].decode()
        if name not in names and not name.startswith(excluded):
            included.append(row)
    if not included:
        raise ValueError("empty shared compiler input inventory")
    return hashlib.sha256(b"\0".join(included)).hexdigest()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--revision", default="HEAD")
    args = parser.parse_args()
    print(identity(args.source, args.revision))
