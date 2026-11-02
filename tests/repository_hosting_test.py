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
NIGHTLY = ".github/workflows/nightly.yml"
PREVIEW = ".github/workflows/preview.yml"
ROLLUP = ".github/workflows/rollup.yml"
CACHE = ".github/workflows/cache.yml"
TRAIN = ".github/workflows/train.yml"
BETA = ".github/workflows/beta.yml"
BACKPORT = ".github/workflows/backport.yml"
ROLLBACK = ".github/workflows/rollback.yml"
RELEASE_TRAIN = (TRAIN, BETA, BACKPORT, ROLLBACK)
ROLLUP_SECRETS = ("GTI_ROLLUP_APP_ID", "GTI_ROLLUP_APP_PRIVATE_KEY")
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
    # CI, nightly toolchain releases and the rollup bot run on GitHub. The frozen
    # Gitea release workflow stays as the record of the contract it replaces.
    expect(set(workflows) == {CI, NIGHTLY, PREVIEW, RELEASE, ROLLUP, CACHE, *RELEASE_TRAIN},
           f"unexpected live workflows: {workflows}")
    for relative in (CI, NIGHTLY, PREVIEW, RELEASE, ROLLUP, CACHE, *RELEASE_TRAIN, ".github/README.md", ".gitea/README.md",
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
    expect('local release_server = "https://github.com"' in installer
           and 'local repository = "lang-gti-official/gti"' in installer
           and PACKAGE_SERVER not in installer and "gti-lang" not in installer,
           "the installer downloads toolchain releases from GitHub Releases, never Gitea")
    expect('vim.g.gti_channel = \\"nightly\\"' in installer and "stable_unavailable" in installer,
           "an unpublished stable release must fail clearly and name the nightly channel")
    expect('"https://raw.githubusercontent.com/%s/release-status/release-status.json"' in installer
           and "check_not_yanked" in installer and PACKAGE_SERVER not in installer,
           "the stable installer reads the yank list from GitHub's release-status branch")
    expect("*.gti linguist-language=C++" in text(root, ".gitattributes"),
           "GTI source language classification must remain intact")
    helper = text(root, "scripts/agent_workflow.py")
    expect(f'CANONICAL = "{CANONICAL_ROOT}.git"' in helper and "class GitHub:" in helper
           and "https://api.github.com/repos/" in helper and 'def rehost(' in helper
           and "enqueuePullRequest" in helper and "/api/v1/repos/" not in helper
           and "GITEA_TOKEN" not in helper and "MIRROR_REMOTE" not in helper,
           "lifecycle must use GitHub, the merge queue and explicit rehosting, never Gitea credentials")

    preview = text(root, PREVIEW)
    expect("branches: [preview]" in preview and "preview_release.py check" in preview
           and "preview_admission" in preview and "needs: [select, package]" in preview
           and preview.count("contents: write") == 1,
           "preview must retain combined admission, exact package checks and isolated publication")
    ci, nightly, release = text(root, CI), text(root, NIGHTLY), text(root, RELEASE)
    expect("merge_group:" in ci and "ready_for_review" in ci and "name: CI gate" in ci
           and "GTI_EVENT_PATH: ${{ github.event_path }}" in ci
           and '--event "${GTI_EVENT_PATH}"' in ci
           and "python3 tests/repository_hosting_test.py ." in ci
           and "npm run test:source-text" in ci and "--target gti_docgen" in ci
           and "cmp build/documentation-index.json" in ci
           and "validation_checks.py shard" in ci and "--shard-reports" in ci
           and "installed_library_smoke" in ci
           and ci.count("validation_checks.py declared --event") == 2,
           "CI lost its gate, merge-queue suite, Tree-sitter or compiler-owned documentation checks")
    declared_steps = [step for step in re.split(r"^      - ", ci, flags=re.MULTILINE)
                      if "validation_checks.py declared --event" in step]
    expect(len(declared_steps) == 2 and all(
        "if: github.event_name == 'pull_request'" in step for step in declared_steps),
        "PR declarations must only run on pull_request; manual CI has no PR body")
    # Every native job takes the one reviewed toolchain; none installs its own.
    expect(ci.count("bash scripts/configure_ci_toolchain.sh") == 4 and "llvm-18" not in ci
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
           "the frozen Gitea workflow must stay the Linux-only record it was")
    expect("macos-26, platform: darwin-arm64" in ci and "ubuntu-24.04, platform: linux-x64" in ci
           and "ubuntu-24.04-arm" not in ci and "configure_ci_toolchain.sh" not in nightly
           and "release_artifacts.py verify" in nightly,
           "integration produces reviewed packages; nightly only verifies and publishes them")
    expect("pull_request_target" not in nightly and "\npermissions:\n  contents: read\n" in nightly
           and set(re.findall(r"secrets\.([A-Za-z0-9_]+)", nightly)) <= {"GITHUB_TOKEN"}
           and nightly.count("contents: write") == 1,
           "nightly must default to a read-only token, use only GITHUB_TOKEN and write only when publishing")
    expect("--force-with-lease=refs/tags/nightly:" in nightly and "push --force " not in nightly,
           "the nightly tag moves only with an explicit lease")
    policy = text(root, "docs/architecture/build-and-driver.md")
    expect("### GitHub nightly publication" in policy and "host-runtime.json" in policy
           and "linux-aarch64" in policy and "### GitHub release train" in policy
           and "beta/stable release train" not in policy and "release-status" in policy
           and "Manual macOS release publication" not in policy,
           "GitHub nightly publication, the release train and the linux-arm64 gap must be explicit")
    check_release_train(root, ci)
    beta, backport, cache = text(root, BETA), text(root, BACKPORT), text(root, CACHE)
    # The main-branch cache warm-up only helps if it compiles exactly what CI
    # compiles: every configure command it runs must appear verbatim in ci.yml.
    warmups = re.findall(r"\n        run: >-\n((?:          [^\n]*\n)+)", cache)
    expect(len(warmups) == 2 and all(block in ci for block in warmups)
           and "push:\n    branches: [main]" in cache and "secrets." not in cache
           and "pull_request" not in cache,
           "the compiler-cache warm-up must run on main with ci.yml's exact configure commands")
    for workflow, contents in ((CI, ci), (NIGHTLY, nightly), (RELEASE, release), (BETA, beta),
                               (BACKPORT, backport), (CACHE, cache)):
        parallel = '--jobs "${GTI_TEST_JOBS}"' if workflow == BACKPORT else (
            '--parallel "${GTI_JOBS}"' if workflow == CACHE else '--parallel "${GTI_TEST_JOBS}"')
        if workflow != NIGHTLY:  # Publication does not compile or run CTest.
            expect("bash scripts/configure_ci_resources.sh" in contents and "vars.GTI_TEST_JOBS" in contents
                   and parallel in contents and "_NPROCESSORS_ONLN" not in contents,
                   f"resource budgets lost: {workflow}")
            expect("-DGTI_ENABLE_THREAD_SANITIZER_TESTS=OFF" in contents,
                   f"sanitizer capability policy lost: {workflow}")
        actions = ACTION_USE.findall(contents)
        expect(actions, f"workflow has no auditable actions: {workflow}")
        trusted = "https://gitea.com/actions/" if workflow == RELEASE else "actions/"
        for action, revision in actions:
            expect(action.startswith(trusted) and re.fullmatch(r"[0-9a-f]{40}", revision),
                   f"untrusted/unpinned action: {workflow}: {action}@{revision}")
    expect("gh release" not in release and "GH_TOKEN" not in release
           and "api.github.com" not in release and "${{ github." not in release,
           "the frozen Gitea workflow must not be half-converted; GitHub releases get their own workflow")
    expect("gti-lang" not in nightly and "howetotech" not in nightly and "gitea" not in nightly.lower(),
           "GitHub nightly publication must not reach Gitea")
    release_policy = text(root, "scripts/check_release_version.py")
    expect(f'"{RELEASE}"' in release_policy and f'"{NIGHTLY}"' in release_policy
           and f'"{BETA}"' in release_policy,
           "release policy must classify its package workflows")
    expect("gti-lang/gti-reference" in text(root, "docs/architecture/reference-documentation.md"),
           "the reference website stays on Gitea")
    check_rollup(root, ci, text(root, ROLLUP), workflows)
    for skill in (".codex/skills/finish-release/SKILL.md", ".claude/skills/finish-release/SKILL.md"):
        contents = text(root, skill)
        expect("GitHub Releases" in contents and "package registry" in contents,
               f"delivery instructions must send toolchain releases to GitHub and keep the registry on Gitea: {skill}")


def check_release_train(root, ci):
    """The release train publishes to GitHub only, with the App token only where a PR opens."""
    checkout = re.search(r"uses: (actions/checkout@[0-9a-f]{40})", ci).group(1)
    for workflow in RELEASE_TRAIN:
        contents = text(root, workflow)
        actions = ACTION_USE.findall(contents)
        expect(actions, f"workflow has no auditable actions: {workflow}")
        for action, revision in actions:
            expect(action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision),
                   f"untrusted/unpinned action: {workflow}: {action}@{revision}")
            if action == "actions/checkout":
                expect(f"{action}@{revision}" == checkout, f"{workflow} must pin the checkout revision CI uses")
        expect("pull_request_target" not in contents and "\npermissions:\n  contents: read\n" in contents,
               f"{workflow} must default to a read-only token and never run pull_request_target")
        expect("gti-lang" not in contents and "howetotech" not in contents and "gitea" not in contents.lower(),
               f"the release train must not reach Gitea: {workflow}")
        expect("--force" not in contents and "--clobber" not in contents and "delete-asset" not in contents,
               f"the release train never forces a ref or replaces a published asset: {workflow}")
        secrets = set(re.findall(r"secrets\.([A-Za-z0-9_]+)", contents))
        expect(secrets == (set(ROLLUP_SECRETS) if workflow == TRAIN else set()),
               f"unexpected secrets in {workflow}: {sorted(secrets)}")
    cut = text(root, TRAIN).split("\n  cut:\n", 1)[1].split("\n  promote:\n", 1)[0]
    expect("secrets.GTI_ROLLUP_APP_ID" in cut and re.findall(r"GH_TOKEN: (.*)", cut)
           == ["${{ steps.token.outputs.token }}"] and "repositories: gti" in cut,
           "the release cut opens its PR only with the repository-scoped App token")
    expect("environment: stable" in text(root, TRAIN).split("\n  promote:\n", 1)[1]
           and "environment: stable" in text(root, ROLLBACK),
           "promotion and rollback run only behind the protected stable environment")
    readme = text(root, ".github/README.md")
    for needle in ("## Release train", "`stable` environment", "release immutability",
                   "release-blocker", "release_train.py rulesets", "beta/*", "release-status"):
        expect(needle in readme, f".github/README.md must document the release train owner setup: {needle}")


