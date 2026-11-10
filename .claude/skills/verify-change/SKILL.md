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
after integration, so never run it locally to finish a task. Local runs
follow the CI test policy: harnesses take the configured tree's
`GTI_TEST_BACKENDS` (`llvm`), and the frozen C++ backend is not built unless
deliberate C++ work configures `-DGTI_ENABLE_CPP_BACKEND=ON
-DGTI_TEST_BACKENDS=cpp`; `-LE cpp-backend` stays harmless.

Use the canonical planner to avoid a stale client-specific selection policy:

```sh
python3 .codex/skills/verify-change/scripts/plan_checks.py
```

Preserve this client's separate task-worktree handling from
[agent-workflow](../agent-workflow/SKILL.md). Do not claim a skipped or failing
gate passed, and report the complete suite as deferred to post-integration full
validation, never as
passed.
