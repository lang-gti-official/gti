#!/usr/bin/env python3

import argparse
import re
import subprocess
import sys


RELEASE_FILES = {
    ".github/workflows/release.yml",
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


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Require shipped GTI changes to advance VERSION."
    )
    parser.add_argument("base", help="base Git revision")
    parser.add_argument("head", nargs="?", default="HEAD", help="head Git revision")
    arguments = parser.parse_args()

    changed = [
        path
        for path in git(
            "diff",
            "--name-only",
            "--diff-filter=ACMR",
            arguments.base,
            arguments.head,
        ).splitlines()
        if path
    ]
    shipped = [path for path in changed if is_release_file(path)]
    if not shipped:
        print("No shipped compiler or editor tooling changed.")
        return 0

    base_text, base_version = version_at(arguments.base)
    head_text, head_version = version_at(arguments.head)
    if head_version <= base_version:
        print(
            "Release-sensitive files changed without advancing VERSION ",
            f"beyond {base_text}:",
            sep="",
            file=sys.stderr,
        )
        for path in shipped:
            print(f"  {path}", file=sys.stderr)
        print(
            "Update VERSION to a greater semantic version. A main-branch VERSION "
            "change publishes the matching release automatically.",
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
