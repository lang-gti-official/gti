#!/usr/bin/env python3
"""GitHub is the canonical host; Gitea is the backup mirror; nothing points at retired hosts."""

import pathlib
import re
import subprocess
import sys


CANONICAL_ROOT = "https://github.com/lang-gti-official/gti"
MIRROR_ROOT = "https://git.internal.howetotech.dev/gti-lang/gti"
HISTORICAL_EXCEPTIONS = {
    "docs/plans/compiler-structural-baseline.json",
}
HISTORICAL_PREFIXES = ("docs/plans/archive/",)
FORBIDDEN = (
    "https://git2." + "internal.howetotech.dev",
    "https://" + "github.com/LukeHHA/" + "gti",
    "git@" + "github.com:LukeHHA/" + "gti",
    ".gitea" + "/workflows/",
)
ACTION_USE = re.compile(r"^\s*uses:\s*(\S+)@([^\s#]+)", re.MULTILINE)
FIRST_PARTY_ACTIONS = ("actions/",)
WORKFLOWS = (".github/workflows/ci.yml", ".github/workflows/release.yml", ".github/workflows/mirror.yml")


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
    # Gitea Actions are archived (2026-10-11); GitHub hosts the live workflows.
    gitea_files = [path for path in tracked if path.startswith(".gitea/")]
    expect(not gitea_files, f"archived Gitea Actions metadata remains live: {gitea_files}")
    for relative in (
        "docs/plans/archive/gitea-actions/README.md",
        "docs/plans/archive/gitea-actions/workflows/ci.yml",
        "docs/plans/archive/gitea-actions/workflows/release.yml",
        ".github/ISSUE_TEMPLATE/bug_report.md",
        ".github/README.md",
        *WORKFLOWS,
    ):
        expect((root / relative).is_file(), f"hosting record or workflow is missing: {relative}")
    expect(
        "archived 2026-10-11" in text(root, "docs/plans/archive/gitea-actions/README.md"),
        "the archived Gitea Actions record does not say it is archived",
    )

    violations: list[str] = []
    for relative in tracked:
        if relative in HISTORICAL_EXCEPTIONS or relative.startswith(HISTORICAL_PREFIXES):
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
        "README clone command is not canonical GitHub",
    )
    expect(
        f'"{CANONICAL_ROOT}.git"' in readme,
        "README Lazy specification is not canonical GitHub",
    )
    expect(
        MIRROR_ROOT in readme and "mirror" in readme,
        "README does not name the Gitea backup mirror",
    )
    expect(
        not re.findall(r"actions/workflows/\S+/badge\.svg", readme)
        or f"{CANONICAL_ROOT}/actions/workflows/ci.yml/badge.svg" in readme,
        "README workflow badges do not come from the canonical GitHub repository",
    )

    lazy = text(root, "lazy.lua")
    expect(
        f'"{CANONICAL_ROOT}.git"' in lazy and 'name = "gti"' in lazy,
        "Lazy package metadata is not pinned to canonical GitHub",
    )
    installer = text(root, "lua/gti/installer.lua")
    expect(
        'local release_server = "https://github.com"' in installer
        and 'local repository = "lang-gti-official/gti"' in installer,
        "toolchain installer is not pinned to canonical GitHub releases",
    )
    attributes = text(root, ".gitattributes")
    expect(
        "*.gti linguist-language=C++" in attributes,
        "GitHub language statistics do not classify GTI sources",
    )

    helper = text(root, "scripts/agent_workflow.py")
    expect(
        f'CANONICAL = "{CANONICAL_ROOT}.git"' in helper
        and f'MIRROR = "{MIRROR_ROOT}.git"' in helper
        and 'MIRROR_REMOTE = "gitea"' in helper
        and "api.github.com" in helper,
        "the workflow helper does not integrate on GitHub and mirror to Gitea",
    )

    ci = text(root, ".github/workflows/ci.yml")
    expect(
        "GTI_HEAD_SHA: ${{ github.sha }}" in ci
        and "gitea." not in ci
        and "GITEA_" not in ci
        and "npm run test:source-text" in ci
        and "--target gti_docgen" in ci
        and "cmp build/documentation-index.json" in ci
        and "working-directory: reference" not in ci
        and "pnpm" not in ci,
        "CI lost GitHub revision, Tree-sitter, or compiler-documentation authority",
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
    release = text(root, ".github/workflows/release.yml")
    expect(
        not (root / ".github/workflows/nightly.yml").exists(),
        "nightly and stable must reuse one bundled build, not duplicate workflows",
    )
    expect(
        "runner: macos-15" in release
        and "platform: darwin-arm64" in release
        and "platform: linux-x64" in release,
        "release packaging lost its Linux or Apple Silicon platform",
    )
    nightly = release.split("  publish-nightly:\n", 1)[-1]
    expect(
        "branches:\n      - main" in release
        and "git push --force origin refs/tags/nightly" in nightly
        and "admit-nightly" in nightly
        and "group: gti-nightly-publication" in nightly,
        "nightly is not driven from authoritative GitHub main",
    )
    upload = nightly.find("gh release upload nightly")
    prune = nightly.find("python3 scripts/sync_release_assets.py")
    expect(
        0 <= upload < prune,
        "nightly must upload current assets before pruning stale assets",
    )

    expect(
        "branches:\n      - main" in release
        and "scripts/resolve_release_source.py source" in release
        and "if: needs.metadata.outputs.stable == 'true'" in release
        and "gh release create \"${GTI_RELEASE_TAG}\"" in release
        and "managed_thread_runtime_test.py" in release
        and "atomic_runtime_test.py" in release,
        "stable release is not owned by VERSION changes on GitHub main",
    )
    expect(
        "-DCMAKE_BUILD_TYPE=Debug" not in ci
        and "-DCMAKE_BUILD_TYPE=RelWithDebInfo" in ci
        and '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g"' in ci,
        "compiler sanitizer CI must test an optimized compiler with symbols",
    )
    for workflow, contents in (
        ("ci.yml", ci),
        ("release.yml", release),
    ):
        expect(
            "bash scripts/configure_ci_resources.sh" in contents
            and "vars.GTI_TEST_JOBS" in contents
            and "_NPROCESSORS_ONLN" not in contents
            and "ctest" in contents
            and '--parallel "${GTI_TEST_JOBS}"' in contents,
            f"workflow lost explicit compile/test resource budgets: {workflow}",
        )
        expect(
            "runs-on: ubuntu-latest" in contents and "ubuntu-24.04" not in contents,
            f"workflow lost its floating Ubuntu runner label: {workflow}",
        )
        expect(
            "scripts/check_ci_sanitizers.py --sanitizer address,undefined" in contents
            and "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF" in contents
            and "--sanitizer thread" not in contents,
            f"workflow lost required ASan/UBSan preflight or explicit TSAN "
            f"capability policy: {workflow}",
        )
    mirror = text(root, ".github/workflows/mirror.yml")
    expect(
        "GITEA_MIRROR_TOKEN" in mirror and f"{MIRROR_ROOT}.git" in mirror
        and "refs/heads/main:refs/heads/main" in mirror,
        "the Gitea backup mirror workflow lost its destination or secret",
    )
    for workflow in WORKFLOWS:
        contents = text(root, workflow)
        actions = ACTION_USE.findall(contents)
        expect(actions, f"GitHub workflow has no auditable action uses: {workflow}")
        for action, revision in actions:
            expect(
                action.startswith(FIRST_PARTY_ACTIONS),
                f"GitHub workflow uses a third-party action: {workflow}: {action}",
            )
            expect(
                re.fullmatch(r"[0-9a-f]{40}", revision) is not None,
                f"GitHub workflow action is not commit-pinned: "
                f"{workflow}: {action}@{revision}",
            )
        expect(
            "gitea.server_url" not in contents and "${{ gitea." not in contents,
            f"GitHub workflow still reads Gitea context: {workflow}",
        )

    version_check = text(root, "scripts/check_release_version.py")
    expect(
        '".github/workflows/release.yml"' in version_check and ".gitea/" not in version_check,
        "release-version policy does not classify the GitHub release workflow",
    )
    for skill in (
        ".codex/skills/finish-release/SKILL.md",
        ".claude/skills/finish-release/SKILL.md",
    ):
        contents = text(root, skill)
        expect(
            "canonical GitHub" in contents and "GitHub release" in contents,
            f"delivery instructions are not GitHub-authoritative: {skill}",
        )


if __name__ == "__main__":
    main()
