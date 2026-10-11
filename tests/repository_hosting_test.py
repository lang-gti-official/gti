#!/usr/bin/env python3

import pathlib
import re
import subprocess
import sys


CANONICAL_ROOT = "https://git.internal.howetotech.dev/gti-lang/gti"
HISTORICAL_EXCEPTIONS = {
    "docs/plans/compiler-structural-baseline.json",
}
HISTORICAL_PREFIXES = ("docs/plans/archive/",)
FORBIDDEN = (
    "https://git2." + "internal.howetotech.dev",
    "https://" + "github.com/LukeHHA/" + "gti",
    "git@" + "github.com:LukeHHA/" + "gti",
    ".github" + "/workflows/",
)
ACTION_USE = re.compile(r"^\s*uses:\s*(\S+)@([^\s#]+)", re.MULTILINE)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def text(root: pathlib.Path, relative: str) -> str:
    path = root / relative
    expect(path.is_file(), f"required repository file is missing: {relative}")
    return path.read_text(encoding="utf-8")


def tracked_files(root: pathlib.Path) -> list[str]:
    result = subprocess.run(
        ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
        cwd=root,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise AssertionError(
            "could not enumerate tracked files: "
            + result.stderr.decode("utf-8", errors="replace")
        )
    return [
        relative
        for item in result.stdout.split(b"\0")
        if item
        and (relative := item.decode("utf-8"))
        and (root / relative).is_file()
    ]


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: repository_hosting_test.py <repository-root>")
    root = pathlib.Path(sys.argv[1]).resolve()

    tracked = tracked_files(root)
    github_files = [path for path in tracked if path.startswith(".github/")]
    expect(not github_files, f"legacy GitHub metadata remains tracked: {github_files}")

    violations: list[str] = []
    for relative in tracked:
        if relative in HISTORICAL_EXCEPTIONS or relative.startswith(
            HISTORICAL_PREFIXES
        ):
            continue
        path = root / relative
        try:
            contents = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        found = [needle for needle in FORBIDDEN if needle in contents]
        if found:
            violations.append(f"{relative}: {', '.join(found)}")
    expect(
        not violations,
        "retired first-party repository dependencies remain:\n" + "\n".join(violations),
    )

    readme = text(root, "README.md")
    expect(
        f"git clone {CANONICAL_ROOT}.git gti" in readme,
        "README clone command is not canonical Gitea",
    )
    expect(
        f'"{CANONICAL_ROOT}.git"' in readme,
        "README Lazy specification is not canonical Gitea",
    )

    lazy = text(root, "lazy.lua")
    expect(
        f'"{CANONICAL_ROOT}.git"' in lazy and 'name = "gti"' in lazy,
        "Lazy package metadata is not pinned to canonical Gitea",
    )
    installer = text(root, "lua/gti/installer.lua")
    expect(
        'local release_server = "https://git.internal.howetotech.dev"'
        in installer
        and 'local repository = "gti-lang/gti"' in installer,
        "toolchain installer is not pinned to canonical Gitea releases",
    )

    expect(
        not any(relative.startswith("reference/") for relative in tracked),
        "the website must live only in gti-lang/gti-reference, not compiler source",
    )
    expect(
        (root / "src/docgen/main.cpp").is_file()
        and '"src/docgen/main.cpp"' in text(root, "CMakeLists.txt"),
        "the compiler-owned documentation producer was lost with the website",
    )
    reference_ownership = text(root, "docs/architecture/reference-documentation.md")
    expect(
        "gti-lang/gti-reference" in reference_ownership
        and "Repository Ownership And Migration" in reference_ownership,
        "the standalone website's ownership/migration rationale is undocumented",
    )
    # Gitea Actions are archived (2026-10-11): the self-hosted runner could not
    # carry the suite, and CI hosting moves to the gti-lang GitHub
    # organization. The record stays readable; nothing under .gitea executes.
    gitea_files = [path for path in tracked if path.startswith(".gitea/")]
    expect(not gitea_files, f"archived Gitea Actions metadata remains live: {gitea_files}")
    for relative in (
        "docs/plans/archive/gitea-actions/README.md",
        "docs/plans/archive/gitea-actions/workflows/ci.yml",
        "docs/plans/archive/gitea-actions/workflows/release.yml",
    ):
        expect((root / relative).is_file(), f"archived Gitea workflow record is missing: {relative}")
    expect(
        "archived 2026-10-11" in text(root, "docs/plans/archive/gitea-actions/README.md"),
        "the archived Gitea Actions record does not say it is archived",
    )
    readme_badges = re.findall(r"actions/workflows/\S+/badge\.svg", readme)
    expect(not readme_badges, f"README advertises archived workflow status: {readme_badges}")
    version_check = text(root, "scripts/check_release_version.py")
    expect(
        ".gitea/" not in version_check,
        "release-version policy still classifies an archived Gitea workflow as shipped",
    )
    for skill in (
        ".codex/skills/finish-release/SKILL.md",
        ".claude/skills/finish-release/SKILL.md",
    ):
        contents = text(root, skill)
        expect(
            "canonical Gitea" in contents and "Gitea release" in contents,
            f"delivery instructions are not Gitea-authoritative: {skill}",
        )


if __name__ == "__main__":
    main()
