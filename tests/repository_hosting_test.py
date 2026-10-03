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

    ci = text(root, ".gitea/workflows/ci.yml")
    expect(
        "GTI_HEAD_SHA: ${{ gitea.sha }}" in ci
        and "GITHUB_SHA" not in ci
        and "npm run test:source-text" in ci
        and "pnpm examples:contract-smoke" in ci,
        "CI lost Gitea revision, Tree-sitter, or reference-site authority",
    )
    nightly = text(root, ".gitea/workflows/nightly.yml")
    expect(
        "branches:\n      - main" in nightly
        and "git push --force origin refs/tags/nightly" in nightly
        and "managed_thread_runtime_test.py" in nightly
        and "atomic_runtime_test.py" in nightly,
        "nightly is not driven from authoritative Gitea main",
    )
    upload = nightly.find("uses: https://gitea.com/actions/gitea-release-action@")
    prune = nightly.find("python3 scripts/sync_gitea_release_assets.py")
    expect(
        0 <= upload < prune,
        "nightly must upload current assets before pruning stale assets",
    )

    release = text(root, ".gitea/workflows/release.yml")
    expect(
        "branches:\n      - main" in release
        and "paths:\n      - VERSION" in release
        and "tag_name: ${{ needs.metadata.outputs.tag }}" in release
        and "managed_thread_runtime_test.py" in release
        and "atomic_runtime_test.py" in release,
        "stable release is not owned by VERSION changes on Gitea main",
    )
    expect(
        "-DCMAKE_BUILD_TYPE=Debug" not in ci
        and "-DCMAKE_BUILD_TYPE=RelWithDebInfo" in ci
        and '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g"' in ci,
        "compiler sanitizer CI must test an optimized compiler with symbols",
    )
    for workflow, contents in (
        ("ci.yml", ci),
        ("nightly.yml", nightly),
        ("release.yml", release),
    ):
        expect(
            "bash scripts/configure_ci_resources.sh" in contents
            and "vars.GTI_TEST_JOBS" in contents
            and "_NPROCESSORS_ONLN" not in contents
            and 'ctest' in contents
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
        actions = ACTION_USE.findall(contents)
        expect(actions, f"Gitea workflow has no auditable action uses: {workflow}")
        for action, revision in actions:
            expect(
                action.startswith("https://gitea.com/"),
                f"Gitea workflow action is not fully qualified: {workflow}: {action}",
            )
            expect(
                re.fullmatch(r"[0-9a-f]{40}", revision) is not None,
                f"Gitea workflow action is not commit-pinned: "
                f"{workflow}: {action}@{revision}",
            )
    expect(
        (root / ".gitea" / "ISSUE_TEMPLATE" / "bug_report.md").is_file(),
        "Gitea issue template is missing",
    )

    version_check = text(root, "scripts/check_release_version.py")
    expect(
        '".gitea/workflows/release.yml"' in version_check,
        "release-version policy does not classify the Gitea release workflow",
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
