#!/usr/bin/env python3
"""Check that editor queries only reference nodes the grammar produces.

The Neovim plugin loads `queries/gti/*.scm` from its checkout and the compiled
parser from the released toolchain, so a query that names a node the grammar no
longer produces does not fail here: it fails later, inside whichever plugin
first renders a GTI buffer, as an opaque Tree-sitter node-type error.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

# `(name` opens a node pattern; `name:` introduces a field.
NODE_PATTERN = re.compile(r"\(([a-z_][a-z0-9_]*)")
FIELD_PATTERN = re.compile(r"\b([a-z_][a-z0-9_]*):\s")


def load_grammar(node_types: Path) -> tuple[set[str], set[str]]:
    entries = json.loads(node_types.read_text())
    named = {entry["type"] for entry in entries if entry.get("named")}
    fields = {field for entry in entries for field in entry.get("fields", {})}
    return named, fields


def check(queries: Path, named: set[str], fields: set[str]) -> list[str]:
    failures: list[str] = []
    for path in sorted(queries.glob("*.scm")):
        for number, line in enumerate(path.read_text().splitlines(), 1):
            code = line.split(";")[0]
            for match in NODE_PATTERN.finditer(code):
                if match.group(1) not in named:
                    failures.append(
                        f"{path.name}:{number}: unknown node type "
                        f"'{match.group(1)}'"
                    )
            for match in FIELD_PATTERN.finditer(code):
                if match.group(1) not in fields:
                    failures.append(
                        f"{path.name}:{number}: unknown field "
                        f"'{match.group(1)}'"
                    )
    return failures


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        print("usage: tree_sitter_query_test.py <node-types.json> <queries-dir>")
        return 2

    node_types = Path(argv[1])
    queries = Path(argv[2])
    if not node_types.is_file():
        print(f"FAIL: missing generated grammar node types: {node_types}")
        return 1
    if not queries.is_dir():
        print(f"FAIL: missing query directory: {queries}")
        return 1

    named, fields = load_grammar(node_types)
    if not named:
        print(f"FAIL: {node_types} declares no named node types")
        return 1

    scms = sorted(queries.glob("*.scm"))
    if not scms:
        print(f"FAIL: {queries} contains no .scm query files")
        return 1

    failures = check(queries, named, fields)
    if failures:
        print("FAIL: editor queries reference nodes the grammar does not have")
        for failure in failures:
            print(f"  {failure}")
        print("Regenerate the parser or update the query after a grammar change.")
        return 1

    print(
        f"ok: {len(scms)} query files match {len(named)} named node types "
        f"and {len(fields)} fields"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
