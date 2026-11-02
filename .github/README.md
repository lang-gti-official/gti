# GTI on GitHub

https://github.com/lang-gti-official/gti is canonical for source, branches,
pull requests, issues, CI and toolchain releases. The reference website and the
`gti add` package registry stay on Gitea; see [Gitea](../.gitea/README.md).

## Integration

Agents can push branches and open draft pull requests before CI passes. Use
`agent_workflow.py start` to publish the task branch and `checkpoint` after a
meaningful change to create or update its draft PR. Run focused validation
before `finish`; only integration into `main` waits for the required checks
and merge queue. Nightly, beta and stable publication are separate steps and
do not block contributors from submitting their work.

`main` changes only through the merge queue. The `main` ruleset requires pull
requests, the merge queue and the `CI gate` status check, denies force pushes
and deletion, and has no bypass actors. Configure or audit it with:

```sh
python3 scripts/agent_workflow.py protection --configure
python3 scripts/agent_workflow.py protection
```

`workflows/ci.yml` reports the single required check, `CI gate`:

- On a pull request: lint (formatting, release version, Tree-sitter), the
  workflow regression tests, and a macOS arm64 and Linux x64 build running
  `feedback-fast` and `compiler_pipeline`. Drafts run lint only.
- On manual dispatch from main: full integration and production packaging,
  without PR-body declarations, for release commissioning or artifact recovery.
- In the merge queue: system-LLVM fast checks plus production packaging and
  installed-consumer checks on both platforms, and the complete suite on the
  exact candidate in its bundled-release configuration, split into
  cost-balanced shards (`scripts/validation_checks.py shard`, balanced by
  `tests/ci_test_durations.json`) on both platforms, the ASan/UBSan subset, the
  installed-toolchain smoke and deterministic docgen. The gate merges the shard
  reports and fails unless each platform ran its whole inventory exactly once.
- Explicitly allowlisted orchestration-only PRs and merge groups run the
  inexpensive lint and workflow checks. They produce no release packages;
  publication still requires full integration evidence for its exact source.

Sanitizer jobs run the compiler aggregate separately from the other library
tests, allowing the long compiler and driver suites to overlap. Both jobs are
required, and each builds only its selected test targets.

Sanitizer jobs stream verbose CTest output, including the compiler aggregate's
current suite and per-suite elapsed time, and retain JUnit and CTest logs even
when a test fails. `compiler_pipeline` contains several suites, so an unchanged
CTest completion count alone does not show which suite is active.

Every native job builds with the reviewed runtime compiler that
`scripts/configure_ci_toolchain.sh` installs and exports: GCC 14 with LLVM 20 on
`ubuntu-24.04`, and Apple clang 21 (Command Line Tools 27) with Homebrew LLVM 22
on `macos-26`. The script fails before the build if `runtime/src/atomic_lane.h`
does not admit the compiler.

Contributors run focused checks only; the queue owns the complete suite. A
queue failure belongs to the change that introduced it.

## Nightly toolchain

