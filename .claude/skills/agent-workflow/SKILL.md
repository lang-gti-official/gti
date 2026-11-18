---
name: agent-workflow
description: Carry GTI implementation tasks through isolated branches, published checkpoints, focused validation, GitHub merge-queue integration, local-main synchronization, and safe worktree cleanup. Use for repository changes and resuming unfinished implementation tasks; read-only discussions and audits do not create branches or PRs.
---

# GTI Agent Workflow — Claude Client

Read [the canonical workflow skill](../../../.codex/skills/agent-workflow/SKILL.md)
completely before task actions. It owns GitHub publication, Gitea-era task
rehosting, focused validation, merge-queue integration, degraded-mode batches,
bounded logs, waiting, sidebar titles and cleanup. Do not maintain a competing
lifecycle here.

For new feature-planning requests, invoke the `plan-feature` skill before
starting a compiler task. `AGENTS.md` authorizes a draft RFC and draft proposal
PR in `lang-gti-official/rfcs`, using that repository's document workflow. This
does not authorize merging the proposal or implementing it. Discussion-only,
local-only, and no-publication requests override that default; ordinary bug-fix
plans and execution plans for accepted designs do not create new RFCs.

The short version: write tests as
[Writing Tests](../../../AGENTS.md#writing-tests) says, run focused checks
(reproducer, owning tests, affected cross-phase consumers, `gti_test_ui`,
smoke), record them with `verify`,
and `finish`: it labels the PR for a rollup (phase `rollup`) or, for Risk-system
diffs and `--no-rollup`, queues it alone (phase `queued`). Never run the
complete suite to finish a task; the queue proves build and fast checks on the
exact candidate, and full validation runs the suite after integration.

For a pre-cutover worktree, read that skill from the current primary checkout,
not its stale branch copy, and invoke the primary helper with
`--repo <task-worktree>`. Do not restore Gitea as `origin` to satisfy an old
helper; preserve live validation before explicit rehosting.

## Claude-specific worktree handling

The canonical repository is `lang-gti-official/gti` on GitHub; the helper uses
the authenticated GitHub CLI. Claude Code task branches use the `claude/`
prefix and Codex's use `codex/`. The helper accepts either, and without
`--branch` it picks `claude/` from Claude Code's `CLAUDECODE=1`.

Claude Code's managed session worktree may be inside the primary checkout's
`.claude/worktrees/`, which the helper cannot adopt. Let the helper create a
task worktree outside the primary checkout. Omit `--worktree` to apply the
[Mac mini SSD preference and internal fallback](../../../AGENTS.md#mac-mini-storage-preference).
An explicit path overrides selection; build within the chosen task checkout:

```sh
python3 scripts/agent_workflow.py start <task> --title '<outcome>' \
  --branch claude/<task>
```

Make task edits/builds/checks there with absolute paths and preserve the
session worktree. `--adopt` is for an existing clean task checkout outside
primary, not a way around ownership or freshness checks. Retain the ordinary
GitHub PR URL, and attach it with the client's PR tool when one is available.

Put temporary files (check plans, PR bodies, logs) in a directory unique to
the task, such as a subdirectory of the session scratchpad, never at the top
of a shared scratch or `/tmp` directory. Wait on a background operation by its
pid or output file (or the client's background-task notification); never with
`until ! pgrep -f "<text>"`, which matches its own command line and never
exits.

The stash stack is shared across worktrees: never use an unqualified
`git stash`/`git stash pop` or blanket-stash another active checkout.
Preserve overlaps with an explicitly identified recoverable snapshot.

Run task cleanup from a different checkout only after confirmed remote merge,
primary-main synchronization and ignored-file preservation. The client owns
archival of its session worktree; never remove that session checkout with Git.
Unavailable title/PR/archive controls are reported honestly; they do not permit
inventing a successful operation or skipping integration.
