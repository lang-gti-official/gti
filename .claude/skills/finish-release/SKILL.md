---
name: finish-release
description: Apply GTI version and release policy to focused-validated work delivered through the agent-workflow merge-queue lifecycle. Use when an implementation is ready to deliver or the user asks to commit, publish or release. Asynchronous release packaging does not block completion.
---

# Finish A GTI Release — Claude Client

Read [the canonical finish-release skill](../../../.codex/skills/finish-release/SKILL.md)
completely before deciding VERSION or delivery. It owns the version policy,
`Release-Impact` trailers, delivery through the merge queue, and the rule that
toolchain releases publish to GitHub Releases (the scheduled nightly workflow;
stable waits for the beta/stable train) while the `gti add` package registry
stays on Gitea.
Source branches and PRs go only to the canonical GitHub repository.

Deliver through [agent-workflow](../agent-workflow/SKILL.md), keeping this
client's separate task-worktree handling.
