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
(`gti-lang-official/gti`, remote `origin`) is the canonical host for branches,
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

## Start and publish

1. Inspect status; preserve other tasks' work. Fetch current `origin/main`.
   Give each task its own `codex/` branch and worktree; the helper enforces the
   `codex/` prefix for every client. Never implement on primary `main`, reuse
   another task's branch, or reset a checkout.
2. Let the helper create the task worktree. Claude Code's managed worktrees
   live under the primary checkout's `.claude/worktrees/`, and the helper
   refuses any worktree inside the primary checkout, so they cannot be adopted.
   Choose an unused absolute path outside the primary checkout (the session
   scratchpad directory when one is listed, otherwise a sibling directory):

   ```sh
   python3 scripts/agent_workflow.py start <task> --title '<outcome>' \
     --worktree <absolute-path> --branch codex/<task>
   ```

   The helper fetches `origin/main`, creates the branch and worktree from it,
   and pushes the branch immediately. Make every edit, build, and check in that
   worktree using absolute paths, and leave the session's managed worktree
   clean. `--adopt` registers an existing clean checkout outside the primary
   checkout, such as a preserved task worktree being resumed, and requires it
   to contain current `origin/main`.

   The first meaningful checkpoint creates a draft PR; do not manufacture empty
   commits or one-off plan files for this purpose. Retain its ordinary URL (the
   helper reports it as `pr_url`), and attach it to the session with the
   client's PR tool when that tool supports GitHub.
3. Commit and push coherent checkpoints, before a pause or handoff, and at
   completion. Review the diff, then use `checkpoint`. It can commit
   `--message <text> --paths <files...>` (it refuses a pre-staged index), or
   publish an already committed clean checkpoint. Include work that belongs to
   this task only. An unfinished checkpoint can be published without claiming
   it is validated.

## Validate and integrate

Use the owning feature skill and the `verify-change` skill to select real
checks with an optimized GTI compiler. Provide required runnable examples and
their inventory updates for language changes. Use the `finish-release` skill
for VERSION and release-trailer decisions.

1. Run `reconcile <task>` to merge current `origin/main` into the task branch
   and push. Resolve any conflicts in the task worktree; do not force-push.
2. Put the proportionate final commands in a temporary JSON file containing
   argv arrays, then run `verify <task> --checks-file <file>`. For example:

   ```json
   [["git", "diff", "--check"],
    ["ctest", "--test-dir", "build-release", "--output-on-failure", "-R", "^owning_gate$"]]
   ```

   Build necessary targets first, through a build-window slot:
   `python3 scripts/build_window.py --kind build -- cmake --build <tree> ...`.
   The helper executes these commands in the task worktree inside a
   build-window `suite` slot and holds its metadata lock only while recording,
   so other sessions can still start, checkpoint and inspect tasks while it
   waits for or runs validation; `python3 scripts/build_window.py --status`
   shows who holds the window. It binds passing evidence to the clean,
   published head and current remote base. It does not select adequate tests
   or prove their quality; that remains the agent's responsibility. Changes or
   upstream advancement require revalidation.
3. Write the final PR description to a temporary file, including behaviour,
   validation and limitations. Run `finish <task> --body-file <file>`.
   It marks the PR ready, checks validation and branch protection, merges through
   GitHub with the exact tested head, then attempts a local-main fast-forward
   and pushes `main` and the tags to the `gitea` mirror remote.
   Use `--pr-only` when the user requested review before integration.
4. Required server checks/reviews are respected; never force or administratively
   bypass a rejection. Wait using the client's wait or continuation facilities
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
overlapping files in a recoverable snapshot, update and reapply, resolve them,
then retry. The Git stash stack is shared by every worktree and session: never
use a bare `git stash`/`git stash pop`, and do not blanket-stash another agent's
active checkout. Unique local-main commits receive a recovery ref and must be
reconciled through a PR. Report "merged, sync blocked" until synchronization
actually succeeds.

Inspect the task worktree for remaining source and ignored files. Preserve
needed ignored files; generated builds may be discarded explicitly after
inspection. Then run `cleanup <task>` from a checkout other than the task
worktree, such as the session's managed worktree. The helper removes a plain
clean worktree with no ignored files; it never force-removes one. Cleanup
checks remote merge and commit identity, conditionally deletes the remote task
branch, and deletes the integrated local branch. Unexpected commits stop
deletion. The client archives the session's own managed worktree; do not
remove it with Git.

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

Report the merged PR, resulting local-main commit, validation and any cleanup
exception. State that the checkout is ready for the installer only after sync.
Run the installer automatically only when the user asked for installation.
