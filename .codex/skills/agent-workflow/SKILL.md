---
name: agent-workflow
description: Carry GTI implementation tasks through isolated branches, published checkpoints, focused validation, GitHub merge-queue integration, local-main synchronization, and safe worktree cleanup. Use for repository changes and resuming unfinished implementation tasks; read-only discussions and audits do not create branches or PRs.
---

# GTI Agent Workflow

The normal implementation outcome is a PR that is ready, with its focused
validation recorded, and handed to integration: labelled for a rollup, registered for GitHub merge-when-ready, or in the
merge queue. The queue merges it, and `sync`
brings the primary checkout to current `main` for the user's local installer.
The repository owner's standing authorization is in `AGENTS.md`. Explicit
instructions such as "PR only", "leave local", "do not merge", or a
release/install request override the default. Never infer permission to change
other repositories or services.

Use `scripts/agent_workflow.py` for shared Git/API operations. GitHub
(`lang-gti-official/gti`, remote `origin`) is the canonical host for branches,
PRs, issues, CI and toolchain releases. Gitea keeps only the reference website
and the `gti add` package registry, and its `main` is frozen. The helper authenticates through the GitHub CLI
(`gh auth login`) or `GH_TOKEN` and never stores a token; it keeps token-free
recovery records in the common Git directory. Run it from any checkout of the
repository, or pass `--repo <checkout>`; it always acts on the primary checkout
it shares a Git directory with. Read
[the command contract](../../../docs/architecture/agent-workflow.md) when
recovering interruptions or interpreting a refused operation.

GitHub history carries rewritten commit identities: every tree is identical to
Gitea's, but commit IDs differ. Gitea-era task records show as `legacy-gitea`
in `status`; pre-Gitea schema-2 GitHub records show as `legacy-github` and
also need explicit rehosting because their historical PR numbers changed.
Publish the task's branch to GitHub with the same rewritten
identity, open its PR, then run `rehost <task> --pr <number>`; it requires the
PR head to carry the task's published tree and moves a clean worktree onto the
identical rewritten commit. Wait for any live validation to finish first. Do not
restore a retired remote to satisfy a stale helper.

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

