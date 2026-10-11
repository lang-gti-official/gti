# GTI on GitHub

The repository at `https://github.com/lang-gti-official/gti` is GTI's
canonical Git host. GitHub owns branches, pull requests, issues, CI, tags,
nightly builds, stable releases, and release assets. The self-hosted Gitea
instance at `https://git.internal.howetotech.dev/gti-lang/gti` is a backup
mirror of `main` and the tags; it hosts no task branches, pull requests or
issues. The workflows here do not depend on the mirror.

## Repository setup

1. Branch protection for `main` is installed by the workflow helper:
   `python3 scripts/agent_workflow.py protection --configure`. It requires a
   pull request and an up-to-date branch, binds administrators to the same
   rules, and denies force pushes and deletion. Existing required reviews and
   status checks are preserved. Changing those requirements is a separate
   decision.
2. Optionally set `GTI_JOBS` and `GTI_TEST_JOBS` to positive integers in
   **Settings -> Secrets and variables -> Actions -> Variables**. By default
   both use the job's available processor count (`nproc`, not `nproc --all`,
   with macOS/POSIX fallbacks and a one-worker detection fallback), capped by
   the memory each workflow states per compile job and per CTest worker.
   Jobs install `ccache` and persist only compiler objects, separately for
   native, sanitizer, and bundled-release profiles. Cache failures never skip
   build or verification gates.
3. Optionally add the `GITEA_MIRROR_TOKEN` repository secret (a Gitea access
   token with write access to `gti-lang/gti`) so `mirror.yml` pushes `main`
   and the tags to the backup after every push. Without it, or when the Gitea
   host is unreachable from GitHub's runners, that job reports the fact and
   succeeds; the workflow helper's `sync` still mirrors from the developer
   machine after each integration.
4. The `GITHUB_TOKEN` the publication jobs use needs no configuration; they
   request `contents: write` explicitly and ordinary CI requests read access
   only.

The runners are GitHub-hosted `ubuntu-latest` (Ubuntu 24.04, so `llvm-18-dev`
is available) and `macos-15` (Apple Silicon). Jobs install CMake 3.20 or
newer, Ninja, a C++20 compiler, and supported LLVM packages where needed.
Release builds download GTI's pinned upstream LLVM source and therefore need
outbound access to that upstream release host.

GitHub-hosted runners execute a small, race-free ASan/UBSan probe before
their expensive builds. ThreadSanitizer is a separate runner capability:
`GTI_ENABLE_THREAD_SANITIZER_TESTS` defaults to `ON` for local and compatible
dedicated runners; these workflows set it to `OFF`. Mixed runtime matrices
still execute their ordinary and ASan/UBSan modes, while dedicated TSAN CTests
report an explicit skip (return code 77) rather than a false pass. A dedicated
TSAN runner must first pass `scripts/check_ci_sanitizers.py --sanitizer
thread` in the exact job environment, then configure without disabling the
option.

## Workflow authority

- `ci.yml` runs for every push to `main`, every pull request, and manual
  dispatch. It is the canonical remote CI signal. The formatting and
  release-version checks gate the native toolchain job; the sanitizer job
  follows the native job so the two never share one runner's budget.
- `release.yml` runs for every `main` commit or by manual dispatch. One bundled
  build and validation suite per platform (`linux-x64` on `ubuntu-latest`,
  `darwin-arm64` on `macos-15`) supplies both channels. Nightly always gets its
  archives; stable gets archives only on a `VERSION` change or exact-commit
  manual rebuild. Stable is archived before the nightly identity stamp. Stable
  publication rejects a conflicting immutable `v<VERSION>` tag. Serialized
  nightly publication skips superseded revisions, then advances the mutable
  `nightly` tag and GitHub release. Current assets are uploaded before assets
  outside the new build are pruned by `scripts/sync_release_assets.py`.
- `mirror.yml` pushes `main` and the tags to the Gitea backup when its secret
  is configured and the host is reachable.

Both publication jobs use the repository's workflow token and publish only
after the build-tree and installed-toolchain gates pass. A successful push
merely starts that asynchronous work; local validation remains the pre-push
authority. Release archives are downloaded by the Neovim plugin from
`https://github.com/lang-gti-official/gti/releases/download/<tag>/<asset>`.

## History

The project moved from GitHub to a self-hosted Gitea instance and back. The
Gitea workflows are archived under `docs/plans/archive/gitea-actions/` and the
first GitHub workflows under `docs/plans/archive/github-actions/`. Gitea
remains the mirror; its pull requests and issues were not migrated, and the
old personal GitHub repository is not a remote of this project.
