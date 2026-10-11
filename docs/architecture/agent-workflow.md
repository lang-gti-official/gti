# Agent Implementation Workflow

Status: Repository automation and the owner's implementation-task contract.

An implementation normally ends with a GitHub PR merged into `main`, the primary
local checkout synchronized, the Gitea backup mirror updated, and the task's
branches/worktree cleaned up. This
makes the user's local installer consume integrated work. Review-only, PR-only,
local-only and installation instructions override the appropriate default.
`AGENTS.md` provides standing authorization; the `agent-workflow` skill supplies
agent procedure and `finish-release` supplies version/publication policy.

## Task identity and operations

`scripts/agent_workflow.py` uses Python's standard library and Git. One task
owns a unique `codex/` branch, a separate worktree and one PR. The worktree
must lie outside the primary checkout: `start` refuses nested paths, including
Claude Code's managed `.claude/worktrees/`, so such clients let `start` create
the task worktree elsewhere. GitHub PRs and published commits are authoritative
inventory; token-free JSON records under the common Git directory's
`gti-agent-workflow/` retain operation state and recovery pointers. They are
shared across worktrees, never committed, and contain no credentials. HTTP
redirects are refused. Canonical fetch and push URLs are both checked before
credentials or code are sent.

Metadata operations hold a short atomic directory lock
(`gti-agent-workflow/lock`, with PID/time in `owner.json`); a waiter polls for
up to thirty seconds and then reports the owner. An interrupted lock must be
inspected before its exact directory is removed; the helper never removes a
live lock. `verify` holds the lock only while it records the attempt and the
result. Its checks run inside a `suite` slot of the shared build window below,
so a peer's `start`, `status`, `checkpoint` or `cleanup` never waits on a
validation run. GitHub's up-to-date branch rule closes the race with other
machines. Nothing locks ordinary editing.

| Command | Result |
| --- | --- |
| `start <task> --title <text> --worktree <path> [--branch codex/<name>] [--adopt]` | Fetch main, create or register a clean worktree, publish its branch; rediscover a PR after the first meaningful commit. |
| `checkpoint <task> [--message <text> --paths <files...>]` | Commit explicitly scoped paths or publish an existing clean commit; create a draft PR on the first meaningful checkpoint. |
| `reconcile <task>` | Merge current main into the task branch, preserve conflicts for resolution, invalidate old validation and publish. |
| `verify <task> --checks-file <json>` | Record the attempt, release the lock, execute nonempty argv arrays without a shell inside a build-window `suite` slot, then record passing checks against the published clean head and current remote main. |
| `finish <task> --body-file <text> [--pr-only]` | Publish the final PR description, mark ready, verify identities/protection, merge the exact head without bypasses and synchronize local main. PR-only stops ready for review. |
| `sync <task>` | Confirm merged-head/merge-commit reachability, fast-forward primary main, preserving unrelated local edits, then push `main` and the tags to the `gitea` mirror remote and record the result. |
| `cleanup <task>` | After confirmed merge/sync, remove a plain clean worktree with no ignored files and delete only unchanged integrated branches. |
| `abandon <task> --reason <text>` | Publish the final checkpoint and close its PR with a reason, preserving branch and worktree. |
| `status [task]` | Read task records and current remote PR state. |
| `protection [--configure]` | Audit, or establish the owner-approved GitHub branch protection for `main` (pull request required, up-to-date branch, administrators bound, no force pushes or deletion), retaining existing review/status requirements. |

Checkpoint paths are literal files, including exact identities for deleted
files: Git pathspec magic, deleted-directory prefixes, directory/root staging,
and pre-existing staged work cannot silently include another task's changes.
New task starts reject any already-published branch/PR identity. First publication
uses a compare-and-create lease, so a concurrently created branch is preserved.

`--repo <checkout>` precedes the command when invoked elsewhere. Credentials
come from `GTI_GITHUB_TOKEN`, `GH_TOKEN`, `GITHUB_TOKEN`, the primary
checkout's ignored `.env` (the helper parses the token key without executing
the file), or the authenticated GitHub CLI (`gh auth token`). A worktree never
needs its own credential file. `origin` must be the canonical GitHub
repository and, when present, the `gitea` remote must be the Gitea mirror;
both fetch and push URLs are checked before anything is sent.

## Hosts and records

GitHub (`gti-lang-official/gti`) owns branches, pull requests, issues, CI and
releases. The self-hosted Gitea repository is a backup mirror of `main` and
the tags: `sync` pushes both there after the primary fast-forward (`main`
without force, so a merge that landed only on the mirror surfaces as a
rejected push; tags with force so a moved `nightly` follows) and records `mirror` (remote, pushed,
time, error) in the task state. A failing or missing mirror remote is reported
and does not undo the sync; the GitHub `mirror.yml` workflow is a second path
when its secret is configured. Task records created before the move carry no
`host` field; `status` lists them as `legacy-gitea` without contacting any
API, every other command refuses them, and `start --adopt` renames the record
to `<task>.legacy-gitea.json` and registers the worktree afresh on GitHub.

## Build window

`scripts/build_window.py` is the shared, counted semaphore for heavy local
work. Three concurrent full CTest runs on one machine have stretched a
one-minute test to seventeen minutes and made timing-sensitive LSP tests
flake, so every worktree and session of a repository shares one window:

```sh
python3 scripts/build_window.py --kind suite -- ctest --test-dir build-release -j 6
python3 scripts/build_window.py --kind build -- cmake --build build-release -j 8
python3 scripts/build_window.py --status
```

