---
name: agent-workflow
description: Carry GTI implementation tasks through isolated branches, published checkpoints, validated GitHub PR integration, local-main synchronization, and safe worktree cleanup. Use for repository changes and resuming unfinished implementation tasks; read-only discussions and audits do not create branches or PRs.
---

# GTI Agent Workflow

The normal implementation outcome is a merged GitHub PR and a primary checkout
on current `main`, ready for the user's local installer. The repository owner's
standing authorization is in `AGENTS.md`. Explicit instructions such as
"PR only", "leave local", "do not merge", or a release/install request override
the default. Never infer permission to change other repositories or services.

Use `scripts/agent_workflow.py` for shared Git/API operations. GitHub
(`lang-gti-official/gti`, remote `origin`) is the canonical host for branches,
PRs, issues, CI and releases; the self-hosted Gitea repository (remote
`gitea`) is a backup mirror that `sync` updates and that never receives PRs or
issues. The helper authenticates with the GitHub CLI (`gh auth token`) or
`GTI_GITHUB_TOKEN`/`GH_TOKEN`/`GITHUB_TOKEN`, including from the primary
checkout's ignored `.env`, and keeps token-free recovery records in the common
Git directory. Run it from any checkout of the repository, or pass
`--repo <checkout>`; it always acts on the primary checkout it shares a Git
directory with. Do not copy credentials into worktrees. Task records from
before the GitHub move show as `legacy-gitea` in `status`; re-register such a
worktree with `start --adopt`. Read
[the command contract](../../../docs/architecture/agent-workflow.md) when
recovering interruptions or interpreting a refused operation.

## Execution discipline

