---
name: verify-change
description: Plan, run, and report proportionate validation for a GTI working-tree, staged, commit, or branch change. Use when Codex needs to verify an implementation before handoff, commit, push, release, or PR review; select focused CTest and tooling gates; confirm tests, documentation, VERSION, and generated artifacts; or audit whether a change has sufficient evidence.
---

# Verify A GTI Change

Read [the canonical verification skill](../../../.codex/skills/verify-change/SKILL.md)
completely before selecting or running checks. It owns optimized compilers,
risk-based focused/batch/platform coverage, preservation, exact-source evidence,
bounded output and waiting. Gitea CI is advisory, never a substitute or a queue
to wait on before a locally validated merge.

Use the canonical planner to avoid a stale client-specific selection policy:

```sh
python3 .codex/skills/verify-change/scripts/plan_checks.py
```

Preserve this client's separate task-worktree handling from
[agent-workflow](../agent-workflow/SKILL.md). Do not claim a skipped or failing
gate passed; retain original evidence and revalidate affected upstream deltas.
