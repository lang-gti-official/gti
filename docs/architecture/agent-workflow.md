# Agent Implementation Workflow

Status: Repository automation and the owner's implementation-task contract.

Canonical `origin` is `https://github.com/lang-gti-official/gti.git`. GitHub
owns branches, PRs, issues, CI and toolchain releases; Gitea keeps only the
reference website and the `gti add` package registry, and its `gti-lang/gti`
`main` is frozen. Gitea-era instructions in
historical notes or pre-cutover worktrees do not select the current host. For a
preserved worktree, consult the primary checkout's current `AGENTS.md` and
skill, and invoke its helper with `--repo <task-worktree>`. Preserve live runs
and follow the explicit [record-rehosting contract](#hosts-and-records); never
overwrite their identity.

An implementation normally ends with its PR ready and in the merge queue, with
focused validation recorded. The queue validates the exact candidate and
merges it; `sync` then synchronizes the primary local checkout and `cleanup`
removes the task's branches and worktree. This makes the user's local installer
consume integrated work. Review-only, PR-only, local-only and installation
instructions override the appropriate default.
`AGENTS.md` provides standing authorization; the `agent-workflow` skill supplies
agent procedure and `finish-release` supplies version/publication policy.

## Bounded output, analysis and waiting

Execution efficiency protects time and context without weakening design review,
defined safety, performance acceptance, or necessary validation. No fixed limit
on reasoning depth or the number of reviews applies. A new counterexample,
unproven invariant, alternative representation, or performance/maintainability
concern can justify revisiting a decision without an external code change.
At meaningful checkpoints retain the question, findings and remaining proof
obligations, so reconsideration builds on earlier work rather than loses it.

### Output and evidence

Routine tool/log responses must fit within 200 lines and 16 KiB, including
aggregated nested calls. Keep raw command output and structured reports under
the normal local/remote evidence contract; expose exit status, pass/fail/skip
counts, source identity and paths first. Fetching remote results still obeys
the separate bundle-retention/truncation limits below. Display limits never
justify altering tests, cutting off a command, hiding failures, or describing
truncated evidence as complete.

For saved logs use the read-only, byte-and-line-bounded tail viewer:

```sh
python3 scripts/bounded_log.py /absolute/path/build.log
python3 scripts/bounded_log.py /absolute/path/test.log --lines 100 --max-bytes 16384
```

Its defaults are 40 output lines and 8 KiB; hard ceilings are 200 lines and
16 KiB, including its header. It reads only a bounded tail, also handling huge
single-line JSON and invalid UTF-8 without flooding context. It does not modify
the file. Success means the excerpt was read, not that the recorded test passed.
Regression coverage is `bounded_log` / `python3 tests/bounded_log_test.py`.

Locate failures with scoped searches and inspect the relevant span/excerpt.
If larger inspection is necessary, state the question and use focused chunks
or structured projections; never dump whole session histories, ignored-file
inventories or directory trees by default. Activated skill instructions must
be read completely, using chunks if needed. For canonical manuals, use the
documentation index and headings to select the sections that own this task.
Do not re-read unchanged material solely because a process is quiet.

### Live operations and checkpoints

Keep a compact ledger in existing task/PR notes or temporary state, containing
acceptance obligations, resolved decisions/open questions, exact source/base
and compiler/build/platform identities, commands/results/log paths, and live
session or remote run IDs. This is operational state, not another canonical
architecture document. On compaction/resumption recover that state and pending
operations before launching anything again; re-read instructions when required
or changed, and revisit unresolved work.

Resume/wait on the same process or run. Prefer product session/thread waits or
managed detached remote runs; use interruptible waits no longer than 60 seconds
at a time. Begin remote status checks at 30–60 seconds and back off to 2–5
minutes if unchanged, using bounded interruptible waits between checks. Poll
one authoritative status source, not repeated SSH tails, process lists, Git
state and document reads together. Keep user updates concise; report meaningful
progress, failures, required decisions, or a materially changed schedule.

Quiet output or a normal build-window queue is not proof of a hang or a blocker.
Use the command's timeout, elapsed time and liveness/progress evidence before
investigating a stall. Never reclaim a live peer's slot or duplicate an active
validation run. Independent useful analysis may proceed while tests run; when
only an external result remains, wait rather than manufacture additional review.
These rules do not discourage deep review, counterexample searches or fixing
newly discovered obligations.

Reconcile near the final validation boundary. If main advances during a run,
retain its exact-head evidence and inspect the upstream delta after completion.
Then select fresh validation for the reconciled clean published head/base.
Every affected, previously failed and required broad/platform gate still runs;
uncertain interaction risk calls for broader checks, not optimistic omission.
Do not relabel earlier results or override the helper's identity checks. When
cost or duration grows materially, explain the cause, evidence obtained and
remaining work to the user; efficiency does not authorize reduced correctness.

## Sidebar task titles

Each agent keeps its own chat's sidebar title aligned with its current task.
At task start, use a short, specific description such as `Fix LSP test-target
diagnostics`; replace it when beginning a different task. Use the client's
chat-title control (for example, `set_thread_title` targeting the current chat)
when available. If unsupported, report that limitation in the task update and
continue; a missing UI control is not an implementation blocker. Do not claim
an unsuccessful rename succeeded or rename another agent's chat for this rule.

On successful delivery, use `Complete — <task>`. Normal implementation delivery
includes required validation, PR integration and local-main synchronization;
explicit PR-only/local-only requests retain their requested boundary. Until
then, use an honest status when useful, such as `Validating — <task>`,
`Handed off — <task>`, `Blocked — <task>` or `Cancelled — <task>`, rather than
`Complete`. Update titles at task/status transitions, not on every poll.
Sidebar labels aid navigation; PRs, commits and retained evidence remain the
authoritative work inventory and verification record.

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
validation run. The merge queue serializes integration across machines, so no
local race with other tasks needs closing. Nothing locks ordinary editing.

| Command | Result |
| --- | --- |
| `start <task> --title <text> --worktree <path> [--branch codex/<name>] [--adopt]` | Fetch main, create or register a clean worktree, publish its branch; rediscover a PR after the first meaningful commit. |
| `checkpoint <task> [--message <text> --paths <files...>]` | Commit explicitly scoped paths or publish an existing clean commit; create a draft PR on the first meaningful checkpoint. |
| `reconcile <task>` | Merge current main into the task branch to resolve a conflict, preserve conflicts for resolution, invalidate old validation and publish. |
| `verify <task> --checks-file <json>` | Record the attempt, release the lock, execute nonempty argv arrays without a shell inside a build-window `suite` slot, then record passing focused checks against the published clean head. An advancing main does not void them. |
| `verify <task> --focused --checks-file <json>` | For a checked automation-only or source-only GTI stdlib diff, run owning checks directly and record scope `automation` or `source-stdlib`; refuse compiler/native/build changes and remote use. Actual broad workloads still use the window. |
| `finish <task> --body-file <text> [--pr-only] [--no-rollup]` | Publish the final PR description, mark ready, verify identities and the `main` ruleset. A rollup-eligible PR is labelled `rollup=maybe` (`rollup=always` when automation-only) and waits for the rollup bot (phase `rollup`); a Risk-system or `--no-rollup` PR is labelled `rollup=never` and registers native GitHub merge-when-ready with an expected-head guard. Required PR checks/reviews may still be pending (phase `awaiting-ci`); GitHub enters the queue automatically once they pass (phase `queued`). Both states are completed contributor handoffs. Once merged, it synchronizes local main. PR-only stops ready for review. |
| `sync <task>` | Confirm merged-head/merge-commit reachability, or the rollup bot's proof for a member it closed, and fast-forward primary main, preserving unrelated local edits. Never push to a backup. |
| `rehost <task> --pr <number>` | Explicitly bind a schema-2 GitHub or schema-3 Gitea task to its migrated GitHub PR when the PR head carries the task's published tree; move a clean worktree onto the identical rewritten commit and preserve the original record. Refuse live verification. |
| `cleanup <task>` | After confirmed merge/sync, remove a plain clean worktree with no ignored files and delete only unchanged integrated branches. |
| `abandon <task> --reason <text>` | Publish the final checkpoint and close its PR with a reason, preserving branch and worktree. A task with no PR whose branch holds no commits beyond `origin/main` has nothing to preserve. Its clean worktree and branches are removed and the reason is recorded. |
| `status [task]` | Read task records and current remote PR state. |
| `protection [--configure]` | Audit, or establish the owner-approved `main` ruleset: pull requests, the merge queue and the `CI gate` check required; no force pushes, deletion or bypass actors. |

Checkpoint paths are literal files, including exact identities for deleted
files: Git pathspec magic, deleted-directory prefixes, directory/root staging,
and pre-existing staged work cannot silently include another task's changes.
New task starts reject any already-published branch/PR identity. First publication
uses a compare-and-create lease, so a concurrently created branch is preserved.

`--repo <checkout>` precedes the command when invoked elsewhere. Credentials
come from `GH_TOKEN`/`GITHUB_TOKEN` or the authenticated GitHub CLI
(`gh auth token`), read per run and never stored. A worktree never needs its
own credential file. Both fetch and all push URLs of `origin` must be exactly
`https://github.com/lang-gti-official/gti.git` before credentials or
publication are used. Authenticated API requests never follow redirects.

## Hosts and records

Policy revision (2026-10-29): canonical source, PRs, issues and CI moved to
GitHub `lang-gti-official/gti`, and toolchain releases follow. Gitea keeps only
the reference website and the `gti add` package registry; the Gitea `gti-lang/gti` `main` is frozen with an empty merge allowlist.
Publishing to GitHub rewrote commit identities only (author, committer and
tagger), so every tree is identical while every commit ID differs; the owner
retains the old-to-new map outside the repository.

New helper records use schema 4 and `host: github`. Gitea-era schema-3 records
show `legacy-gitea`, and older records `legacy-<host>`, without contacting an
API using their old numbers. Schema-2 GitHub records also refer to the earlier
repository history and PR numbers, even when their URL names this repository.
To move either a schema-2 GitHub or schema-3 Gitea task, publish its branch to
GitHub with the same identity rewrite, open its PR, and run
`rehost <task> --pr <number>` once its live validation has finished. The helper
requires the PR head to carry exactly the task's published tree, saves the
unmodified original record under task evidence, moves a clean worktree whose
head is the published head onto the identical rewritten commit, and records
both identities. Focused evidence bound to old commit IDs is dropped and must be
recorded again with `verify`. A running `verifying` record refuses rehosting,
and `start --adopt` cannot silently replace either kind of migrated PR.

## Build window

`scripts/build_window.py` is the shared, counted semaphore for heavy local
work. Three concurrent full CTest runs on one machine have stretched a
one-minute test to seventeen minutes and made timing-sensitive LSP tests
flake, so every worktree and session of a repository shares one window:

```sh
python3 scripts/build_window.py --kind suite -- ctest --test-dir build-release -j 12 --test-load 12
python3 scripts/build_window.py --kind build -- cmake --build build-release -j 8
python3 scripts/build_window.py --status
```

A suite may use every core of this 12-core host, but `--test-load 12` stops
CTest from starting another test while the load average is at or above twelve.
An idle host therefore runs the suite at full width, and the suite backs off
while the window's builds or other sessions' work load the machine. CTest
charges a multi-worker case suite its reserved processors against `-j`; at
`-j 6` those reservations, not test durations, bounded the full suite's wall
time.

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
with its own `owner.json`; it is normally held for milliseconds and reclaimed
when its owner has exited. A live owner is never reclaimed because of age;
an absent/unreadable owner gets a one-minute initialization grace period before
reclamation, so peers cannot steal the gap between `mkdir` and owner publication.

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

Local `verify --checks-file` acquires a `suite` slot around its checks and records them in the
slot file, so helper validation and ad hoc suite runs queue behind each
other. Builds that the checks need should be run beforehand through a `build`
slot. The environment variables above apply to the helper too.

<a id="mac-mini-remote-runs"></a>

## Remote build/test hosts

`scripts/remote_runner.py` is repository automation, not a second compiler or
driver. It sends a disposable source snapshot to a trusted SSH account, runs
exact build/test argument vectors, and brings evidence back. It requires
Python 3.9+, Git and SSH locally; each host additionally needs CMake, Ninja and
the repository's native build dependencies. Ubuntu profiles use the local
`tailscale ssh` client, which validates the server's advertised node host key
through the Tailscale coordination server ([Tailscale CLI](https://tailscale.com/docs/reference/tailscale-cli)).
Tailscale authentication/policy must already permit `root` access. The Mini
uses its Tailscale address with ordinary SSH and its existing keys/host
verification. Neither transport disables host-key checks or changes SSH
configuration.

Three named presets are available; saved connection overrides live only under
the Git common directory:

| Runner | Connection | OS / declared capacity | Default build / test workers |
| --- | --- | --- | --- |
| `mini` | `lukeh@100.103.38.120` | macOS; observed host CPU count | half CPUs / all CPUs |
| `ubuntu-6` | `root@100.77.66.96` | Ubuntu; 6 vCPUs, 13 GB RAM | 3 / 6 |
| `ubuntu-8` | `root@100.81.111.76` | Ubuntu; 8 vCPUs, 13 GB RAM | 3 / 6 |

Selection is explicit, not automatic failover or test sharding: inspect
`status --all`, then choose an idle appropriate host. Linux evidence does not
replace macOS-only acceptance. `run --platform Darwin` or `--platform Linux`
rejects a mismatched selection before uploading; the worker also verifies the
required platform before executing steps. Configure validates the actual OS.

From any task worktree, configure once (settings are shared in the Git common
directory, not committed):

```sh
python3 scripts/remote_runner.py --runner mini --host lukeh@100.103.38.120 configure
python3 scripts/remote_runner.py --runner ubuntu-6 configure
python3 scripts/remote_runner.py --runner ubuntu-8 configure
python3 scripts/remote_runner.py hosts
python3 scripts/remote_runner.py status --all
python3 scripts/remote_runner.py run --target gti_runtime_failure_tests --ctest-regex '^runtime_failure_boundary$'
```

For a compiler feature, build the compiler and applicable test targets in the
**same run**; binaries do not persist for a later run. For example:

```sh
python3 scripts/remote_runner.py --runner ubuntu-6 run --target gti --target gti_optimizer_tests --ctest-regex '^optimizer_foundation$'
python3 scripts/remote_runner.py --runner ubuntu-8 run --full-suite --detach
python3 scripts/remote_runner.py run --full-suite --detach
python3 scripts/remote_runner.py status
python3 scripts/remote_runner.py status run-<32-hex-digits>
python3 scripts/remote_runner.py fetch run-<32-hex-digits>
python3 scripts/remote_runner.py cancel run-<32-hex-digits>
```

Omit `--target` to build all targets. `--ctest-regex` and `--full-suite` are
exclusive; zero matching tests is an error. Default CMake configuration is
`Release`; `--build-type` also accepts `RelWithDebInfo` and `MinSizeRel`.
The configured cache is checked before building. Extra options use
`--cmake-arg=-DNAME=value`. On macOS the wrapper prepends Homebrew's bin
directory and supplies the Homebrew LLVM directory when present. Otherwise it
discovers LLVM through `llvm-config --cmakedir`. The Mini retains normal CMake
compiler selection. The Ubuntu profiles select `/usr/bin/gcc-14` and
`/usr/bin/g++-14`: Ubuntu 26's newer default GCC 15 and Clang 21 currently expose
warning-as-error failures in the compiler sources. No warning is suppressed.
`--cc`/`--cxx` override the profile defaults and can be saved by `configure`;
remote `CC`/`CXX` take precedence over those defaults, and explicit
`--cmake-arg=-DCMAKE_CXX_COMPILER=...` arguments are applied last. Custom steps
retain their exact toolchain arguments. Remote
Python is `/opt/homebrew/bin/python3` on the Mini and `/usr/bin/python3` on
Ubuntu (`--python` overrides it).
Global options precede the subcommand. `--repo` selects another worktree;
`--runner` takes precedence over `GTI_REMOTE_RUNNER` and the saved default.
`--host`/`--root`, or `GTI_REMOTE_HOST`/`GTI_REMOTE_ROOT`, override the selected
profile; `--transport ssh|tailscale` selects the client. Existing single-host
settings migrate on the next configure, preserving their endpoint, paths and
default. `configure` saves only that named host without changing the default;
`configure --default` changes it explicitly. Metadata updates are serialized so
concurrent configuration cannot lose a peer's profile. Custom names can be
registered with `--runner NAME --host USER@HOST` and the applicable paths/client.
Saved settings also retain the original flat connection fields for the selected
default, allowing older task worktrees to keep using the Mini during an update.
Saved host overrides take precedence over preset changes. The explicit Mini
configure command above updates an existing LAN-address configuration too.
Detached runs retain their recorded endpoint; configuring a new default does
not rewrite their connection records.

All runs use one window **per host**, not their isolated Git directories. The
Mini's is `/private/tmp/gti-remote-runner/window`; Ubuntu uses
`/tmp/gti-remote-runner/window`. The authoritative `build_window.Window` uses
two build slots on macOS, one on Linux, and one suite slot on each. This is a
server-side OS policy shared by all clients. The 13-GB VMs intentionally do not
stack two compiler builds. Configure/build take build slots; test steps take
suite slots. Mini builds default to half the CPUs; Ubuntu caps default build/
test workers as above. CTest uses the actual host CPU count for `--test-load` to
back off under concurrent load. `--jobs` and `--test-jobs` override command
parallelism, not slot counts. On a dedicated
Mini, `--jobs 12` can use its full 12-core build capacity. Agents should inspect
`status` before increasing parallelism. Manual heavy commands on a host must
use the **same directory and counts**; for the Mini:

```sh
GTI_BUILD_WINDOW_DIR=/private/tmp/gti-remote-runner/window python3 scripts/build_window.py --kind build -- cmake --build build-release -j 6
GTI_BUILD_WINDOW_DIR=/private/tmp/gti-remote-runner/window python3 scripts/build_window.py --kind suite -- ctest --test-dir build-release -j 12 --test-load 12
```

On Ubuntu, set `GTI_BUILD_WINDOW_DIR=/tmp/gti-remote-runner/window` and
`GTI_BUILD_WINDOW_BUILD_SLOTS=1`, keeping `GTI_BUILD_WINDOW_SUITE_SLOTS=1`.
Different hosts can independently hold their own build/suite slots.

The snapshot contains tracked working files and nonignored untracked files,
including edits and deletions. Ignored build trees and Git configuration/hooks
are not copied; `.env` and `.env.*` working files are excluded. **Committed
history is transferred in a Git bundle and is not redacted**: do not use this
runner with secrets in history or other nonignored source files. Symlinks,
submodules, special files and snapshots above 512 MiB are rejected rather than
silently building different sources. The runner detects edits during snapshot
creation; retry once edits settle. Evidence records original HEAD, working-tree
status, a hash of the exact snapshot, commands, elapsed time and exit status.
Submitting an uncommitted snapshot is useful during iteration but does not
replace the lifecycle's exact head/base validation before integration.

`run` normally waits and fetches automatically, returning the build/test exit
status. `--detach` prints the run ID immediately. The worker survives SSH/client
disconnection; reconnect with `status`/`fetch`. Ctrl-C requests cancellation.
Before submission, the client records the run's named host and full token-free
connection under `<git common dir>/gti-remote-runner/runs/<run-id>.json`.
`status`, `fetch`, `cancel` and `clean` for that ID use its recorded connection
even after the default, environment or saved profile changes. Conflicting
explicit connection options fail instead of contacting the wrong host. Legacy
IDs without a record still use the selected profile; select their original
host explicitly. Records remain local recovery metadata after fetching.
Cancellation and the default three-hour deadline (including window waits,
overridable with `--timeout` in seconds) stop the command process group before
releasing its slot and cleaning its files. Commands must not intentionally
daemonize into an unrelated process group; this is trusted build automation,
not a sandbox for arbitrary untrusted programs.

Results default to `<git common dir>/gti-remote-runner/results/<run-id>/`;
`--results <directory>` overrides the location. They include `report.json`,
`request.json`, command/worker logs and CTest's `Testing/` evidence. Logs are
bounded to 32 MiB total, at most 8 MiB per file, keeping the tail;
`truncation.json` identifies truncated files. Binaries are not exported. Fetch
validates/extracts locally **before** deleting remote evidence. A failed fetch
leaves remote results retrievable; it never overwrites existing local results.

Every run uses a marked directory below
the selected root's `runs/run-<32-hex-digits>`. Source, build, upload
and staging files are deleted **before completion**, on success, failure,
timeout and cancellation. Only a small result bundle, state and worker scripts
remain until fetching; fetching deletes the whole run directory. Terminal state
is published once, as the worker's final filesystem write, so fetching can safely
remove results immediately after observing completion. Unfetched
results expire after 24 hours. Each remote action sweeps only marked inactive
runs; after a crash/reboot, it also recovers abandoned uploads/workers after a
60-second launch grace period. Active workers/recorded children are never
swept. Crash recovery is serialized across agents. Cleanup is not a scheduled
background service: following a power loss or SIGKILL, the next runner command
performs recovery. Use `clean` to trigger that sweep; `clean <run-id>` explicitly
discards a finished run's results without fetching. It refuses active runs and
cleanup errors. Unmarked/pre-existing directories and other users' worktrees
are never deleted. Small shared connection/window metadata intentionally stays.

For non-CMake harnesses or custom multi-step validation, `--steps-file` replaces
the default configure/build/test sequence with a JSON array such as:

```json
[
  {"name":"configure","kind":"build","cwd":"source","argv":["cmake","-S","{source}","-B","{build}","-G","Ninja","-DCMAKE_BUILD_TYPE=Release","-DLLVM_DIR=/opt/homebrew/opt/llvm/lib/cmake/llvm"]},
  {"name":"build","kind":"build","cwd":"source","argv":["cmake","--build","{build}","--target","gti","--parallel","6"]},
  {"name":"fixture","kind":"suite","cwd":"source","argv":["python3","tests/my_fixture.py","{build}/gti"]}
]
```

These are argument arrays, not shell strings; `{source}` and `{build}` are
substituted with isolated absolute paths. Commands run in the indicated
directory. Custom plans remain responsible for optimized GTI compilers,
correct test inputs and evidence. Do not nest `agent_workflow verify` or another
`build_window` acquisition for the same slot inside a step: the wrapper already
holds that slot. To validate a published task through the normal PR lifecycle
without occupying a laptop suite slot, use:

```sh
python3 scripts/agent_workflow.py verify <task> --remote-steps-file /absolute/path/plan.json
```

Prefix that command with `GTI_REMOTE_RUNNER=ubuntu-6` or `ubuntu-8` to use a VM
without changing other agents' shared default. Each invocation submits one
plan to one host; split independent plans explicitly when using several hosts.

The helper submits that plan through the runner, retains results under its
local task metadata, and checks success and source identity before binding
validation to the clean published head and current upstream base. Remote HEAD
and Git status are recorded again after checks, before cleanup; a plan that
changes source cannot validate the published commit. Changes to
either head/base still invalidate evidence. Failed remote runs cannot authorize
integration. Publication still uses `agent_workflow finish` after reviewing the
retrieved evidence; the remote worker never publishes or installs.

## Integration and validation

The first branch push can contain no changes; draft creation waits for a real
commit because unchanged branches cannot form meaningful PRs. PRs are created
as GitHub drafts; creation verifies that state. `finish` applies the final
title, marks the PR ready for review and verifies the draft state cleared. An
interrupted PR-create request is
recovered by branch lookup before another create. Head and base repository identities must both be canonical GTI, so a
similarly named fork branch cannot be adopted or modified. Closed task branches
are not reused.

Checkpoint publication is distinct from validation. The agent selects suitable
checks using the owning skill and `verify-change`, with optimized compilers,
required examples and cross-phase evidence. Verification records are operational
evidence, not a security attestation or a replacement for code review. Each
record includes executed commands, head, base and time. Any changed commit or
dirty source requires verifying again; an advancing main does not, because the
merge queue validates the combination. Failed checks never mark the task ready.

`finish` hands the PR to a rollup (below) or registers native GitHub
`enablePullRequestAutoMerge` with `expectedHeadOid` equal to the validated head.
A branch that moved before registration is refused. GitHub owns the durable
wait for required checks/reviews and automatic entry to the required queue;
no agent polling loop, scheduled waiter or CI bypass is needed. `awaiting-ci`
means GitHub confirmed that intent, not that CI passed. `status` reports live
`remote_integration` (awaiting-ci, queued, not-requested, or head-changed) without
rewriting historical validation. Lost responses can be retried without duplicate
requests; a refused/unconfirmed request is an error, not completion.

The expected-head guard applies at registration. A later direct push by a
writer can retain GitHub auto-merge intent, so task authors must publish through
`checkpoint`/`reconcile`: before pushing a changed head they disable auto-merge,
dequeue if necessary, confirm cancellation, and invalidate previous verification.
Failure to confirm cancellation prevents publication. `--pr-only` also withdraws
intent. New work after a confirmed merge belongs to a new task. The `main` ruleset requires
pull requests, the merge queue and the `CI gate` status check, denies force
pushes and deletion, and has no bypass actors, administrators included. The
queue builds each candidate on the current `main`, runs the complete suite and
merges it only if `CI gate` passes; it removes a failing PR and rebuilds the
entries behind it. The last handoff record stays `rollup`, `awaiting-ci` or `queued` until refreshed;
GitHub owns the transition and failures/conflicts still need repair. Once GitHub reports
the merge, and `sync` (or a repeated `finish`) then advances local main. Do not force a merge
or bypass a server rule. Release workflows run after integration and need not
finish before local installation. VERSION follows normal repository policy;
workflow-only automation does not change the packaged compiler version.

The library sanitizer jobs partition `cpp_mir_body_emitter_lifecycle` through
its existing `--list` / `--test NAME` protocol. Each job runs half of the sorted
named-case inventory serially under the unchanged 5,400-second aggregate bound;
native integration still runs the ordinary aggregate CTest. The sanitizer jobs
retain per-case outcomes and durations. The final gate requires both reports
for its exact candidate, identical inventories, and every named case passing
exactly once. Missing, stale, failed or timed-out case evidence fails the gate.
Compiler sanitizer jobs build only the targets of `compiler_pipeline`; library
jobs retain the library subset targets. This avoids filling runner disks with
unexecuted sanitizer library binaries in compiler jobs.
This avoids assigning the entire lifecycle suite to one library shard without
removing sanitizer coverage or increasing timeouts. Runner termination still
fails integration; this scheduling change does not claim to repair host loss.

## Risk-based batch validation

The risk tiers below select how broad a change's focused checks must be. The
batch commands that follow are the degraded-mode path: in normal operation each
PR goes through the merge queue, which runs the complete suite.

Per-change validation establishes that a patch fixes its motivating problem
and preserves the affected rules. Integration validation establishes that the
selected patches work together. Neither an ICE symptom nor a short diff is a
low-risk classification. The plan names the invariant and its affected users.

| Tier | Changed invariant | Component evidence before admission |
| --- | --- | --- |
| T1 | Confined recovery, diagnostic or otherwise narrow behavior | Meaningful regression, owning tests, affected cross-phase checks, compiler smoke |
| T2 | Shared semantic, generic, ownership or lowering behavior | T1 plus relevant subsystem suites and supported boundaries |
| T3 | System, layout/ABI/runtime, lifecycle or optimizer invariants | T2 plus explicit platform, runtime/performance and other applicable acceptance gates |

For an independent isolated change, the affected invariant and consumer graph
determine completion coverage, not patch size or tier alone. An isolated LLVM
lowering slice that leaves frontend semantics, MIR/LoweredProgram contracts and
C++ emission unchanged can complete with its backend execution/rejection,
dependency, driver, ABI, installed-consumer and required platform/performance
checks. It does not automatically require unrelated C++ runtime matrices or
an all-target build/full CTest run. Record the untouched boundaries and why the
selected gates cover the change. Shared-contract or cross-cutting changes need
broader consumer coverage; full integration/release campaigns and the combined
batch protocol below retain their explicit full-suite requirements. This is
test selection, not permission to skip an affected, failed or mandatory gate.

[`tests/ownership.toml`](../../tests/ownership.toml) is the shared source for
this selection. It maps source paths to their owning (T1) and consumer (T2)
CTest tests and marks `shared` and `system` risk; `system` areas (ABI/layout,
runtime services and allocation, CMake/toolchain, CI and release workflows,
test infrastructure) validate alone and are never combined with other
changes. It is the one source the planner, PR CI and reviewers share: the
verify-change planner selects from it, CI's workflow regression job
validates it, and reviewers check a change's evidence against it. Its
selection is a lower bound that the author extends from the changed
invariant. `tests/ownership_map_test.py` keeps it complete: every tracked
source file has an area or a stated reason, and the `test_ownership_map`
CTest requires every named test to be registered.

The compiler-smoke execution covers scalar control flow and owning strings in
a vector at O0/O3. The GTI executable itself is always optimized. Each plan
also includes `compiler_pipeline`. Shared semantic/IR/representation/runtime
paths have a script-enforced T2 floor; analysis may always raise the tier.
Tier selection is reviewable policy, not a proof inferred from filenames.

A regression should fail for the intended reason on a known-bad revision,
pass with the patch, and pass on the combined candidate. Preserve the original
command, source identity, failure and log. When historical binaries are
unavailable or reproduction would be unsafe, state the concrete reason and
retain existing evidence. Use time/memory bounds for exhaustion cases; never
reproduce an OOM without a guard. Code-generation fixes also check execution,
cleanup and relevant source rejections rather than only disappearance of an
ICE. Existing safety, platform and runtime-performance requirements still apply.

### Component admission

Publish the task as a draft PR, reconcile current main, and configure a separate
optimized CMake tree. Write a temporary JSON plan, for example:

```json
{
  "tier": "T2",
  "rationale": "Preserve cleanup publication for this newly supported MIR shape",
  "regressions": ["owning_regression"],
  "owning": ["owning_suite"],
  "cross_phase": ["related_runtime"],
  "broader": ["cpp_mir_body_emitter_lifecycle"],
  "targets": ["gti_owning_tests", "gti_cpp_mir_body_emitter_lifecycle_tests"],
  "platforms": ["darwin"],
  "reproduction": {
    "revision": "0123456789012345678901234567890123456789",
    "command": ["old-gti", "repro.gti", "--emit-cpp"],
    "failure": "GTI-B0001 at the original conditional receiver",
    "log": "/absolute/path/original-failure.log"
  }
}
```

These test/target names illustrate the schema; select real exact CTest names.
`cross_phase` may be empty only with a nonempty `unaffected` explanation.
T2/T3 require `broader`. `reproduction` may instead contain exactly one
`exception` explanation. Optional `additional_checks` is a list of argv arrays
for the applicable non-CTest tooling, native/platform or performance gates.
Optional `platform_tests` maps declared platforms to test names that only apply
on those hosts (a name appearing there is scoped even when also in `regressions`).
`platform_checks` similarly maps platforms to extra argv commands. This keeps,
for example, mandatory macOS native tests out of Linux's required inventory
without weakening the macOS obligation. Admission itself must run a regression
on a declared platform. All unscoped tests/checks remain required everywhere.
Do not include credentials. Every declared gate is executed; plans are not
permission to omit required checks. `platforms` lists hosts on which the
combined full campaign must pass; CI's Linux native/sanitizer jobs are additional
server gates and do not substitute for required macOS evidence.

```sh
python3 scripts/agent_workflow.py batch enroll my-fix \
  --plan /tmp/my-fix-checks.json --build /absolute/optimized-tree
python3 scripts/agent_workflow.py batch status
```

Admission rebuilds the selected targets plus `gti`/runtime, checks the source
directory and host optimization in CMakeCache, rejects missing/disabled tests,
runs the exact selection and smoke, and rejects skipped required results. T1
selections run directly; T2/T3 subsystem coverage uses the shared suite window.
Evidence is retained under the common Git directory's
`gti-agent-workflow/batch-evidence/<task>/<head>/`, including logs and JUnit.
The admission binds head/base, plan and platform requirements. The original
failure log is represented by its SHA256 in the portable plan, not uploaded.

The result is **ready for integration**, not fully verified, merged or released.
An attached PR-body marker makes CI report incomplete integration even if
someone marks it ready manually; the helper independently refuses direct
component integration. A changed source must be reconciled, published and admitted
again. `batch withdraw <task> --reason <text>` clears an unassigned admission
for normal independent validation; it does not authorize a partial merge.
Withdrawal requires a draft PR so marking it ready starts independent CI.

### One bounded combined campaign

A designated integrator selects one to eight admitted task names explicitly.
Collect ready work for at most ten minutes, visible in `batch status`; start
earlier when useful, including a single ready fix. Do not wait for unfinished
features or add unadmitted PRs. One active batch at a time prevents agents
starting duplicate combined campaigns. Other agents keep implementing and
performing focused checks while that batch runs.

```sh
python3 scripts/agent_workflow.py batch prepare fixes-oct23 \
  --tasks first-fix second-fix --worktree /absolute/new-candidate
python3 scripts/agent_workflow.py batch verify fixes-oct23 \
  --build /absolute/candidate-release-tree --jobs 4
python3 scripts/agent_workflow.py finish fixes-oct23 --body-file /tmp/batch-pr.md
```

Prepare creates a normal task branch/PR and merges exact original commits.
`.gti-validation/batches/<batch>.json` records the starting main, each original
task/PR/head, and its plan. Records already integrated into main are immutable.
Member conflicts stop with the candidate and original branches preserved;
review the invariant, resolve and commit, then `batch resume <batch>`. The
membership cannot silently move to a newer PR head. A member's unpublished
commits also prevent selection.

Full verification rebuilds the whole optimized candidate, executes every
registered CTest without a selection filter, requires every component's exact
tests and smoke to be present and passed, and executes additional gates once
per distinct argv command. Nonrequired platform skips remain explicit evidence;
they are never reported as passes. A focused or incomplete report cannot stand
in for a full result. Reports are bound to the clean published head and main
base; all declared platforms must have evidence for that same pair. Retain an
old run if main advances, inspect the delta, reconcile near the acceptance
boundary and obtain the affected/required fresh evidence. Never relabel the
old run or repeatedly restart it while it is still running.

Remote verification uses `batch verify <batch> --remote-steps-file <plan.json>`
with the normal remote runner. Configure its separate optimized build first
in that plan. Include a source-directory suite step whose argv is
`["python3", "scripts/validation_checks.py", "full", "--build", "{build}",
"--base", "<exact-current-main-sha>"]`. The remote snapshot carries Git history
but no `origin/main` ref, so the explicit base SHA is required. The worker owns
the remote shared window; its complete source identity and structured full
result are checked before platform evidence is recorded. No installation occurs.

When a combined test fails, retain the report and identify the patch owning
the violated invariant. That owner repairs it; another agent may help after
establishing intended behavior, not by weakening unrelated expectations. To
remove a suspect patch, publish the current source, use
`batch release <batch> --reason <text>` and prepare a new explicit selection.
Release closes the unsuccessful batch while preserving its branch/worktree
and returns its members to the queue. Do not erase conflict work to release it.

Normal `finish` refuses component admissions and requires full platform evidence
for batches; the combined PR then still passes through the merge queue. An
umbrella merge preserves all original commits. GitHub need not mark included
PRs merged:
component `sync` verifies the committed membership manifest, exact original
head/admission, every required platform's accepted combined proof, the umbrella
PR's real merged state and both heads' main reachability. Only then it closes
the original PR with an explicit umbrella/merge link, recording
`integrated_via_batch` without pretending its remote state is merged. Normal
cleanup repeats that proof. Heuristic merged detection and manual-merge
overrides are not enabled to fake a provider state. The primary checkout
and release policy remain the normal completion boundary; there is no backup push.

### Required CI and the merge queue

Owner policy revision (2026-10-29): CI is the required integration gate, and
contributors no longer run the complete suite. This supersedes the 2026-10-23
advisory-CI policy. `.github/workflows/ci.yml` reports one required check,
`CI gate`, from a job that depends on every other job:

| Event | Mode | Jobs the gate requires |
| --- | --- | --- |
| `pull_request` (ready) | full | lint (formatting, release version, Tree-sitter), workflow regressions, and `fast` on macOS arm64 and Linux x64 (`feedback-fast`, `compiler_pipeline` and the PR's declared T0/T1 tests) |
| `pull_request` (draft) | draft | lint and workflow regressions only; a draft cannot enter the queue |
| `merge_group` | full | lint, workflow regressions, system-LLVM `fast` checks, production packaging and installed checks, every bundled-release `integration` shard on both platforms, and `sanitizers` |
| `workflow_dispatch` on main | full | the same full integration and production packaging gates, for release commissioning/recovery |
| PR or merge group | automation | lint and workflow regressions only |
| PR or merge group | component | always fails: a batch component cannot merge directly |

`validation_checks.py ci-mode` classifies the candidate from its diff. A
conservative automation-only diff (docs and the workflow scripts with their own
tests) runs the inexpensive gates; CMake, compiler tests, runtime, packaged
scripts, release packaging, integration/build workflows and unknown paths select full
coverage, so a change to an integration job is itself validated by it. Only the
explicit orchestration allowlist (`rollup.yml`, `train.yml`, `backport.yml`,
`rollback.yml`) can use automation mode. Their lint and workflow regressions
remain mandatory on both PRs and merge groups. Changes to validation routing,
batch selection or named-test sharding always select full coverage, even when
combined with an allowed workflow. Mixed candidates inherit full coverage from
any non-automation path; adding a new workflow does not automatically exempt it. The
classification step binds `github.event_path` to `GTI_EVENT_PATH` and rejects
missing or non-file payloads before running the classifier.

In full integration, production packaging builds the pinned bundled LLVM/Clang
release configuration on both platforms and validates the staged toolchain
before archiving it. The integration shards reuse its object cache, with the
same bundled release configuration; the cache never substitutes for validation.
`validation_checks.py shard` builds the optimized tree, takes the
complete CTest inventory, and runs its share of a deterministic partition:
longest first onto the lightest shard, weighted by `tests/ci_test_durations.json`
(slowest observed seconds; unknown tests take the default). The first shard on
each platform also runs the native smoke, and the first Linux shard checks
deterministic docgen. The system-LLVM `fast` checks also run in full
integration; their Linux job retains the installed-library and process-resource
consumer checks against system LLVM. Production packages independently run
their installed-consumer checks against bundled LLVM. Each shard uploads a report
of its head, tree, inventory and executed tests. The gate merges them and fails
unless, for each required platform, the shards are complete, disjoint, agree
on source and inventory, ran every registered test exactly once and include the
smoke, and both platforms validated the same source. Refresh the duration table
from queue JUnit reports when shard wall times drift apart.

Nightly consumes these CI-produced archives; it does not repeat compilation or
the full suite. Publication binds the complete artifact set to the successful
full CI run and attempt, exact source, platform, configuration and file hashes.
The main-only full CI dispatch provides the same evidence for commissioning or
recovery, including a main commit admitted under an explicit merge exception.
The exception itself is not release evidence. Automation-only passes produce
no production archives, and missing or expired artifacts require a new full
run rather than reuse of older binaries under a new source identity. See
[GitHub nightly publication](build-and-driver.md#github-nightly-publication).

Shards run with every test timeout scaled three times
(`-DGTI_TEST_TIMEOUT_SCALE=3`): hosted runners have 3-4 cores, against the
8-12 core hosts the bounds were measured on. A test that still times out is too
slow for its bound, not flaky.

`tests/ci_quarantine.json` lists tests that already fail on `main` and that the
queue excludes on the named platforms until their linked issue is fixed. Every
entry names exactly one registered test, its platforms, a
`lang-gti-official/gti` issue, the reason and the date. The smoke cannot be
quarantined, and every shard of a platform must apply the same exclusions. The
gate prints the exclusions it applied. Adding an entry is a reviewed change
that runs the complete suite, and it is only for a failure reproduced on a clean
`main` build, never for a regression a pull request introduces. The fix for a
quarantined test removes its entry in the same change.

A ready, full-mode pull request declares its evidence in a `Validation:` block
in its body. The `T0` (reproducer or new regression) and `T1` (owning tests)
items list CTest names only; other items are prose for reviewers.
`validation_checks.py declared` parses the block: the plan job rejects a missing
or malformed declaration, and the `fast` job re-runs the declared tests by exact
name on both platforms. Each one must be registered and enabled. The body is
untrusted input, so a name must match the CTest name grammar and reaches CTest
only as an escaped, anchored pattern. `finish` enforces the same declaration
locally and refuses a declared test that the recorded `verify` checks did not
select. A test counts as selected when a `ctest -R` or `--tests-regex` pattern
matches it or a focused validation plan names it. The re-run guards against a
claimed but unrun or host-specific result; the merge queue still runs the
complete suite.

A queue failure belongs to the change that introduced it. The queue removes
that PR and rebuilds the entries behind it, so other work keeps merging; the
owner reproduces the failing tests locally, repairs the change without
weakening unrelated expectations, verifies and re-queues. The Linux
workflow-regression job installs CMake (including CTest) and Make explicitly
before its real, disposable CMake/CTest batch fixtures run. `CIEnvironment` in
`agent_workflow` executes these shell steps with missing event aliases and an
initially tool-free PATH. The lightweight job also runs `repository_hosting`,
the ownership-map coverage check and the planner's tests.

### Rollups

A complete queue run is long and hosted macOS concurrency is small, so the
queue's group size is one and rollups combine compatible work instead.
`scripts/rollup.py`, run by `.github/workflows/rollup.yml` every 30 minutes,
on dispatch, and on an exact `/rollup` comment from a writer, composes several
green PRs into one queue entry. Only `main`'s code runs; PR content is never
checked out or executed, and merges are Git plumbing with hooks disabled.

| Label | Meaning |
| --- | --- |
| `rollup=maybe` | Default for a ready PR that is not Risk-system |
| `rollup=always` | Automation-only (docs and workflow scripts with their tests) |
| `rollup=never` | Validates alone; `finish` sets it for Risk-system diffs and `--no-rollup`, and on every rollup PR |
| `p=N` | Priority; higher goes first |
| `rollup-failed` | Bot-managed: the PR was in a failed rollup and validates alone |
| `needs-rebase` | Bot-managed: the PR conflicts with `main`; cleared once it merges cleanly |

Risk-system is a conservative path rule (`validation_checks.system_risk`),
applied to the diff and to every path any member commit touched: CMake and
toolchain files, `.github/workflows/`, `tests/CMakeLists.txt`,
`tests/TestThroughput.cmake`, `scripts/configure_ci_*`, `runtime/include/gti/`,
`runtime/src/atomic_lane.h`, `include/gti/object_layout.h` and the LLVM ABI,
layout and runtime-ABI sources. It will later read the `risk = "system"`
areas of the test ownership map.

`compose` takes open, ready PRs on `main` labelled `rollup=maybe` or
`rollup=always` whose `CI gate` passed on their current head, excluding queued
PRs, members of an open rollup, and `rollup=never` or `rollup-failed` PRs. It
orders them by priority, then the time their gate passed, then number, and
merges each exact head onto current `main` with `--no-ff`, up to six members. A
PR that conflicts with `main` is skipped and labelled `needs-rebase` with a
comment; one that conflicts only with an earlier member waits for a later
rollup. The bot never resolves conflicts. With fewer than two members it does
nothing. Otherwise it commits `.gti-validation/batches/rollup-<date>-<n>.json`
(the batch manifest schema that `validation_checks.py full` checks) and opens
`Rollup of N pull requests`, labelled `rollup=never`, listing each member and
carrying a `Validation:` block whose T0/T1 items are the deduplicated union of
the members' declared tests, at most 60. Members are not batch admissions, so
each manifest plan names only the member's declared T0/T1 tests (the compiler
smoke when it declared none), records an undeclared tier as T2 and points to
the member's PR for reproduction; it claims no other evidence. A rollup
manifest does not change the CI mode its members' paths select.

`enqueue` queues a rollup once its own `CI gate` passed, pinned to its head.
`reconcile` handles the verdict. When a rollup merged, it closes every member
whose head is now reachable from `main` with a comment that records the proof
`sync` checks (a merged canonical rollup PR whose head contains the member's
exact head, with both reachable from `main`); a member with newer commits stays
open with a note. When the rollup failed (its merge-queue `CI gate` or its PR
`CI gate` failed), it labels every member `rollup-failed` with a link, queues
each one alone so the queue isolates the culprit, then closes the rollup and
deletes its branch. A queue drop without a `CI gate` verdict counts as
infrastructure and is re-queued once per rollup. A member that moves or closes
before its rollup is queued makes the rollup stale: the bot closes it and the
members return to the pool. Close a rollup PR to stop it. Bisection of a
failed rollup is future work. Every command takes `--dry-run`. The bot writes
only with its GitHub App installation token; see `.github/README.md`.

Automation-only and source-only GTI stdlib local owning checks may use
`verify --focused`, which checks the actual diff and does not reserve a
broad-suite slot. The source-library path requires a change under `stdlib/std/`
ending in `.gti`; accompanying GTI fixtures/examples,
Python test harnesses, explicit surface/census metadata, release metadata and
Markdown documentation are permitted. Compiler, native runtime, CMake and
other implementation changes cannot use that path. Its checks require a named
`tests/*_test.py` harness or CTest with one anchored exact test name, such as
`-R '^encoding_runtime$'`. Obvious native builds, shell wrappers, orchestration,
full corpus scripts and unfiltered CTest are refused. This is a conservative
selection guard, not a proof of a Python harness's workload or adequate coverage:
the agent must review actual scope, use an optimized compiler, and retain owning
positive/negative, affected consumer and performance evidence. Broad workloads
still use the shared window. An active `verify` is atomically refused on
duplicate submission; resume the recorded operation instead.

Degraded mode: if GitHub Actions is unavailable for more than four hours, a
designated integrator may validate one combined candidate with `batch verify`
on a remote runner and record that evidence on the PR. Agents never fall back
to per-PR local full suites. Do not reintroduce advisory CI or administratively
bypass a server rejection.

## Early combined preview

`origin/preview` is the default early source branch for completed agent work while
their original PRs continue full validation and review. Preview admission is
separate from `verify`/`finish`: it never writes task validation records, marks
original PRs ready, or grants permission to merge them. Every original PR still
satisfies the existing main gates. Never merge the preview branch wholesale
into main; `finish` and the CI release-version gate reject its `.gti-preview.json`
marker and marker history, including candidates that later remove it. Stable/nightly source
resolution rejects that marker too. Preview publication creates no release,
tag, package, or local installation.

The candidate combines the currently published preview, exact selected PR
head, and current main in a new external worktree. Its committed
`.gti-preview.json` records all included task/PR/head identities, their merge
commits, exact focused CTest names, main and previous-preview identities. Admission
requires explicit task selection; it does not sweep open PRs. Each agent must submit its own completed task after focused validation passes
and its PR is published, unless the user requests no preview or no publication.
This includes fixes and automation tasks, not only new language features.
No per-task permission is needed. The caller must select appropriate focused checks. Draft PRs are allowed
because their full acceptance may still be running.

```sh
# Select the owning regression tests by their exact CTest names.
python3 scripts/agent_workflow.py preview submit my-task --tests cpp_mir_body_emitter_lifecycle
```

This is the only normal agent-facing preview action. It returns a submission
ID and log path after launching a detached worker; it does **not** wait for
builds, tests or publication. Continue ordinary `verify`, `finish` and cleanup
without waiting for preview. A failed, conflicting, stale or interrupted
preview job never changes the original task's acceptance records or PR state.
Omitting `--tests` on resubmission reuses that task's last explicit selection;
the first submission requires focused names. An identical live submission is
deduplicated. A different live submission for the same task is refused, not
silently replaced. `preview status [SUBMISSION]` reports progress, or lists
submissions without contacting the remote build host. Submission success means
queued, not preview-validated or published.

The worker copies its small tooling closure outside the task worktree so normal
task cleanup is safe. A separate single-slot preview semaphore serializes
admissions; it consumes no ordinary build slot while waiting. Once selected,
the existing remote runner uses the configured/default host (or
`GTI_REMOTE_RUNNER`) and its LLVM/compiler/job defaults, an optimized Release
build, and `preview_admission`. That aggregate CTest is registered **only** in
combined candidates carrying `.gti-preview.json`; it invokes the existing
example gate with every manifest feature's exact focused checks. No aggregate
gate is registered in ordinary main/PR suites. Source compiler and project
CLI behavior are unchanged.

The submitted PR head is pinned: a closed/advanced PR becomes superseded, never
an implicit request to admit a different commit. Main/preview advancement gets
one fresh preparation and revalidation attempt, then a separate stale result.
Re-submit later if preview is still useful. Conflicts and failed candidates
retain their checkout and log for optional recovery; they require no action
to finish the original PR. On successful publication the worker removes only
its unchanged, clean checkout, retaining checkpoint refs and evidence. Modified
or ignored files are preserved. An exited worker is reported as interrupted;
inspect its retained log and any remote run before resubmitting rather than
automatically launching a duplicate uncertain run.

The lower-level `prepare`, `verify`, `publish`, `seed`, `resume` and `revert`
commands remain for maintainer recovery, not mandatory feature-delivery steps.
For example, `preview prepare my-task --worktree /absolute/new-candidate
--tests owning_test`, followed by `preview verify CANDIDATE` and
`preview publish CANDIDATE`, still supports explicit local validation settings.

`verify` configures and builds an optimized compiler from the combined source,
reruns the focused checks of **every included feature**, and runs the fixed
`preview_examples.py` gate. This covers the numbered corpus at O0/C++20 and
O3/C++23, practical HTTP examples and established automated
project/native harnesses. Missing, disabled or skipped required tests fail
admission, including focused tests. One-off checks must be registered with
CTest first; focused names are deduplicated for execution while their owning
PR identities remain in the manifest. New nested example directories require an explicit coverage update;
the allocator helper is exercised by its numbered client. Objective-C++ execution is required on macOS and explicitly reported
as inapplicable elsewhere. Interactive `toy-3d-engine` needs its native
dependencies and a graphics session; this automated gate does not certify GUI
execution. Preview is therefore evidence for the automated runnable examples
on the recorded host, not a claim that every interactive program or every
platform has run. No main validation requirement is reduced by this scope.
With no pending feature checks, the build targets are `gti`, `gti_runtime`
and `gti_lsp`, which cover the fixed example harness dependencies. Candidates
with focused checks build all targets because arbitrary registered CTest names
can require additional test executables; neither path reduces gate coverage.

Use `GTI_REMOTE_RUNNER=mini ... preview verify CANDIDATE --remote` (or a named
Linux host) to run the same generated argv plan through the remote runner.
Additional CMake settings use repeated `--cmake-arg=...`; `--jobs N` controls
the build. Local and remote execution both use the shared build windows. Logs
and exact-head evidence live under the common Git directory's
`gti-agent-workflow/preview/<candidate>-results-<attempt>` (a fresh destination
for every attempt, preserving earlier evidence); `preview status CANDIDATE`
reports the token-free recovery record. Candidate branches are published as
`codex/preview-<candidate>` and retained for recovery. No candidate PR is opened.

Publication accepts only the unchanged, clean, tested candidate, and refuses
an advanced main or preview. An ancestry check plus an exact remote
lease makes preview updates append-only and detects concurrent publishers.
The lease is also compare-and-create for the first publication; it never
authorizes replacing published history. Lost push responses can be retried.
If either input advances, prepare a fresh candidate and repeat its checks.

`preview seed --worktree /absolute/new-candidate` starts preview from main
without adding a PR, or refreshes an existing preview with main. It requires
the same `verify` and `publish` steps; an absent preview branch remains absent
until these pass. `preview revert my-task --worktree /absolute/new-candidate`
constructs ordinary revert commits, retains the original PR, and requires the
same combined checks before publication. Re-admitting a removed task restores
its prior revert before merging the newly selected head. Merge/revert conflicts
remain in the candidate worktree: resolve and commit them, then run
`preview resume CANDIDATE` to finish preparation before verification. The
manifest and evidence always describe the resulting combined source.
Admission refuses an already-reachable PR head (except explicitly restoring a
removed task). Removal refuses a feature already integrated into main; changes
to accepted main behavior require their own normal PR.
If a previously removed feature later merges into main, the next seed,
admission or removal restores its recorded contributions before merging main.
This matters because Git otherwise retains the preview revert even though the
original feature head is now an ancestor of main. The inclusion record marks
the restoration as accepted by that exact main commit; it does not reclassify
accepted work as a pending preview feature. Conflicts still require explicit
resolution and the combined candidate must pass the normal preview gates.

Low-level/failed preview candidates and source installations are retained intentionally; do not
delete them while another user may rely on their commits or installed paths.
No automatic pruning/resetting of modified worktrees is performed.

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

A task can have no work at all: no PR, and local, remote and worktree tips
already contained in `origin/main`. GitHub refuses a PR for such a branch, so
`abandon` retires it directly instead.
- Every check runs before the first removal. Uncommitted changes or ignored
  files stop it, and primary `main` must contain the local branch.
- It removes the worktree without force, deletes the remote branch with a
  compare-and-delete lease and deletes the local branch with `git branch -d`.
- It records the reason. A retry after an interruption skips the pieces already
  removed.
- Any unique commit, published or not, keeps the checkpoint-and-close path.

Interrupted operations are retried using the same task. Task records survive
merge rejection and sync failure. A confirmed remote merge resumes at sync;
cleanup tolerates branches already deleted. If records are lost, preserve and
adopt the published branch/worktree, rediscover the PR, and confirm the remote
merge before proceeding. Recovery adoption accepts an existing open/ready PR
even when main advanced, allowing helper reconciliation without first requiring
an unknown task to reconcile itself. Already merged tasks resume sync; lost
abandonment responses are confirmed remotely and do not republish closed work.
Server API rejections are reported without secrets.
