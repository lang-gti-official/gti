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

GitHub (`lang-gti-official/gti`) owns branches, pull requests, issues, CI and
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
retains ordinary SSH and its existing keys/host verification. Neither transport
disables host-key checks or changes SSH configuration.

Three named presets are available; saved connection overrides live only under
the Git common directory:

| Runner | Connection | OS / declared capacity | Default build / test workers |
| --- | --- | --- | --- |
| `mini` | `lukeh@192.168.0.53` | macOS; observed host CPU count | half CPUs / all CPUs |
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
python3 scripts/remote_runner.py --runner mini configure
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
remain until fetching; fetching deletes the whole run directory. Unfetched
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