def check_rollup(root, ci, rollup, workflows):
    """The rollup bot holds the only secrets and never runs pull-request content."""
    checkout = re.search(r"uses: (actions/checkout@[0-9a-f]{40})", ci).group(1)
    actions = ACTION_USE.findall(rollup)
    expect(actions, "rollup workflow has no auditable actions")
    for action, revision in actions:
        expect(action.startswith("actions/") and re.fullmatch(r"[0-9a-f]{40}", revision),
               f"untrusted/unpinned action: {ROLLUP}: {action}@{revision}")
        if action == "actions/checkout":
            expect(f"{action}@{revision}" == checkout, "rollup must pin the checkout revision CI uses")
    expect("actions/create-github-app-token" in {action for action, _ in actions},
           "rollup must mint a short-lived App installation token")
    expect(set(re.findall(r"secrets\.([A-Za-z0-9_]+)", rollup)) == set(ROLLUP_SECRETS),
           "rollup may reference only its App secrets")
    writer = rollup.split("\n  rollup:\n", 1)[1]
    expect(re.findall(r"GH_TOKEN: (.*)", writer) == ["${{ steps.token.outputs.token }}"]
           and "secrets.GITHUB_TOKEN" not in rollup and "repositories: gti" in rollup,
           "rollup writes only with its repository-scoped App token, never GITHUB_TOKEN")
    expect(all(name in rollup.split("Require the rollup GitHub App secrets", 1)[1].split("uses:", 1)[0]
               for name in (*ROLLUP_SECRETS, ".github/README.md")),
           "a missing App secret must fail clearly, naming both secrets and the setup section")
    for workflow in workflows:
        if workflow not in (ROLLUP, TRAIN):
            expect(not any(name in text(root, workflow) for name in ROLLUP_SECRETS),
                   f"only the rollup bot and the release cut may reference the App secrets: {workflow}")
    triggers = rollup.split("\npermissions:", 1)[0]
    expect("pull_request" not in triggers and "pull_request_target" not in rollup
           and "workflow_run" not in triggers and "permissions: {}" in rollup
           and "cancel-in-progress: false" in rollup,
           "rollup must not run on pull-request events, and runs one at a time without cancellation")
    steps = rollup.split("uses: actions/checkout@")[1:]
    expect(steps and all(re.match(r"[0-9a-f]{40} # [^\n]*\n        with:\n(?:          [^\n]*\n)*?"
                                  r"          ref: main\n", step) for step in steps)
           and rollup.count("persist-credentials: false") == len(steps),
           "rollup must check out only main, without persisted credentials")
    expect(not re.search(r"refs/pull|head_ref|github\.event\.(?:issue|pull_request|comment)\.[a-z_.]*(?:head|sha|ref)", rollup),
           "rollup must never check out or reference pull-request heads")


if __name__ == "__main__":
    main()
