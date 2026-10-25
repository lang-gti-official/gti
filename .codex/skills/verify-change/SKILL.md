---
name: verify-change
description: Plan, run, and report proportionate validation for a GTI working-tree, staged, commit, or branch change. Use when Codex needs to verify an implementation before handoff, commit, push, release, or PR review; select focused CTest and tooling gates; confirm tests, documentation, VERSION, and generated artifacts; or audit whether a change has sufficient evidence.
---

# Verify A GTI Change

Validate the behavior owned by the change and report concrete evidence. Keep
source code, tests, and canonical documentation authoritative; this workflow is
not a second semantic or test-selection authority.

## Establish The Scope

1. Run `git status --short` and preserve unrelated work. Do not clean, reset,
   stage, or reformat files outside the requested change.
2. Read [`docs/index.md`](../../../docs/index.md), then locate and read the
   task-relevant sections of
   [`docs/architecture/verification.md`](../../../docs/architecture/verification.md)
   and the owning language, architecture, decision, and plan documents. Use
   headings/search first rather than dumping the entire verification history.
   Read activated skill instructions completely.
3. Use the user-specified base when present. Otherwise verify the working tree
   against `HEAD`, including staged, unstaged, and untracked files.
4. Run the deterministic surface planner from the repository root:

   ```sh
   python3 .codex/skills/verify-change/scripts/plan_checks.py
   ```

   For staged changes, pass `--staged`. For a branch or commit range, pass
   `--base <ref>`. Treat its candidates as a lower bound; inspect the diff and
   expand them from the canonical verification document.

## Select Evidence

