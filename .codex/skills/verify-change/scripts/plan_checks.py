#!/usr/bin/env python3
"""Summarize GTI change surfaces and candidate verification commands."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


@dataclass(frozen=True)
class Check:
    name: str
    command: str
    reason: str


def git(repo: Path, *args: str) -> bytes:
    result = subprocess.run(
        ["git", *args],
        cwd=repo,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        message = result.stderr.decode(errors="replace").strip()
        raise RuntimeError(message or f"git {' '.join(args)} failed")
    return result.stdout


def repository_root(start: Path) -> Path:
    output = git(start, "rev-parse", "--show-toplevel")
    return Path(output.decode().strip()).resolve()


def nul_paths(output: bytes) -> set[str]:
    return {
        item.decode(errors="surrogateescape")
        for item in output.split(b"\0")
        if item
    }


def changed_paths(
    repo: Path, base: str | None, staged: bool, include_untracked: bool
) -> tuple[list[str], str]:
    if staged:
        tracked = nul_paths(
            git(
                repo,
                "diff",
                "--cached",
                "--name-only",
                "--diff-filter=ACMRTUXB",
                "-z",
                "HEAD",
            )
        )
        scope = "staged index against HEAD"
    elif base:
        merge_base = git(repo, "merge-base", base, "HEAD").decode().strip()
        tracked = nul_paths(
            git(repo, "diff", "--name-only", "--diff-filter=ACMRTUXB", "-z", merge_base)
        )
        scope = f"merge base {merge_base} of {base} and HEAD through the working tree"
    else:
        tracked = nul_paths(
            git(repo, "diff", "--name-only", "--diff-filter=ACMRTUXB", "-z", "HEAD")
        )
        scope = "HEAD through the working tree"

    if include_untracked and not staged:
        tracked.update(nul_paths(git(repo, "ls-files", "--others", "--exclude-standard", "-z")))

    return sorted(tracked), scope


def under(path: str, *prefixes: str) -> bool:
    return any(path == prefix.rstrip("/") or path.startswith(prefix) for prefix in prefixes)


def classify(paths: Iterable[str]) -> dict[str, list[str]]:
    groups: dict[str, list[str]] = {
        "compiler/frontend": [],
        "driver/cli/project": [],
        "lsp/editor": [],
        "syntax/tooling": [],
        "stdlib/runtime": [],
        "build/release": [],
        "tests/examples/benchmarks": [],
        "documentation/plans": [],
        "codex automation": [],
        "other": [],
    }

    for path in paths:
        matched = False

        def add(group: str) -> None:
            nonlocal matched
            groups[group].append(path)
            matched = True

        if under(path, "src/compiler/", "include/gti/") and not under(path, "include/gti/driver/"):
            add("compiler/frontend")
        if under(path, "src/driver/", "include/gti/driver/", "src/cli/"):
            add("driver/cli/project")
        if under(path, "src/lsp/", "lsp/", "editors/", "nvim/") or path in {
            "tests/lsp_smoke_test.py",
            "tests/nvim_plugin_smoke_test.lua",
        }:
            add("lsp/editor")
        if under(path, "tree-sitter-gti/") or "format" in Path(path).name.lower():
            add("syntax/tooling")
        if under(path, "stdlib/", "runtime/"):
            add("stdlib/runtime")
        if (
            path == "CMakeLists.txt"
            or path == "VERSION"
            or under(path, "cmake/", ".github/workflows/")
            or path in {
                "scripts/check_release_version.py",
                "scripts/check_llvm_link_surface.py",
            }
        ):
            add("build/release")
        if under(path, "tests/", "examples/", "benchmarks/"):
            add("tests/examples/benchmarks")
        if path == "README.md" or path == "AGENTS.md" or under(path, "docs/"):
            add("documentation/plans")
        if under(path, ".codex/", ".agents/"):
            add("codex automation")
        if not matched:
            groups["other"].append(path)

    return {name: values for name, values in groups.items() if values}


def candidate_checks(paths: list[str], groups: dict[str, list[str]]) -> list[Check]:
    checks: list[Check] = [
        Check("diff", "git diff --check", "reject whitespace and patch-format errors"),
    ]

    first_party_cpp = any(
        Path(path).suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}
        and not under(path, "tree-sitter-gti/src/parser.c", "build/", "build-")
        for path in paths
    )
    needs_build = any(
        name in groups
        for name in (
            "compiler/frontend",
            "driver/cli/project",
            "lsp/editor",
            "stdlib/runtime",
            "build/release",
        )
    )

    if first_party_cpp:
        checks.append(
            Check(
                "format",
                "python3 scripts/clang_format.py --check",
                "first-party C or C++ changed",
            )
        )
    if needs_build:
        checks.append(Check("build", "cmake --build build -j4", "compiled surfaces changed"))
    if "compiler/frontend" in groups:
        checks.append(
            Check(
                "compiler",
                "ctest --test-dir build --output-on-failure -R '^compiler_pipeline$'",
                "frontend or compiler code changed; expand for affected IR/backend suites",
            )
        )
    if "driver/cli/project" in groups:
        checks.append(
            Check(
                "driver-project",
                "ctest --test-dir build --output-on-failure -R '^(driver_library_boundary|driver_pipeline|project_model|cli_workflow|project_cli_workflow)$'",
                "driver, project model, or CLI behavior changed",
            )
        )
    if "lsp/editor" in groups:
        checks.append(
            Check(
                "lsp",
                "ctest --test-dir build --output-on-failure -R '^lsp_protocol$'",
                "LSP or editor behavior changed",
            )
        )
    if "syntax/tooling" in groups and any(under(path, "tree-sitter-gti/") for path in paths):
        checks.extend(
            [
                Check(
                    "tree-sitter-generate",
                    "npm --prefix tree-sitter-gti run generate",
                    "Tree-sitter grammar or generated source changed",
                ),
                Check(
                    "tree-sitter-tests",
                    "npm --prefix tree-sitter-gti test",
                    "Tree-sitter corpus changed",
                ),
                Check(
                    "tree-sitter-highlights",
                    "npm --prefix tree-sitter-gti run test:highlights",
                    "Tree-sitter highlighting may be affected",
                ),
                Check(
                    "tree-sitter-shipped",
                    "npm --prefix tree-sitter-gti run test:shipped",
                    "shipped GTI sources must continue to parse",
                ),
                Check(
                    "tree-sitter-generated-diff",
                    "git diff --exit-code -- tree-sitter-gti/src",
                    "generated parser must match the grammar",
                ),
            ]
        )
    if "stdlib/runtime" in groups:
        checks.append(
            Check(
                "stdlib-runtime",
                "ctest --test-dir build --output-on-failure -R '^(compiler_pipeline|cli_workflow|project_cli_workflow)$'",
                "stdlib or runtime behavior changed; expand for the owning feature suite",
            )
        )
    if "build/release" in groups:
        checks.append(
            Check(
                "build-release",
                "ctest --test-dir build --output-on-failure",
                "build, packaging, or release behavior can affect the full matrix",
            )
        )

    seen: set[str] = set()
    unique: list[Check] = []
    for check in checks:
        if check.command not in seen:
            seen.add(check.command)
            unique.append(check)
    return unique


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Summarize changed GTI surfaces and candidate verification commands."
    )
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument(
        "--base",
        help="Compare from the merge base of this ref through the current working tree.",
    )
    scope.add_argument(
        "--staged",
        action="store_true",
        help="Inspect only changes staged in the index against HEAD.",
    )
    parser.add_argument(
        "--no-untracked",
        action="store_true",
        help="Exclude untracked files from the change surface.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format (default: text).",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        repo = repository_root(Path.cwd())
        paths, scope = changed_paths(repo, args.base, args.staged, not args.no_untracked)
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    groups = classify(paths)
    checks = candidate_checks(paths, groups)
    payload = {
        "repository": str(repo),
        "scope": scope,
        "changed_files": paths,
        "surfaces": groups,
        "candidate_checks": [check.__dict__ for check in checks],
        "advisory": (
            "Candidate checks are a lower bound. Inspect the diff and "
            "docs/architecture/verification.md before omitting or adding gates."
        ),
    }

    if args.format == "json":
        json.dump(payload, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return 0

    print(f"Repository: {repo}")
    print(f"Scope: {scope}")
    print(f"Changed files: {len(paths)}")
    if not paths:
        print("  (none)")
    else:
        for name, values in groups.items():
            print(f"\n{name} ({len(values)}):")
            for path in values:
                print(f"  - {path}")
    print("\nCandidate checks:")
    for check in checks:
        print(f"  [{check.name}] {check.command}")
        print(f"    {check.reason}")
    print(f"\nAdvisory: {payload['advisory']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
