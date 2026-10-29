#!/usr/bin/env python3
"""Fail closed on the hosting split: GitHub owns source, CI and toolchain releases;
Gitea keeps the reference website and the `gti add` package registry."""

import pathlib
import re
import subprocess
import sys


CANONICAL_ROOT = "https://github.com/lang-gti-official/gti"
PACKAGE_SERVER = "https://git.internal.howetotech.dev"
HISTORICAL_EXCEPTIONS = {"docs/plans/compiler-structural-baseline.json", "CHANGELOG.md",
                         "docs/plans/implementation-sequence.md"}
HISTORICAL_PREFIXES = ("docs/plans/archive/",)
# The frozen Gitea source repository must never again be a clone, push, Lazy or
# API default. Its release downloads and historical issue links stay valid.
RETIRED_SOURCE = PACKAGE_SERVER + "/gti-lang/gti.git"
FORBIDDEN = (
    RETIRED_SOURCE,
    "https://git2." + "internal.howetotech.dev",
    "https://" + "github.com/LukeHHA/" + "gti",
    "git@" + "github.com:LukeHHA/" + "gti",
    PACKAGE_SERVER + "/api/v1/repos/gti-lang/gti/pulls",
)
ACTION_USE = re.compile(r"^\s*(?:-\s+)?uses:\s*(\S+)@([^\s#]+)", re.MULTILINE)
CI = ".github/workflows/ci.yml"
RELEASE = ".gitea/workflows/release.yml"


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
    workflows = sorted(p for p in tracked if p.startswith((".github/workflows/", ".gitea/workflows/")))
    # CI runs on GitHub. The frozen Gitea release workflow stays until the GitHub
    # release workflow replaces it.
    expect(set(workflows) == {CI, RELEASE}, f"unexpected live workflows: {workflows}")
    for relative in (CI, RELEASE, ".github/README.md", ".gitea/README.md",
                     ".gitea/ISSUE_TEMPLATE/bug_report.md",
                     "docs/plans/archive/github-actions-20261024/README.md"):
        text(root, relative)
    violations = []
    for relative in tracked:
        if relative in HISTORICAL_EXCEPTIONS or relative.startswith(HISTORICAL_PREFIXES):
            continue
        if relative == "tests/repository_hosting_test.py":
            continue
        try:
            contents = (root / relative).read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        found = [needle for needle in FORBIDDEN if needle in contents]
        if found:
            violations.append(f"{relative}: {', '.join(found)}")
    expect(not violations, "retired hosting defaults remain:\n" + "\n".join(violations))

    readme, lazy = text(root, "README.md"), text(root, "lazy.lua")
    expect(f"git clone {CANONICAL_ROOT}.git gti" in readme, "clone command is not canonical")
    expect(f'"{CANONICAL_ROOT}.git"' in readme and f'"{CANONICAL_ROOT}.git"' in lazy,
           "Lazy installation must use the complete canonical Git URL")
    expect(f'CANONICAL = "{CANONICAL_ROOT}.git"' in text(root, "scripts/install_neovim_local.py"),
           "the local installer must resolve preview source from canonical GitHub")
    installer = text(root, "lua/gti/installer.lua")
    expect(f'local release_server = "{PACKAGE_SERVER}"' in installer
           and 'local repository = "gti-lang/gti"' in installer,
           "the installer keeps the existing Gitea archives until the GitHub release switch lands")
    expect("*.gti linguist-language=C++" in text(root, ".gitattributes"),
           "GTI source language classification must remain intact")
    helper = text(root, "scripts/agent_workflow.py")
    expect(f'CANONICAL = "{CANONICAL_ROOT}.git"' in helper and "class GitHub:" in helper
           and "https://api.github.com/repos/" in helper and 'def rehost(' in helper
           and "enqueuePullRequest" in helper and "/api/v1/repos/" not in helper
           and "GITEA_TOKEN" not in helper and "MIRROR_REMOTE" not in helper,
           "lifecycle must use GitHub, the merge queue and explicit rehosting, never Gitea credentials")

    ci, release = text(root, CI), text(root, RELEASE)
    expect("merge_group:" in ci and "ready_for_review" in ci and "name: CI gate" in ci
           and "GTI_EVENT_PATH: ${{ github.event_path }}" in ci
           and '--event "${GTI_EVENT_PATH}"' in ci
           and "python3 tests/repository_hosting_test.py ." in ci
           and "npm run test:source-text" in ci and "--target gti_docgen" in ci
           and "cmp build/documentation-index.json" in ci
           and "validation_checks.py shard" in ci and "--shard-reports" in ci
           and "installed_library_smoke" in ci,
           "CI lost its gate, merge-queue suite, Tree-sitter or compiler-owned documentation checks")
    # Every native job takes the one reviewed toolchain; none installs its own.
    expect(ci.count("bash scripts/configure_ci_toolchain.sh") == 3 and "llvm-18" not in ci
           and "brew install" not in ci and "macos-26" in ci and "macos-15" not in ci,
           "native CI jobs must use the reviewed compiler from configure_ci_toolchain.sh")
    expect("-DCMAKE_BUILD_TYPE=Debug" not in ci and "-DCMAKE_BUILD_TYPE=RelWithDebInfo" in ci
           and '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O1 -g"' in ci
           and "check_ci_sanitizers.py" in ci and "--sanitizer address,undefined" in ci,
           "sanitizer verification must retain an optimized compiler with symbols")
    expect("permissions:\n  contents: read" in ci and "pull_request_target" not in ci
           and "secrets." not in ci,
           "CI must run untrusted pull requests with a read-only token and no secrets")
    expect("macos-15" not in release and "macOS-ARM64)" not in release
           and "Linux-X64) platform=linux-x64" in release,
           "package automation must use existing Linux runners, not a nonexistent macOS runner")
    policy = text(root, "docs/architecture/build-and-driver.md")
    expect("### Manual macOS release publication" in policy and "host-runtime.json" in policy
           and "Apple Silicon" in policy and "Linux-only" in policy,
           "manual macOS publication and runtime/platform limitations must be explicit")
    for workflow, contents in ((CI, ci), (RELEASE, release)):
        expect("bash scripts/configure_ci_resources.sh" in contents and "vars.GTI_TEST_JOBS" in contents
               and '--parallel "${GTI_TEST_JOBS}"' in contents and "_NPROCESSORS_ONLN" not in contents,
               f"resource budgets lost: {workflow}")
        expect("-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF" in contents,
               f"sanitizer capability policy lost: {workflow}")
        actions = ACTION_USE.findall(contents)
        expect(actions, f"workflow has no auditable actions: {workflow}")
        trusted = "actions/" if workflow == CI else "https://gitea.com/actions/"
        for action, revision in actions:
            expect(action.startswith(trusted) and re.fullmatch(r"[0-9a-f]{40}", revision),
                   f"untrusted/unpinned action: {workflow}: {action}@{revision}")
    expect("gh release" not in release and "GH_TOKEN" not in release
           and "api.github.com" not in release and "${{ github." not in release,
           "the frozen Gitea workflow must not be half-converted; GitHub releases get their own workflow")
    expect(f'"{RELEASE}"' in text(root, "scripts/check_release_version.py"),
           "release policy must classify its live package workflow")
    expect("gti-lang/gti-reference" in text(root, "docs/architecture/reference-documentation.md"),
           "the reference website stays on Gitea")
    for skill in (".codex/skills/finish-release/SKILL.md", ".claude/skills/finish-release/SKILL.md"):
        contents = text(root, skill)
        expect("GitHub Releases" in contents and "package registry" in contents,
               f"delivery instructions must send toolchain releases to GitHub and keep the registry on Gitea: {skill}")


if __name__ == "__main__":
    main()
