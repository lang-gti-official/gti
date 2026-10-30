#!/usr/bin/env python3

from __future__ import annotations

import argparse
import re
import subprocess
import sys


RELEASE_FILES = {
    ".gitea/workflows/release.yml",
    ".github/workflows/nightly.yml",
    "CMakeLists.txt",
    "LICENSE",
    "VERSION",
    "build.lua",
    "lazy.lua",
    "scripts/install_neovim_local.py",
    "scripts/install_neovim_local.lua",
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
# A pull request describes its shipped change with one changelog fragment
# instead of editing VERSION or CHANGELOG.md; a release cut consumes them.
FRAGMENT_DIRECTORY = "changes/"
FRAGMENT_PATTERN = re.compile(r"changes/[a-z0-9][a-z0-9-]*\.(breaking|feature|fix|internal)\.md")
FRAGMENT_EXEMPT = {"changes/README.md"}


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


def release_cut(revision: str) -> str | None:
    values = git(
        "show", "-s", "--format=%(trailers:key=Release-Cut,valueonly,unfold)", revision,
    ).strip().splitlines()
    return values[0].strip() if len(values) == 1 and values[0].strip() else None


def fragment_errors(base: str, head: str) -> list[str]:
    errors = []
    for path in changed_paths(base, head):
        if not path.startswith(FRAGMENT_DIRECTORY) or path in FRAGMENT_EXEMPT:
            continue
        present = subprocess.run(["git", "cat-file", "-e", f"{head}:{path}"],
                                 check=False, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if present.returncode != 0:
            continue  # consumed by a release cut
        if FRAGMENT_PATTERN.fullmatch(path) is None:
            errors.append(f"{path}: name a fragment changes/<name>.<breaking|feature|fix|internal>.md")
        elif not git("show", f"{head}:{path}").strip():
            errors.append(f"{path}: a changelog fragment needs its user-facing text")
    return errors


def added_fragments(base: str, head: str) -> list[str]:
    paths = git("diff", "--no-renames", "--name-only", "--diff-filter=A", "-z",
                base, head, "--", FRAGMENT_DIRECTORY).split("\0")
    return [path for path in paths if FRAGMENT_PATTERN.fullmatch(path)]


def uncut_version_changes(base: str, head: str) -> list[str]:
    """Every commit that changes VERSION must be a release cut naming that version."""
    missing = []
    for revision in git("rev-list", "--reverse", f"{base}..{head}").splitlines():
        parents = git("rev-list", "--parents", "-n", "1", revision).split()[1:]
        paths = merge_introduced_paths(revision, parents) if len(parents) > 1 else \
            changed_paths(parents[0], revision)
        if "VERSION" in paths and release_cut(revision) != git("show", f"{revision}:VERSION").strip():
            missing.append(revision)
    return missing


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


def merge_introduced_paths(revision: str, parents: list[str]) -> list[str]:
    # A merge commit's own content is what differs from the automatic merge
    # of its parents: its conflict resolution. A clean automatic merge, as a
    # hosted pull-request merge or an upstream reconciliation produces, adds
    # nothing even when both sides touched the same files. Git's merge-tree
    # replays that automatic merge without touching the worktree; where it
    # reports conflicts, its tree carries markers that the resolution then
    # replaces, so the diff names exactly the resolved paths.
    if len(parents) == 2:
        result = subprocess.run(
            ["git", "merge-tree", "--write-tree", parents[0], parents[1]],
            check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        if result.returncode in (0, 1) and result.stdout.strip():
            automatic = result.stdout.splitlines()[0].strip()
            return changed_paths(automatic, revision)
    # Octopus merges, or a Git without merge-tree --write-tree: judge the
    # paths the merge changed relative to every parent.
    paths = set(changed_paths(parents[0], revision))
    for parent in parents[1:]:
        paths &= set(changed_paths(parent, revision))
    return sorted(paths)


def unclassified_changes(base: str, head: str) -> list[str]:
    # Check every delivered commit, not only the final message or the
    # first-parent line: a later documentation commit cannot excuse an earlier
    # shipped change, and a pull-request merge delivers its branch commits
    # through the second parent, each declared or versioned on its own.
    # A merge commit itself is judged only by the content it introduces
    # beyond the automatic merge of its parents.
    revisions = git("rev-list", "--reverse", f"{base}..{head}")
    missing = []
    for revision in revisions.splitlines():
        parents = git("rev-list", "--parents", "-n", "1", revision).split()[1:]
        if not parents:
            raise ValueError("the checked range unexpectedly includes a root commit")
        if len(parents) > 1:
            paths = merge_introduced_paths(revision, parents)
        else:
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

    if (git("ls-tree", "--name-only", arguments.head, ".gti-preview.json").strip() or
            git("log", "--format=%H", arguments.base + ".." + arguments.head, "--", ".gti-preview.json").strip()):
        raise ValueError("preview candidates cannot enter main; validate and merge each original PR")

    if git("merge-base", arguments.base, arguments.head).strip() != git(
        "rev-parse", f"{arguments.base}^{{commit}}"
    ).strip():
        raise ValueError("base must be an ancestor of head")
    errors = fragment_errors(arguments.base, arguments.head)
    if errors:
        print("Invalid changelog fragments:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
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
    uncut = uncut_version_changes(arguments.base, arguments.head)
    if uncut:
        print("VERSION changes only in a release cut (scripts/cut_release.py), whose commit "
              "carries a 'Release-Cut: <new VERSION>' trailer. Describe a change with a "
              "changelog fragment instead. Commits changing VERSION without one:", file=sys.stderr)
        for revision in uncut:
            print(f"  {revision}", file=sys.stderr)
        return 1
    if head_version > base_version:
        print(f"Release cut advances VERSION from {base_text} to {head_text}.")
        return 0
    fragments = added_fragments(arguments.base, arguments.head)
    if fragments:
        print(f"Shipped changes are described by {', '.join(fragments)}; the next release "
              f"cut assembles them. VERSION stays {head_text}.")
        return 0
    missing = unclassified_changes(arguments.base, arguments.head)
    if not missing:
        print(
            f"Internal-only changes retain VERSION {head_text}; nightly "
            "publishes them without a stable release."
        )
        return 0
    print("Release-sensitive files changed without a changelog fragment:", file=sys.stderr)
    for path in shipped:
        print(f"  {path}", file=sys.stderr)
    print("Commits without an internal-only declaration:", file=sys.stderr)
    for revision in missing:
        print(f"  {revision}", file=sys.stderr)
    print(
        "Describe the change in changes/<name>.<breaking|feature|fix|internal>.md. "
        "Without a fragment, each relevant commit needs 'Release-Impact: internal' and "
        "a nonempty 'Release-Reason:' Git trailer. This is a reviewed impact "
        "declaration, not permission to omit a user-visible change.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, ValueError) as error:
        print(f"release version check failed: {error}", file=sys.stderr)
        raise SystemExit(2) from error
