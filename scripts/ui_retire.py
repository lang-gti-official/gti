#!/usr/bin/env python3
"""Remove converted CTest entries from every registry that names them.

Conversion aid for the test-suite redesign (docs/plans/test-suite-redesign.md,
phase 3); it is deleted with the old suite. For each CTest name it removes the
`add_test` block and the `set_tests_properties`/`set_property` calls naming
only that test from tests/CMakeLists.txt, the name from tests/ownership.toml,
tests/ci_test_durations.json, tests/ci_darwin_tests.json and the name lists of
tests/TestThroughput.cmake, and with `--script` deletes the harness script.
It refuses a name that a `foreach` loop or another construct still registers,
and prints every other tracked mention left for the caller to resolve.

    scripts/ui_retire.py constexpr_chrono_runtime macro_values_runtime \
        [--script tests/constexpr_text_runtime_test.py]
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parent.parent
CMAKE = REPOSITORY / "tests/CMakeLists.txt"


def call_span(text: str, start: int) -> int:
    depth = 0
    index = start
    while index < len(text):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    raise ValueError("unbalanced call")


def remove_calls(text: str, name: str) -> tuple[str, int]:
    removed = 0
    pattern = re.compile(
        r"[ \t]*(add_test\(\s*NAME\s+" + re.escape(name) + r"\s"
        r"|set_tests_properties\(\s*" + re.escape(name) + r"\s+PROPERTIES"
        r"|set_property\(\s*TEST\s+" + re.escape(name) + r"\s)")
    while True:
        match = pattern.search(text)
        if not match:
            return text, removed
        open_paren = text.index("(", match.start())
        end = call_span(text, open_paren)
        if end < len(text) and text[end] == "\n":
            end += 1
        text = text[:match.start()] + text[end:]
        removed += 1


def remove_list_word(text: str, name: str) -> str:
    """Drop a bare list item (a CMake list word or a quoted TOML string),
    with the number that pairs with it in a `name value` list."""
    text = re.sub(r"(?m)^[ \t]*" + re.escape(name) + r"[ \t]+\d+[ \t]*\n", "", text)
    text = re.sub(r"(?m)^[ \t]*\"" + re.escape(name) + r"\",?[ \t]*\n", "", text)
    text = re.sub(r"(?m)^[ \t]*" + re.escape(name) + r"[ \t]*\n", "", text)
    text = re.sub(r"(?<=[ \t])" + re.escape(name) + r"(?=[ \t\n)])[ \t]?", "", text)
    text = re.sub(r"\"" + re.escape(name) + r"\",[ \t]?", "", text)
    text = re.sub(r",?[ \t]?\"" + re.escape(name) + r"\"(?=[ \t]*\])", "", text)
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="+")
    parser.add_argument("--script", action="append", default=[], type=Path)
    arguments = parser.parse_args()

    cmake = CMAKE.read_text()
    for name in arguments.names:
        cmake, count = remove_calls(cmake, name)
        if count == 0:
            print(f"no add_test for {name}", file=sys.stderr)
        if re.search(r"(?<![\w-])" + re.escape(name) + r"(?![\w-])", cmake):
            print(f"{name} is still mentioned in tests/CMakeLists.txt", file=sys.stderr)
    CMAKE.write_text(cmake)

    for relative in ("tests/ownership.toml", "tests/TestThroughput.cmake"):
        path = REPOSITORY / relative
        text = path.read_text()
        for name in arguments.names:
            text = remove_list_word(text, name)
        path.write_text(text)

    durations = REPOSITORY / "tests/ci_test_durations.json"
    data = json.loads(durations.read_text())
    for name in arguments.names:
        data["seconds"].pop(name, None)
    durations.write_text(json.dumps(data, indent=1) + "\n")

    darwin = REPOSITORY / "tests/ci_darwin_tests.json"
    data = json.loads(darwin.read_text())
    data["tests"] = [t for t in data["tests"] if t not in arguments.names]
    darwin.write_text(json.dumps(data, indent=2) + "\n")

    for script in arguments.script:
        path = script if script.is_absolute() else REPOSITORY / script
        subprocess.run(["git", "rm", "-q", str(path)], cwd=REPOSITORY, check=True)

    words = arguments.names + [s.name for s in arguments.script]
    mentions = subprocess.run(
        ["git", "grep", "-n", "-w", "-E", "|".join(re.escape(w) for w in words), "--",
         ":!docs/", ":!CHANGELOG.md"],
        cwd=REPOSITORY, capture_output=True, text=True).stdout
    if mentions:
        print("remaining mentions:\n" + mentions)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
