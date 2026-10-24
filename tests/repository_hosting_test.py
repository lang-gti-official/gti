#!/usr/bin/env python3
"""Fail closed on retired first-party hosting while preserving explicit history."""

import pathlib
import re
import subprocess
import sys


CANONICAL_ROOT = "https://git.internal.howetotech.dev/gti-lang/gti"
HISTORICAL_EXCEPTIONS = {"docs/plans/compiler-structural-baseline.json", "CHANGELOG.md"}
HISTORICAL_PREFIXES = ("docs/plans/archive/",)
MIGRATION_RECORD_FILES = {"scripts/agent_workflow.py", "tests/agent_workflow_test.py"}
RETIRED_ROOT = "https://" + "github.com/lang-gti-official/gti"
FORBIDDEN = (
    "https://git2." + "internal.howetotech.dev",
    "https://" + "github.com/LukeHHA/" + "gti",
    "git@" + "github.com:LukeHHA/" + "gti",
    RETIRED_ROOT,
    "https://api." + "github.com/repos/lang-gti-official/gti",
)
ACTION_USE = re.compile(r"^\s*uses:\s*(\S+)@([^\s#]+)", re.MULTILINE)
WORKFLOWS = (".gitea/workflows/ci.yml", ".gitea/workflows/release.yml")


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def text(root, relative):
    path = root / relative
    expect(path.is_file(), f"required repository file is missing: {relative}")
    return path.read_text(encoding="utf-8")


def tracked_files(root):
    result = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                            cwd=root, check=True, capture_output=True)
    return [item.decode() for item in result.stdout.split(b"\0") if item
            and (root / item.decode()).is_file()]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: repository_hosting_test.py <repository-root>")
    root = pathlib.Path(sys.argv[1]).resolve()
    tracked = tracked_files(root)
    expect(not any(p.startswith(".github/workflows/") for p in tracked),
           "retired GitHub workflows must not remain executable")
    for relative in (*WORKFLOWS, ".gitea/README.md", ".gitea/ISSUE_TEMPLATE/bug_report.md",
                     "docs/plans/archive/github-actions-20261024/README.md",
                     "docs/plans/archive/github-actions-20261024/workflows/ci.yml",
                     "docs/plans/archive/github-actions-20261024/workflows/release.yml",
                     "docs/plans/archive/github-actions-20261024/workflows/mirror.yml"):
        text(root, relative)
    violations = []
    for relative in tracked:
        if relative in HISTORICAL_EXCEPTIONS or relative.startswith(HISTORICAL_PREFIXES):
            continue
        try:
            contents = (root / relative).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        # Rehosting must validate the original GitHub PR URL, not pretend it
        # was measured on Gitea. No clone/release/API defaults are exempted.
        if relative in MIGRATION_RECORD_FILES:
            contents = contents.replace(RETIRED_ROOT + "/pull/", "")
        found = [needle for needle in FORBIDDEN if needle in contents]
        if found:
            violations.append(f"{relative}: {', '.join(found)}")
    expect(not violations, "retired hosting defaults remain:\n" + "\n".join(violations))

    readme, lazy = text(root, "README.md"), text(root, "lazy.lua")
    expect(f"git clone {CANONICAL_ROOT}.git gti" in readme, "clone command is not canonical")
    expect(f'"{CANONICAL_ROOT}.git"' in readme and f'"{CANONICAL_ROOT}.git"' in lazy,
           "Lazy installation must use the complete canonical Git URL")
    installer = text(root, "lua/gti/installer.lua")
    expect('local release_server = "https://git.internal.howetotech.dev"' in installer
           and 'local repository = "gti-lang/gti"' in installer,
           "download defaults must use canonical Gitea releases")
    expect("*.gti linguist-language=C++" in text(root, ".gitattributes"),
           "GTI source language classification must remain intact")
    helper = text(root, "scripts/agent_workflow.py")
    expect(f'CANONICAL = "{CANONICAL_ROOT}.git"' in helper and "/api/v1/repos/" in helper
           and 'class Gitea:' in helper and 'def rehost(' in helper
           and 'gh", "auth"' not in helper and "MIRROR_REMOTE" not in helper,
           "lifecycle must use Gitea and explicit provenance, never GitHub authentication/backup pushes")

    ci, release = (text(root, p) for p in WORKFLOWS)
    expect("GTI_HEAD_SHA: ${{ gitea.sha }}" in ci and "GITEA_EVENT_PATH" in ci
           and "npm run test:source-text" in ci and "--target gti_docgen" in ci
           and "cmp build/documentation-index.json" in ci,
           "CI lost source, Tree-sitter or compiler-owned documentation checks")
    expect("edited" in ci and "ready_for_review" not in ci,
           "Gitea WIP-title transitions must use supported edited events")
    expect(not any(p.startswith("reference/") for p in tracked)
           and (root / "src/docgen/main.cpp").is_file(),
           "the standalone reference site must not displace compiler-owned docgen")
    expect("gti-lang/gti-reference" in text(root, "docs/architecture/reference-documentation.md"),
           "standalone reference ownership must remain documented")
    expect("macos-15" not in release and "macOS-ARM64)" not in release
           and "Linux-X64) platform=linux-x64" in release,
           "release automation must use existing Linux runners, not queue a nonexistent macOS runner")
    policy = text(root, "docs/architecture/build-and-driver.md")
    expect("### Manual macOS release publication" in policy and "host-runtime.json" in policy
           and "Apple Silicon" in policy and "Linux-only" in policy,
           "manual macOS publication and runtime/platform limitations must be explicit")
    expect("-DCMAKE_BUILD_TYPE=Debug" not in ci and "-DCMAKE_BUILD_TYPE=RelWithDebInfo" in ci
           and '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g"' in ci,
           "sanitizer verification must retain an optimized compiler with symbols")
    for workflow, contents in zip(WORKFLOWS, (ci, release)):
        expect("bash scripts/configure_ci_resources.sh" in contents and "vars.GTI_TEST_JOBS" in contents
               and '--parallel "${GTI_TEST_JOBS}"' in contents and "_NPROCESSORS_ONLN" not in contents,
               f"resource budgets lost: {workflow}")
        expect("scripts/check_ci_sanitizers.py --sanitizer address,undefined" in contents
               and "-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF" in contents,
               f"sanitizer capability policy lost: {workflow}")
        expect("gh release" not in contents and "GH_TOKEN" not in contents
               and "api.github.com" not in contents and "${{ github." not in contents,
               f"GitHub publication/context remains live: {workflow}")
        actions = ACTION_USE.findall(contents)
        expect(actions, f"workflow has no auditable actions: {workflow}")
        for action, revision in actions:
            expect(action.startswith("https://gitea.com/actions/")
                   and re.fullmatch(r"[0-9a-f]{40}", revision),
                   f"untrusted/unpinned action: {workflow}: {action}@{revision}")
    expect('".gitea/workflows/release.yml"' in text(root, "scripts/check_release_version.py"),
           "release policy must classify its live workflow")
    for skill in (".codex/skills/finish-release/SKILL.md", ".claude/skills/finish-release/SKILL.md"):
        contents = text(root, skill)
        expect("canonical Gitea" in contents and "Gitea release" in contents,
               f"delivery instructions are not Gitea-authoritative: {skill}")


if __name__ == "__main__":
    main()