A `suite` slot wraps a whole or broad CTest run or a standalone harness
matrix; a `build` slot wraps a whole `cmake --build`. Focused checks (`ctest
-R '^name$'`, one harness fixture, a single-file `gti` compile) run directly.
The defaults are one concurrent `suite` slot and two concurrent `build` slots.

| Setting | Flag | Environment | Default |
| --- | --- | --- | --- |
| suite slots | `--suite-slots N` | `GTI_BUILD_WINDOW_SUITE_SLOTS` | 1 |
| build slots | `--build-slots N` | `GTI_BUILD_WINDOW_BUILD_SLOTS` | 2 |
| poll interval | `--poll-seconds S` | `GTI_BUILD_WINDOW_POLL_SECONDS` | 5 (at most 60) |
| wait limit | `--timeout-seconds S` | `GTI_BUILD_WINDOW_TIMEOUT_SECONDS` | wait indefinitely |
| storage | `--dir <path>` | `GTI_BUILD_WINDOW_DIR` | `<git common dir>/gti-build-window` |

Flags take precedence over the environment. The storage directory lives under
the Git common directory (`--repo <checkout>` selects which one), so it is
shared by every worktree, never committed, and holds no credentials. Each
slot kind has its own subdirectory, `suite/` or `build/`, and each holder owns
one JSON file there named `<pid>-<token>.json` recording `pid`, `time`, `kind`,
`command` (argv), `cwd` and, for helper validation, the `worktree` and
`checks`. The count-and-claim step is serialized by a short `lock` directory
with its own `owner.json`; it is held for milliseconds and reclaimed when its
owner has exited or it is older than a minute.

Acquisition polls with a bounded sleep. While waiting it prints, on standard
error, which processes hold the slots, with their pids, start times and
commands, and repeats that line when the holders change or once a minute. A
slot whose recorded pid no longer exists is reclaimed automatically before
the count is taken. The wrapper forwards interrupt and terminate signals to
the wrapped command, waits for it, releases the slot and exits with the
command's status (128 plus the signal number when the command was killed), so
Ctrl-C never leaks a slot. The window is advisory coordination between
cooperating agents, not a security boundary. Owner liveness uses
`os.kill(pid, 0)` on POSIX. Windows cannot test liveness without extra
dependencies, so there a slot is never reclaimed automatically; `--status`
reports the recorded owner and the file to delete by hand.

`verify` acquires a `suite` slot around its checks and records them in the
slot file, so helper validation and ad hoc suite runs queue behind each
other. Builds that the checks need should be run beforehand through a `build`
slot. The environment variables above apply to the helper too.

## Integration and validation

The first branch push can contain no changes; draft creation waits for a real
commit because unchanged branches cannot form meaningful PRs. PRs are created
as GitHub drafts and `finish` marks them ready for review (the one GraphQL
call; REST cannot leave draft state). An interrupted PR-create request is
recovered by branch lookup before another create. Head and base repository identities must both be canonical GTI, so a
similarly named fork branch cannot be adopted or modified. Closed task branches
are not reused.

Checkpoint publication is distinct from validation. The agent selects suitable
checks using the owning skill and `verify-change`, with optimized compilers,
required examples and cross-phase evidence. Verification records are operational
evidence, not a security attestation or a replacement for code review. Each
record includes executed commands, head, base and time. Any changed commit,
dirty source or advancing main requires reconciliation/revalidation. Failed
checks never mark the task ready.

Finish merges through GitHub with `merge_method: merge` and `sha` pinned to
the validated head, so a branch that moved is refused. The `main` protection
rule requires a pull request and an up-to-date branch, binds administrators,
and denies force pushes and deletion. Existing required checks and reviews are
preserved and enforced by GitHub. Pending/rejected requirements
leave work unmerged; an agent resumes after they change rather than claiming
completion. Release workflows run after integration and need not finish before
local installation. VERSION follows normal repository policy; workflow-only
automation does not change the packaged compiler version.

## Preservation and cleanup

Fast-forward sync leaves unrelated primary edits in place and refuses overlap.
Incoming paths are checked for untracked/ignored collisions, and Git receives
`--no-overwrite-ignore` so a newly tracked path cannot overwrite ignored data.
The agent backs up only the overlapping intended files and reconciles them
before retrying. Divergent primary commits receive a named recovery ref and
must be integrated through a separate PR; no reset or hidden merge rewrites
the primary checkout. Report merge success separately from blocked local sync.

Cleanup requires remote PR integration, local main reachability and unchanged
branch tips. Remote deletion uses a compare-and-delete lease for the merged SHA;
this is a conditional deletion, never a rewritten shared history. New source,
untracked files, unexpected commits or ignored files stop removal. Managed app
worktrees should be archived with the app after preserving needed ignored data,
then the helper can finish branch cleanup. A recovery snapshot retained by that
archive remains accessible through the chat attachment. Unmerged abandoned work
is retained indefinitely until a separate explicit disposition.

Interrupted operations are retried using the same task. Task records survive
merge rejection and sync failure. A confirmed remote merge resumes at sync;
cleanup tolerates branches already deleted. If records are lost, preserve and
adopt the published branch/worktree, rediscover the PR, and confirm the remote
merge before proceeding. Recovery adoption accepts an existing open/ready PR
even when main advanced, allowing helper reconciliation without first requiring
an unknown task to reconcile itself. Already merged tasks resume sync; lost
abandonment responses are confirmed remotely and do not republish closed work.
Server API rejections are reported without secrets.
