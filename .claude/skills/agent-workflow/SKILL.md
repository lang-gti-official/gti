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

The short version: run focused checks (reproducer, owning tests, affected
cross-phase consumers, `compiler_pipeline`, smoke), record them with `verify`,
and `finish` into the merge queue. Never run the complete suite to finish a
task; the queue runs it on the exact candidate.

For a pre-cutover worktree, read that skill from the current primary checkout,
not its stale branch copy, and invoke the primary helper with
`--repo <task-worktree>`. Do not restore Gitea as `origin` to satisfy an old
helper; preserve live validation before explicit rehosting.

## Claude-specific worktree handling

The canonical repository is `lang-gti-official/gti` on GitHub; the helper uses
the authenticated GitHub CLI. The helper requires a `codex/` branch for every
client.

Claude Code's managed session worktree may be inside the primary checkout's
`.claude/worktrees/`, which the helper cannot adopt. Let the helper create a
task worktree at an unused absolute path outside the primary checkout:

```sh
python3 scripts/agent_workflow.py start <task> --title '<outcome>' \
  --worktree <absolute-path> --branch codex/<task>
```

Make task edits/builds/checks there with absolute paths and preserve the
session worktree. `--adopt` is for an existing clean task checkout outside
primary, not a way around ownership or freshness checks. Retain the ordinary
GitHub PR URL, and attach it with the client's PR tool when one is available.

The stash stack is shared across worktrees: never use an unqualified
`git stash`/`git stash pop` or blanket-stash another active checkout.
Preserve overlaps with an explicitly identified recoverable snapshot.

Run task cleanup from a different checkout only after confirmed remote merge,
primary-main synchronization and ignored-file preservation. The client owns
archival of its session worktree; never remove that session checkout with Git.
Unavailable title/PR/archive controls are reported honestly; they do not permit
inventing a successful operation or skipping integration.
