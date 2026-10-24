# GTI on Gitea

`https://git.internal.howetotech.dev/gti-lang/gti` is canonical for code,
branches, issues, pull requests, Actions, tags and release assets. `origin`
points there. The former Gitea tracker remains read-only in
`gti-lang/gti-legacy-gitea-20261024`; it is not the imported GitHub tracker.
GitHub workflows are retired and no automatic backup push is configured.

## Repository setup

Use `python3 scripts/agent_workflow.py protection --configure --advisory-ci`
for the owner-authorized PR-only, up-to-date, no-direct/force-push and
no-admin-override policy. Preserve existing review requirements. CI is
advisory: never wait for Actions to merge or finish locally validated work.
Exact-source local/managed-runner validation and platform gates remain required.

The helper reads `GTI_GITEA_TOKEN` or `GITEA_TOKEN`, including from the primary
checkout's ignored `.env`, and never copies credentials into task worktrees.
Imported GitHub task records need explicit `rehost <task> --pr <number>` after
the mapping and source identities are checked and their live validation ends.

Existing Linux runners advertise `ubuntu-latest`. No macOS runner service is
configured. Jobs install required CMake, Ninja, C++ and supported LLVM packages;
bundled release builds also need access to the pinned upstream LLVM source.
Set positive `GTI_JOBS` and `GTI_TEST_JOBS` repository Actions variables to
override the explicit CPU/memory-based budgets. Compiler-object caches are
separate for native, sanitizer and bundled-release profiles; cache errors never
skip builds or validation. Gitea-compatible actions are commit-pinned, including
the cache-service-v1 and artifact implementations. Do not substitute GitHub-only
artifact/cache protocols merely because their action names look similar.

Actions CI uses the repository-scoped read-only token; publishing jobs request
`releases: write` and `contents: write` explicitly where needed. No PAT or
GitHub mirror secret is required for publication. Tokens never follow arbitrary
redirects in the release synchronization helper.

## Workflows and platforms

`ci.yml` preserves risk-based routing: drafts/components get inexpensive checks,
ready independent compiler candidates and main also get native/sanitizer
feedback. Gitea WIP-title changes use the `edited` event. Identified body-only
edits do not trigger another full compiler campaign or claim source acceptance.
Unknown edits remain conservative. ASan/UBSan preflight and optimized compilers
are required; dedicated TSAN remains an explicit platform capability, not a
false pass. The sanitizer job follows the native job to avoid overlapping its
resource budget.

`release.yml` shares one bundled Linux build, full CTest and installed-toolchain
validation between nightly and stable. Stable publication requires a VERSION
change or an exact-source manual dispatch and refuses conflicting immutable
tags. Nightly is serialized and skips superseded main commits; upload precedes
exact asset pruning, and pruning refuses an incomplete upload inventory.

Apple Silicon stable releases are manually built, validated, packaged and
uploaded to the matching Gitea release. Follow the
[manual macOS contract](../docs/architecture/build-and-driver.md#manual-macos-release-publication).
No macOS nightly archive or automatic platform completion is promised. Do not
install a runner service without separate authorization. Source/local macOS
installation remains available independently of release assets.

## Historical automation

Previous configurations under `docs/plans/archive/` are historical evidence,
not live policy. GitHub Actions was disabled during the 2026-10-24 cutover.
The owner, not these workflows, controls eventual GitHub deletion and any
future private backup repository.
