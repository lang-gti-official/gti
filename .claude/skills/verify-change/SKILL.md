---
name: verify-change
description: Plan, run, and report proportionate validation for a GTI working-tree, staged, commit, or branch change. Use when Codex needs to verify an implementation before handoff, commit, push, release, or PR review; select focused CTest and tooling gates; confirm tests, documentation, VERSION, and generated artifacts; or audit whether a change has sufficient evidence.
---

# Verify A GTI Change

Read [the canonical verification skill](../../../.codex/skills/verify-change/SKILL.md)
completely before selecting or running checks. It owns optimized compilers,
risk-based focused and platform coverage, preservation, exact-source evidence,
bounded output and waiting. Run focused checks only: the merge queue runs the
bounded gate on the exact candidate and full validation runs the complete suite
after integration, so never run it locally to finish a task. New tests follow
[Writing Tests](../../../AGENTS.md#writing-tests) and run in seconds with
`build-release/gti-test <path-or-substring>`. Local runs follow the CI test
policy: LLVM at `-O0`, no C++-backend checks, and the daily slow tier left out
with `-LE slow`.

Use the canonical planner to avoid a stale client-specific selection policy:

```sh
python3 .codex/skills/verify-change/scripts/plan_checks.py
```

Preserve this client's separate task-worktree handling from
[agent-workflow](../agent-workflow/SKILL.md). Do not claim a skipped or failing
gate passed, and report the complete suite as deferred to post-integration full
validation, never as
passed.