Select T1/T2/T3 from the invariant changed, using the canonical
[batch validation contract](../../../docs/architecture/agent-workflow.md#risk-based-batch-validation).
T1 is confined recovery/diagnostic or otherwise narrow behavior; T2 changes
semantic/shared compiler behavior; T3 changes system, ABI/runtime or optimizer
invariants. "ICE" and a small diff do not establish low risk. The script's
critical-path floor is conservative; review can raise it. T2/T3 plans must
name relevant broader subsystem checks and all required platforms.

For a fix, retain evidence of the intended original failure and its corrected
behavior. Use a known-bad revision where practical; unrelated syntax failure
does not reproduce the bug. If unavailable or unsafe, record the concrete
limitation and existing evidence. Bound resource-exhaustion reproductions.
For code generation, also test observable execution/cleanup and applicable
negative cases so an ICE cannot silently become a miscompile.

- Trace changed behavior through its owning layer. Do not infer GTI semantics
  from emitted C++ or documentation examples.
- For syntax or language meaning, cover applicable parser/AST, semantics, HIR,
  MIR, optimizer/backend, runtime/stdlib, formatter, Tree-sitter, LSP, examples,
  and specification surfaces. Document an intentionally unaffected layer
  instead of adding a placeholder test.
- For diagnostics, verify the stable code, message fragment, exact source/span,
  related information, recovery, and fix-it. Include LSP protocol checks when
  publication or code actions change.
- For driver or project changes, preserve direct-mode compatibility and verify
  immutable plans, exact arguments, path/symlink containment, caching or
  publication behavior, CLI workflows, and installed-library boundaries as
  applicable.
- For build, packaging, runtime, ABI, or release changes, include the affected
  clean-stage or installed-consumer checks. Apply the repository version policy
  to shipped behavior.
- For documentation-only changes, do not build by default unless examples,
  commands, generated material, or claimed behavior require execution.
- For changes affecting runtime execution, apply
  [ADR 012's performance acceptance](../../../docs/decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09).
  Verify representative optimized execution and report runtime, allocations,
  code-size and relevant build/memory tradeoffs. Structural architecture work
  does not need a benchmark to begin, but functional passes alone do not prove
  performance acceptance. A material regression needs correction or explicit
  owner acceptance of the documented tradeoff, not merely a follow-up issue.

## Run Checks

Before expensive runs, record the acceptance gates, commands, compiler/build
mode and platform, source head/base, and where results will be retained. Run
focused owning/portability checks before a broad sweep when they can expose an
invalid setup cheaply; they do not replace required broad coverage.

1. Run the narrow owning test first, followed by every affected cross-phase or
   tooling gate. Use exact CTest names or a deliberately bounded `-R` regex.
2. Build before running a target when its executable may be stale or absent.
   Do not destructively reconfigure an existing build tree or fetch new
   dependencies without the authority required by the task. Every check that
   compiles or analyzes GTI must use a GTI compiler executable built as
   `Release`, `RelWithDebInfo`, or `MinSizeRel`; never report an unoptimized
   compiler as verification evidence. If the available tree is unoptimized,
   create a separate optimized Ninja build rather than repurposing the active
   tree. Run every full build and every whole or broad CTest or harness run
   through the shared build window,
   `python3 scripts/build_window.py --kind build|suite -- <command>`, so
   concurrent sessions share the host; focused `-R` selections and
   single-file compiles run directly.
3. Select completion coverage from the changed invariant and affected consumer
   graph, as described in the canonical verification document. An independent
   isolated subsystem change can complete with owning, cross-phase and required
   platform/runtime checks; an all-target build/full suite is not automatic.
   Name unchanged boundaries and preserve ABI, sanitizer, installed-consumer
   and performance obligations where applicable. Shared-contract/cross-cutting
   changes and designated full integration/release campaigns need broader
   coverage. Keep
   `scripts/local_language_audit.py --full` non-gating; turn any bug it finds
   into a minimal deterministic regression in the owning suite.
   In a batch, perform component regressions, owning/cross-phase checks and
   relevant subsystem coverage once at admission. The designated batch owner
   runs full coverage on the combined candidate; component agents do not each
   repeat that campaign. Full/platform/performance requirements remain part
   of integration acceptance, with original and combined evidence distinguished.
4. Run `git diff --check` and inspect the complete scoped diff for unintended
   generated, vendored, user, or semantic changes.
5. If a check fails, preserve its raw command and output, identify the owning
   failure, and fix only when the task authorizes implementation. Never hide a
   failure by weakening, deleting, or skipping the test.

Use the [execution contract](../../../docs/architecture/agent-workflow.md#bounded-output-analysis-and-waiting)
for bounded log display and waiting on the same live operation. A tail is not
proof of success: verify exit status, test counts/skips, source identity and
truncation metadata. Keep the complete evidence available under the runner's
retention contract; do not reduce test execution to meet a display budget.

When upstream changes invalidate integration binding, inspect the delta and
record why each gate remains applicable or needs fresh execution. Validate the
new clean published head/base through the helper. Repeat every affected,
previously failed and required broad/platform gate; broaden when interaction
risk is uncertain. Earlier passes stay attributed to their original sources,
not substituted for fresh acceptance. Do not restart a running suite merely
because main advanced. Deeper correctness/design analysis can expand the
checks at any point; cost controls never waive the resulting obligations.

## Report The Result

Use `batch enroll <task> --plan <json> --build <optimized-tree>` for focused
handoff. Report "ready for integration; full combined validation pending",
including the exact source, tier/rationale, tests, before-fix evidence,
platforms and retained logs. This is a handoff checkpoint, not completed
integration. An integrated full-suite failure is attributed to the owning
change; helpers may assist after establishing the intended invariant.

Report:

- scope and changed surfaces;
- commands run with pass, fail, or skipped status;
- focused and broad evidence obtained;
- findings or fixes made;
- required checks not run and the exact reason;
- residual platform, configuration, or semantic risk.

Do not call a change verified while a required gate is failing or unrun. State
"partially verified" when environmental limits or task scope leave required
evidence outstanding.

Gitea CI is advisory, not one of those required acceptance gates. Do not wait
for Actions to pass before handing off or merging locally validated work.
Keep local/managed-runner requirements intact and distinguish their evidence
from pending or failed CI feedback; a known CI failure is not a passing result.