`workflows/nightly.yml` runs daily at 02:00 UTC and on manual dispatch (an
optional exact main commit; only main head publishes). It downloads the
production archives from that commit's successful full CI run and attempt,
checks their source/platform/configuration manifest and file hashes, and
republishes the rolling `nightly` prerelease without rebuilding or repeating
the complete suite. Full CI builds `GTI_RELEASE_BUILD=ON` bundled-LLVM packages
for `linux-x64` (`ubuntu-24.04`) and `darwin-arm64` (`macos-26`), validates their
installed consumers, runs the full release suite and separate sanitizer gates,
and retains the archives for 30 days. Nightly publishes one archive and
`.sha256` per platform plus
`BUILD_ID`. A commit that is no longer main head does not publish. Scheduled
runs skip unchanged main; rerunning a failed publish job or dispatching the
same current-main commit can finish an interrupted upload. Missing, expired,
mixed or mismatched artifacts stop publication; they never cause an older
compiler to be stamped with a new commit. Dispatch `ci.yml` on main to obtain
fresh full evidence when commissioning after an approved merge exception or
recovering artifacts. A merge exception itself and PR/automation-only CI are
not release evidence. The publish job alone holds
`contents: write` (plus provenance attestation scopes), is serialized, and moves
the `nightly` tag with an explicit lease. `linux-arm64` is not packaged until a
runtime-safety review admits a linux-aarch64 compiler profile in
`runtime/src/atomic_lane.h`. Stable `v<VERSION>` releases come from the release
train below, which cuts the commit this workflow published. See
[GitHub nightly publication](../docs/architecture/build-and-driver.md#github-nightly-publication).

## Release train

`scripts/release_train.py` decides; four workflows act. See
[GitHub release train](../docs/architecture/build-and-driver.md#github-release-train)
for the full contract.

| Workflow | Trigger | Does |
| --- | --- | --- |
| `workflows/train.yml` job `cut` | 04:00 UTC every other day (the daily schedule admits every second UTC day), dispatch | cuts the published nightly once it has been on `main` 24 h and passed full CI with retained release artifacts; opens and queues a `rollup=never` `Release-Cut:` PR with the App token |
| `workflows/beta.yml` | `push` to `main` (the queue's merge) or `beta/**` (a merged backport), daily 05:30 fallback, dispatch | builds the release cut (or the line's new tip) with its own bundled build, installed checks and full release-suite shards; publishes prerelease `v<VERSION>-beta.<K>` with provenance; creates `beta/<major>.<minor>` |
| `workflows/backport.yml` | pull requests into `beta/**` | `Backport provenance` (each commit a `cherry-pick -x` of a `main` commit; an optional final patch release cut), the release suite, `Backport gate` |
| `workflows/train.yml` job `promote` | with the train, dispatch | behind the `stable` environment: republishes the newest beta that soaked 48 h with no open `release-blocker` issue naming its version as `v<VERSION>`, byte for byte, and marks it latest |
| `workflows/rollback.yml` | dispatch only | behind the `stable` environment: `withdraw-beta` or `yank-stable`, recorded on the `release-status` branch the installer reads |

The cut job is the only train job with a secret (the rollup App's, for the
PR). Beta publication, promotion and rollback write with `GITHUB_TOKEN`
(`contents: write`): their tags, releases and status commits need not trigger
workflows. No train job forces, moves or deletes a tag, branch, release or
asset. Dispatch overrides (`min_age_hours`, `soak_hours`) exist for the owner's
bootstrap; neither waives full CI evidence for the nightly source or the
separate merge-queue evidence for the release-cut landing, and a lowered soak is
written into the stable release notes.

One-time owner setup, in this order:

1. Install the rollup App (section "Rollup bot"); the cut job reuses its
   `GTI_ROLLUP_APP_ID` and `GTI_ROLLUP_APP_PRIVATE_KEY` secrets and needs no
   new permission.
2. Create the label `release-blocker` (Issues, Labels). An open issue with
   it whose title or body names a version (`0.494.0`, `v0.494.0`,
   `0.494.0-beta.2`) blocks that version's promotion.
3. Create the `stable` environment (Settings, Environments, New environment):
   required reviewer yourself, leave *Prevent self-review* off so you can
   approve your own dispatches, deployment branches limited to *Selected
   branches* `main`, and no environment secrets. Promotion and rollback wait
   for its approval.
4. Wait for the first `nightly` release to exist, then enable release
   immutability (Settings, General, Releases, *Enable release immutability*).
   It applies only to releases published after it is enabled, so the rolling
   `nightly` release keeps replacing its assets and moving its tag; never
   delete and recreate the `nightly` release afterwards, or it becomes
   immutable and stops rolling. Beta and stable releases are published from
   drafts, so their assets are complete before they lock.
5. Apply the three proposed rulesets. Review them first:

   ```sh
   python3 scripts/release_train.py rulesets
   ```

   It prints a JSON list: `release-tags` (`refs/tags/v*`: no update, force
   update or deletion; creation stays open so the train can tag), `beta`
   (`refs/heads/beta/*`: pull requests merged by rebase, linear history, an
   up-to-date branch with `CI gate` and `Backport gate`, no force or
   deletion), and `release-status` (no force or deletion). Apply each with:

   ```sh
   for name in release-tags beta release-status; do
     python3 scripts/release_train.py rulesets --name "${name}" |
       gh api -X POST repos/lang-gti-official/gti/rulesets --input -
   done
   ```

   If the first beta's final step cannot create `beta/<major>.<minor>` under
   the `beta` ruleset, create the branch from the published tag
   (`git push origin v0.N.0-beta.1^{commit}:refs/heads/beta/0.N`) with the
   ruleset briefly disabled, then re-enable it.
6. Bootstrap: dispatch `train.yml` with `stage=cut` (and `min_age_hours=0` if
   the nightly commit is younger than 24 h). After the cut PR merges and
   `beta.yml` publishes beta 1, dispatch `stage=promote` (with `soak_hours=0`
   to skip the soak) and approve the `stable` deployment.

To withdraw a beta or yank a stable version, dispatch `workflows/rollback.yml`
with the action, the version (`0.494.0-beta.2` or `0.494.0`) and a one-line
reason, and approve the `stable` deployment. A backport is a pull request into
`beta/<major>.<minor>` labelled `backport` whose commits are
`git cherry-pick -x <main commit>`; end it with
`python3 scripts/cut_release.py --title "<title>"` committed with its
`Release-Cut:` trailer when it carries a user-visible fragment.

## Rollup bot

`workflows/rollup.yml` runs `scripts/rollup.py` every 30 minutes, on manual
dispatch, and when someone with write, maintain or admin permission comments
exactly `/rollup` on an issue or pull request. It composes green PRs labelled
`rollup=maybe` or `rollup=always` into one `rollup/<date>-<n>` PR, queues that
PR once its `CI gate` passes, and reconciles the result, so one complete queue
run covers up to six PRs. Labels: `rollup=never` validates a PR alone, `p=N`
raises its priority, and the bot manages `rollup-failed` and `needs-rebase`.
The bot runs only `main`'s code; it never checks out or executes PR content,
and merges with Git plumbing with hooks disabled.

Pull requests and pushes made with `GITHUB_TOKEN` trigger no other workflow,
so a rollup PR opened with it would never get its PR checks. The bot therefore
writes only with a short-lived installation token of a GitHub App, minted in
the job by `actions/create-github-app-token` for this repository only. It never
falls back to `GITHUB_TOKEN` for writes; that token only reads (the checkout,
the commenter's permission and the merge-queue CI run list). If either secret
below is missing, the job fails and names both.

One-time owner setup:

1. Create a GitHub App owned by the `lang-gti-official` organization
   (Settings, Developer settings, GitHub Apps, New GitHub App). Give it any
   homepage URL and **disable the webhook** (clear *Active*); the bot needs no
   events. Grant exactly these repository permissions:

   | Permission | Access | Used for |
   | --- | --- | --- |
   | Contents | Read and write | push and delete `rollup/*` branches |
   | Pull requests | Read and write | open, close and queue rollup PRs; close integrated members |
   | Merge queues | Read and write | add rollup PRs and isolated members to the merge queue |
   | Issues | Read and write | labels and comments on PRs |
   | Checks | Read-only | the `CI gate` check run on each head |
   | Commit statuses | Read-only | commit status reads (the bot itself reads check runs today) |
   | Metadata | Read-only | mandatory |

   No organization or account permissions. Allow installation only on this
   account.
2. Install the App on the organization with **Only select repositories** and
   select `lang-gti-official/gti` alone.
3. Note the App ID from the App's settings page and generate a private key
   (a `.pem` download).
4. In `lang-gti-official/gti`, Settings, Secrets and variables, Actions, add two
   repository secrets: `GTI_ROLLUP_APP_ID` (the numeric App ID) and
   `GTI_ROLLUP_APP_PRIVATE_KEY` (the whole `.pem` contents).
5. Optionally create the labels `rollup=maybe`, `rollup=always`,
   `rollup=never`, `rollup-failed` and `needs-rebase` with colours of your
   choice; otherwise GitHub creates them on first use.
6. Run the workflow once with *dry_run* to see the planned actions.

The App bypasses no rule: rollup PRs pass the same `CI gate` and merge queue.
Because the App has no Workflows permission, a PR whose commits touch
`.github/workflows/` can never be rolled up; such changes are Risk-system and
validate alone anyway. Rotate the private key by generating a new one, replacing the
secret and deleting the old key.

## Workflow policy

Actions are pinned to full commit SHAs. Workflows run with a read-only token.
Only these jobs write: nightly publication, beta publication, stable promotion
and rollback, with `GITHUB_TOKEN`; and the rollup bot and the release cut, with
the rollup App's token. No other workflow uses a secret.
Set positive `GTI_JOBS` and `GTI_TEST_JOBS` repository variables to override
the CPU/memory-based budgets. Compiler-object caches are separate for native, sanitizer and bundled-release
profiles; cache errors never skip builds or validation.
GitHub shares a cache across branches only when `main` saved it, so
`workflows/cache.yml` warms the system-LLVM native and sanitizer configurations
after merges to `main` (and weekly). The production package job saves the
bundled-release cache for its integration shards. Cache reuse only accelerates
compilation; every required test and package check still runs.

Before its build, the sanitizer job runs `scripts/check_ci_sanitizers.py`: a
small, race-free ASan/UBSan probe with no GTI code, so a runner that cannot
execute sanitizer binaries fails in seconds instead of after a long build.
Hosted jobs configure with `GTI_ENABLE_THREAD_SANITIZER_TESTS=OFF`; dedicated
ThreadSanitizer CTests then report an explicit skip (return code 77), never a
pass. A runner meant for TSAN must first pass
`scripts/check_ci_sanitizers.py --sanitizer thread` in its exact environment.
If it reports `ThreadSanitizer: unexpected memory mapping`, check the compiler
runtime, kernel, address-space limits and container seccomp policy rather than
using a privileged container.

The previous GitHub-era workflows are archived under
`docs/plans/archive/github-actions-20261024/` and do not run.

Compiler sanitizer coverage uses two compiler-suite partitions and two library
partitions. Each compiler partition validates the complete manifest census before
running its assigned suites; their union runs every compiler case once. Native
CI continues to run the unpartitioned compiler aggregate. Suite start and elapsed
time are emitted in verbose sanitizer logs. No sanitizer test is removed and
the existing timeout and per-runner memory limits remain in force.

The serial single-body compile-scaling check warms all three workloads, then
uses the median growth from three interleaved baseline/small/large measurements.
This avoids subtracting independently selected fastest times from different
runner conditions. Every sample is logged; the fourfold workload increase and
8x growth limit are unchanged. `single_body_scaling_measurement` checks transient
baseline noise, persistent excessive growth, and quadratic/cubic rejection.

## Preview toolchains and gtiup

`preview.yml` responds to the canonical preview source branch, with an explicit
main dispatch for commissioning/recovery. It builds both supported production
platforms, runs combined preview admission and installed consumers, and publishes
only their matching source/run/attempt artifacts to the rolling `preview`
prerelease. Both package jobs must run in the same workflow attempt. This
channel does not satisfy nightly's full-main-CI gate or promote into main.
`gtiup` downloads and selects complete toolchains for preview, nightly, beta and
stable; managed Neovim uses the selected LSP/parser. See the root README and
[managed toolchains](../docs/architecture/build-and-driver.md#managed-toolchains-and-preview-distribution).