Rename your own chat's sidebar title to a short description of the new task
before starting work, and refresh it when the task changes. Use the client's
chat-title control, such as `set_thread_title` with the current chat as its
target, not another agent's chat. If unavailable, note that limitation in the
task update and continue without claiming a rename. Follow the canonical
[sidebar-title contract](../../../docs/architecture/agent-workflow.md#sidebar-task-titles)
for completion and intermediate statuses.

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

**You validate the change; the merge queue validates integration.** Run the
exact reproducer or new regression (failing on the parent revision for a fix),
the owning subsystem's tests and the affected cross-phase and tooling
consumers, plus `compiler_pipeline` and the native smoke. Never run the
complete suite to finish a task: the queue runs it on the exact candidate that
becomes `main`, sharded across macOS arm64 and Linux x64, and its `CI gate`
proves every registered test ran. Full integration also builds the bundled
production archives and validates their installed consumers. Nightly publishes
those exact CI artifacts after checking source and run provenance; it does not
rebuild or repeat the complete suite. Automation-only CI does not produce
release artifacts. See the [nightly contract](../../../docs/architecture/build-and-driver.md#github-nightly-publication)
for full main-dispatch recovery when no publishable artifacts exist. Deferring
integration is complete contributor work; report it as deferred, never as passed.

1. Reconcile only to resolve a conflict (`reconcile <task>` merges current
   `origin/main` and pushes; resolve conflicts in this worktree, never
   force-push). An advancing `main` does not void focused evidence.
2. Put the focused commands in a temporary JSON file containing argv arrays,
   then run `verify <task> --checks-file <file>`. For example:

   ```json
   [["git", "diff", "--check"],
    ["ctest", "--test-dir", "build-release", "--output-on-failure", "-R", "^owning_gate$"]]
   ```

   Build necessary targets first, through a build-window slot:
   `python3 scripts/build_window.py --kind build -- cmake --build <tree> ...`.
   The helper runs the checks inside a build-window `suite` slot, holding its
   metadata lock only while recording, and binds passing evidence to the clean
   published head. It does not select adequate tests or prove their quality;
   that remains your responsibility. A new commit requires verifying again.
   For developer-automation-only or source-only GTI stdlib changes,
   `verify --focused --checks-file ...` runs owning checks directly and records
   scope `automation` or `source-stdlib`; its diff guard rejects compiler,
   native runtime and build-system changes.
3. After focused validation passes and the PR is published, submit every completed
   task with `preview submit <task> --tests <exact-owning-ctest-names>`. This is
   the standing default, including fixes and automation work; do not ask again.
   Explicit no-preview or no-publication instructions override it. Select this
   task only, never sweep other PRs. Submission is detached and combined preview
   admission remains mandatory; report a failure without blocking main delivery.
   Continue normal integration without waiting for preview.

   Write the final PR description to a temporary file: behaviour, exactly
   which checks ran, and "complete suite deferred to the merge queue". Unless
   the change is automation-only, end it with a `Validation:` block whose `T0`
   and `T1` items list the CTest names you ran, comma-separated; other items
   (`tier`, `T2`, `reproduction`, `unaffected`, `T3`) are prose:

   ```text
   Validation:
   - tier: T2 (MIR cleanup publication for a new borrow shape)
   - T0: mir_borrow_carrier_cleanup_regression
   - T1: mir_full_expression, optimizer_conformance, compiler_pipeline
   - T2: llvm_place_execution, cpp_mir_body_emitter_lifecycle
   - reproduction: fails on parent <sha> with GTI-B0003, passes on the patch
   - T3: deferred to the merge queue
   ```

   `finish` refuses a missing block or a declared test that no recorded
   `ctest -R` pattern or focused plan ran, and PR CI re-runs the declared T0
   and T1 tests on both platforms. Run
   `finish <task> --body-file <file>`. It marks the PR ready and checks the
   `main` ruleset. A PR that is not Risk-system is labelled `rollup=maybe`
   (`rollup=always` when automation-only) and the task becomes `rollup`: the
   rollup bot combines it with other green PRs into one queue entry. A
   Risk-system diff (build/toolchain files, workflows, test registration, CI
   configuration scripts, runtime ABI/layout headers), a PR labelled
   `rollup=never` or `rollup-failed`, or `finish --no-rollup` instead joins the
   merge queue alone through GitHub's native merge-when-ready. The request
   checks the verified head atomically. Pending required checks/reviews produce
   `awaiting-ci`; GitHub enters the queue when requirements pass (`queued`).
   `rollup`, `awaiting-ci` and `queued` are normal completion points. Do not poll
   healthy PR CI or keep an agent alive to retry queue entry. `status` reports
   live `remote_integration` separately from the last recorded phase. Failures
   and conflicts still require repair. A rejected registration is not a handoff;
   report the error and retry only after its cause is resolved. Lost responses
   are recoverable by retry: existing remote intent/queue membership is reused.
   Later changes must use `checkpoint`/`reconcile`, which cancel pending intent
   and queue membership before pushing and invalidate old validation. GitHub's
   expected-head guard applies at registration, not to arbitrary later direct
   pushes; never push around this helper after handing off. `--pr-only`
   withdraws existing integration intent before stopping ready for review.
   Use `--pr-only` when the user requested review before integration.
4. The queue merges asynchronously. Run `sync <task>` (or `finish` again) once
   it has merged. A PR merged as part of a rollup counts as merged, including
   one the bot closed with its integration proof. If a rollup fails, the bot
   labels its members `rollup-failed` and queues each alone. If the queue
   removes the PR itself because the complete suite failed, the failure
   belongs to this change: reproduce the failing tests locally, repair them
   without weakening unrelated expectations, verify, and `finish` again (it
   then queues alone). A `needs-rebase` label means reconcile with main. Never
   wait in a polling loop for the queue.

`protection` audits server enforcement. `protection --configure` installs the
owner-approved `main` ruleset: pull requests only, the merge queue and the
`CI gate` check required, no force push or deletion, and no bypass for any role.

**Degraded mode and batches.** `batch enroll`/`prepare`/`verify` remain for one
purpose: when GitHub Actions has been unavailable for more than four hours, a
designated integrator may combine admitted tasks into one candidate, validate
it on a remote runner with `batch verify`, and record that evidence on the PR.
Components never merge directly; `sync` closes them as integrated via the
combined PR with exact membership proof. Never merge `preview`.

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

Report the PR, its queue or merge state, exactly which focused checks ran,
that the complete suite is the queue's, the resulting local-main commit once
synced, and any cleanup exception. State that the checkout is ready for the
installer only after sync.
Run the installer automatically only when the user asked for installation.
After delivering the requested outcome, rename the chat `Complete — <task>`.
A task with confirmed GitHub merge-when-ready, queue entry or rollup label and
recorded focused validation is complete; title it `Complete — <task> (awaiting CI)`
or `Complete — <task> (queued)` as applicable until it merges. A batch component handed off for
integration is `Handed off — <task>`.
Explicit PR-only or local-only requests use their requested delivery boundary.