Apply the [bounded-output, analysis and waiting contract](../../../docs/architecture/agent-workflow.md#bounded-output-analysis-and-waiting).
It protects thorough design review and all necessary validation; it does not
impose reasoning-depth or reconsideration limits.

- Before long checks, keep a compact task ledger: acceptance obligations,
  decisions/open questions, exact head/base/compiler/platform identities,
  commands/results/log paths, and live session/run IDs. Keep it in existing
  task notes/PR evidence or temporary state, not a new canonical design document.
- Save raw execution evidence under the normal retention contract and return
  bounded summaries/excerpts. Use `scripts/bounded_log.py` for saved tails;
  budget the whole tool response, not just each nested call. Required skill
  instructions must still be read fully; select relevant canonical sections.
- Resume the same live operation with interruptible waits. Back off remote
  status polling when unchanged; do not repeatedly read logs, Git state or
  manuals merely to occupy a queue. Useful independent review remains welcome.
- On resumption/compaction, recover the ledger and pending handles before
  launching work. Preserve completed evidence rather than restarting audits.
  Explain material cost/schedule growth without dropping required work.

## Start and publish

1. Inspect status; preserve other tasks' work. Fetch current `origin/main`.
   Give each task its own `codex/` branch and worktree. Never implement on
   primary `main`, reuse another task's branch, or reset a checkout.
2. Prefer the app's managed worktree creation tool, based on `origin/main`.
   Switch its clean checkout to the new task branch, then register it:

   ```sh
   python3 scripts/agent_workflow.py start <task> --title '<outcome>' \
     --worktree <absolute-path> --branch codex/<task> --adopt
   ```

   Without `--adopt`, the helper creates a fresh Git worktree at an unused
   explicit path. Start pushes the branch immediately. The first meaningful
   checkpoint creates a draft PR; do not manufacture empty commits or one-off
   plan files for this purpose. Attach the PR to the chat with the app tool
   when supported, and always retain its ordinary URL.
3. Commit and push coherent checkpoints, before a pause or handoff, and at
   completion. Stage explicit files, review the diff, then use `checkpoint`.
   It can commit `--message <text> --paths <files...>`, or publish an already
   committed clean checkpoint. Include work that belongs to this task only.
   An unfinished checkpoint can be published without claiming it is validated.

## Validate and integrate

Use the owning feature skill and `verify-change` to select real checks with an
optimized GTI compiler. Provide required runnable examples and their inventory
updates for language changes. Use `finish-release` for VERSION decisions.

For concurrent fixes, use the canonical
[risk-based batch contract](../../../docs/architecture/agent-workflow.md#risk-based-batch-validation):

- Enroll a completed published draft with a reviewed T1/T2/T3 plan, original
  regression evidence, exact owning/cross-phase tests, appropriate broader
  checks, targets and platform obligations. `batch enroll` rebuilds the
  optimized compiler, runs those checks plus smoke, and records **ready for
  integration**. It does not authorize a direct merge.
- The designated integrator selects at most eight ready tasks explicitly with
  `batch prepare`. Keep collection to at most ten minutes; never wait for an
  unfinished feature. A single ready fix may proceed alone. `batch status`
  exposes queued age and the one active batch. Do not sweep unadmitted PRs in.
- Resolve source conflicts in the candidate, preserving original commits,
  then `batch resume`. Run `batch verify` on the combined source for each
  required platform. This executes the full suite and all members' required
  tests and rejects missing/skipped regressions. Use the ordinary `finish`
  only after full evidence and CI admission pass. Never merge `preview`.
- Diagnose combined failures before changing tests. The owner of the violated
  invariant repairs its patch, or the integrator releases the batch with a
  reason and prepares a new explicit selection. Source heads cannot silently
  change inside a running batch. Preserve each attempt's evidence.
- `batch withdraw` returns an unassigned component to independent validation;
  `batch release` closes a published unsuccessful batch while preserving its
  work and returning its members to the queue. Normal sync/cleanup still
  applies to all confirmed integrated original PRs after the umbrella merge.

Draft/component CI intentionally blocks direct integration and skips full
native builds. Mark independent/candidate PRs ready to start their final CI
campaign. The required `Validation gate` permits no compiler merge until
full native and sanitizer jobs pass; automation-only diffs use their tested
workflow path. Retrying `finish` with an unchanged description does not restart
CI. Activate the published gate with `protection --configure --batch-gate`,
preserving existing requirements as authorized in `AGENTS.md`.

1. Run `reconcile <task>` to merge current `origin/main` into the task branch
   and push. Resolve any conflicts in this worktree; do not force-push.
2. Put the proportionate final commands in a temporary JSON file containing
   argv arrays, then run `verify <task> --checks-file <file>`. For example:

   ```json
   [["git", "diff", "--check"],
    ["ctest", "--test-dir", "build-release", "--output-on-failure", "-R", "^owning_gate$"]]
   ```

   Build necessary targets first, through a build-window slot:
   `python3 scripts/build_window.py --kind build -- cmake --build <tree> ...`.
   The helper executes these commands inside a build-window `suite` slot and
   holds its metadata lock only while recording, so other sessions can still
   start, checkpoint and inspect tasks while it waits for or runs validation;
   `python3 scripts/build_window.py --status` shows who holds the window. It
   binds passing evidence to the clean, published head and current remote
   base. It does not select adequate tests or prove their quality; that
   remains the agent's responsibility. Changes or upstream advancement require
   revalidation. If main advances during a run, retain that run's exact-source
   results, inspect the delta once the run finishes, and select fresh checks
   for the reconciled head/base with `verify-change`. Do not automatically
   restart a full suite on every status poll, or relabel earlier results as
   current. Required/affected broad and platform gates remain mandatory.
3. Write the final PR description to a temporary file, including behaviour,
   validation and limitations. Run `finish <task> --body-file <file>`.
   It marks the PR ready, checks validation and branch protection, merges through
   GitHub with the exact tested head, then attempts a local-main fast-forward
   and pushes `main` and the tags to the `gitea` mirror remote.
   Use `--pr-only` when the user requested review before integration.
4. Required server checks/reviews are respected; never force or administratively
   bypass a rejection. Wait using the product's wait/continuation facilities
   when needed, then retry after the cause changes. Do not silently downgrade
   the outcome to "PR opened". Packaging/nightly/stable jobs after integration
   do not block completion or the user's local installation.

`protection` audits server enforcement. `protection --configure` installs the
owner-approved `main` rule denying direct/force pushes and bypasses and requiring
an up-to-date branch. It preserves existing review/status-check requirements.
Changing those requirements requires separate authorization.

## Sync, cleanup and recovery

`sync <task>` confirms the merge remotely and advances the primary `main`.
Git preserves unrelated edits; overlapping edits stop synchronization. Preserve
overlapping files in a recoverable stash/snapshot, update and reapply, resolve
them, then retry. Do not blanket-stash another agent's active checkout. Unique
local-main commits receive a recovery ref and must be reconciled through a PR.
Report "merged, sync blocked" until synchronization actually succeeds.

Inspect the task worktree for remaining source and ignored files. Preserve
needed ignored files; generated builds may be discarded explicitly after
inspection. Archive managed worktrees with the app tool (when available), then
run `cleanup <task>` from primary `main`. The helper can remove a plain clean
worktree with no ignored files; it never force-removes one. Cleanup checks remote
merge and commit identity, conditionally deletes the remote task branch, and
deletes the integrated local branch. Unexpected commits stop deletion.

`status [task]` reports local records plus remote PR state. Retry after an
interrupted request; PR lookup prevents duplicate creation, and confirmed merges
can resume sync/cleanup without merging twice. Inspect a stale lock's owner
before removing that exact lock directory; the lock is short, and a build-window
slot whose owner exited is reclaimed on its own. Records are recovery aids; GitHub PRs
and published Git commits remain the work inventory. Losing local records can
be recovered by adopting the preserved branch and rediscovering its PR.

For cancelled/superseded work, commit and checkpoint its intended changes,
then `abandon <task> --reason <text>`. This closes the PR while retaining the
published branch and worktree. Never delete unmerged work because of its age.
A task with no PR and no commits beyond `origin/main` holds no work, so the
same command removes its clean worktree and branches instead of opening a PR.

Report the merged PR, resulting local-main commit, validation and any cleanup
exception. State that the checkout is ready for the installer only after sync.
Run the installer automatically only when the user asked for installation.
