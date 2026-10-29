# GTI on GitHub

https://github.com/lang-gti-official/gti is canonical for source, branches,
pull requests, issues, CI and, once its release workflow lands, toolchain
releases. The reference website and the `gti add` package registry stay on
Gitea; see [Gitea](../.gitea/README.md).

## Integration

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
- In the merge queue: the complete suite on the exact candidate, split into
  cost-balanced shards (`scripts/validation_checks.py shard`, balanced by
  `tests/ci_test_durations.json`) on both platforms, the ASan/UBSan subset, the
  installed-toolchain smoke and deterministic docgen. The gate merges the shard
  reports and fails unless each platform ran its whole inventory exactly once.

Every native job builds with the reviewed runtime compiler that
`scripts/configure_ci_toolchain.sh` installs and exports: GCC 14 with LLVM 20 on
`ubuntu-24.04`, and Apple clang 21 (Command Line Tools 27) with Homebrew LLVM 22
on `macos-26`. The script fails before the build if `runtime/src/atomic_lane.h`
does not admit the compiler.

Contributors run focused checks only; the queue owns the complete suite. A
queue failure belongs to the change that introduced it.

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

Actions are pinned to full commit SHAs. Workflows run with a read-only token; only
the rollup bot writes, with its own App token.
Set positive `GTI_JOBS` and `GTI_TEST_JOBS` repository variables to override
the CPU/memory-based budgets. Compiler-object caches are separate for native
and sanitizer profiles; cache errors never skip builds or validation.

The previous GitHub-era workflows are archived under
`docs/plans/archive/github-actions-20261024/` and do not run.
