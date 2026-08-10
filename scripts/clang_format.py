#!/usr/bin/env python3

"""Format or verify GTI's curated first-party C and C++ sources."""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
VERSION_FILE = REPOSITORY_ROOT / ".clang-format-version"
SOURCE_ROOTS = (
    "include/gti",
    "src",
    "runtime/include/gti",
    "runtime/src",
    "tests",
    "examples/gti-vs-cpp",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp"}


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--check",
        action="store_true",
        help="fail when a source file is not canonically formatted",
    )
    action.add_argument(
        "--write",
        action="store_true",
        help="rewrite source files in canonical format",
    )
    parser.add_argument(
        "--clang-format",
        default=os.environ.get("GTI_CLANG_FORMAT", "clang-format"),
        help="clang-format executable (default: GTI_CLANG_FORMAT or clang-format)",
    )
    return parser.parse_args()


def formatter_path(requested: str) -> str:
    resolved = shutil.which(requested)
    if resolved is None:
        raise RuntimeError(f"clang-format executable not found: {requested}")
    return resolved


def require_pinned_version(formatter: str) -> str:
    expected = VERSION_FILE.read_text(encoding="utf-8").strip()
    completed = subprocess.run(
        [formatter, "--version"],
        check=True,
        capture_output=True,
        text=True,
    )
    match = re.search(r"clang-format version ([0-9]+\.[0-9]+\.[0-9]+)", completed.stdout)
    if match is None:
        raise RuntimeError(
            f"could not determine clang-format version from: {completed.stdout.strip()}"
        )
    actual = match.group(1)
    if actual != expected:
        raise RuntimeError(
            f"GTI requires clang-format {expected}, but {formatter} is {actual}"
        )
    return actual


def source_files() -> list[Path]:
    result: list[Path] = []
    for relative_root in SOURCE_ROOTS:
        root = REPOSITORY_ROOT / relative_root
        result.extend(
            path
            for path in root.rglob("*")
            if path.is_file() and path.suffix in SOURCE_SUFFIXES
        )
    return sorted(result)


def main() -> int:
    arguments = parse_arguments()
    try:
        formatter = formatter_path(arguments.clang_format)
        version = require_pinned_version(formatter)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"clang-format setup error: {error}", file=sys.stderr)
        return 2

    files = source_files()
    if not files:
        print("No first-party C or C++ sources found.", file=sys.stderr)
        return 2

    relative_files = [str(path.relative_to(REPOSITORY_ROOT)) for path in files]
    command = [formatter]
    if arguments.check:
        command.extend(("--dry-run", "--Werror"))
    else:
        command.append("-i")
    command.extend(relative_files)

    completed = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False)
    if completed.returncode != 0:
        return completed.returncode

    action = "Checked" if arguments.check else "Formatted"
    print(f"{action} {len(files)} files with clang-format {version}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
