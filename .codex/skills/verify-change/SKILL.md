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
4. Run the deterministic planner from the repository root:

   ```sh
   python3 .codex/skills/verify-change/scripts/plan_checks.py
   ```

   For staged changes, pass `--staged`. For a branch or commit range, pass
   `--base <ref>`; `--paths <path>...` plans for named paths, and
   `--format json` gives the same plan as data. Treat its candidates as a
   lower bound; inspect the diff and expand them from the canonical
   verification document.

## Read The Planner Output

The planner selects tests from [`tests/ownership.toml`](../../../tests/ownership.toml),
the test-ownership map that PR CI and reviewers also use. Each map area names
source paths, the CTest tests that own them and the downstream phases and
tooling that consume them. For the changed paths the planner prints:

- each matched **area** and the changed paths it selected; nested areas
  (for example `llvm-backend` and `llvm-abi-layout`) both apply;
- the **baseline** every implementation change runs: `compiler_pipeline`
  and the native O0/O3 smoke
  (`python3 scripts/validation_checks.py smoke --build build`);
- **T0**, the registered tests whose CMake registration names a changed test,
  fixture or harness file;
- **T1**, the owning tests, and **T2**, the consumer tests, each as one
  runnable `ctest ... -R '<anchored regex>'` command, followed by a
  per-test list of the areas that selected it;
- the format, build, Tree-sitter, agent-workflow and ownership-map checks the
  changed files need;
- the highest **risk** among matched areas. `shared` (the SHARED floor in
  `scripts/validation_checks.py`) means T2 at least, with the subsystem-wide
  sweeps the map lists as consumers. `system` (ABI/layout/calling convention,
  runtime services and allocation, CMake/toolchain, CI and release workflows,
  test infrastructure) means set rollup=never and validate the change alone,
  never combined with other changes; still do not run a near-full suite
  locally, because the merge queue runs it;
- **uncovered** paths that no area owns. Add an area, or an `[[unowned]]`
  entry with its reason, in the same change; `tests/ownership_map_test.py`
  fails while a tracked file under `src/`, `include/`, `runtime/`, `stdlib/`,
  `scripts/` or `lua/` has neither.

The selection is a lower bound, never a proof of sufficiency. The author
extends it from their own analysis of the changed invariant: which phases and
tools consume the changed fact, which feature regressions exercise it, and
which platform, runtime or performance obligations apply. A test the map does
not name is not thereby excused. When review finds a consumer the map missed,
add it to the owning area so the next change inherits it.

## Select Evidence

Select T1/T2/T3 from the invariant changed, using the canonical
[batch validation contract](../../../docs/architecture/agent-workflow.md#risk-based-batch-validation).
T1 is confined recovery/diagnostic or otherwise narrow behavior; T2 changes
semantic/shared compiler behavior; T3 changes system, ABI/runtime or optimizer
invariants. "ICE" and a small diff do not establish low risk. The planner's
risk is a floor from the map; review can raise it. T2/T3 plans must
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
  clean-stage or installed-consumer checks. Artifact-reuse changes must exercise
  rejection of wrong source/run/attempt/platform/configuration, missing or expired
  artifacts, and altered archives or checksums. Fixture publication checks do not
  establish real compiler build or platform evidence. Apply the repository
  version policy to shipped behavior.
- For documentation-only changes, do not build by default unless examples,
  commands, generated material, or claimed behavior require execution.
- Apply [the backend development policy](../../../docs/decisions/031-native-llvm-backend-foundation.md#backend-development-policy-2026-10-25):
  C++ is bug-fix-only through v1.0, not a feature-parity gate for LLVM. Preserve
  its supported regressions/differential checks; record comparator gaps and
  require independent GTI/native execution, rejection, cleanup and applicable
  platform/performance evidence instead of silent skips or new C++ features.
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
   The complete suite is never a contributor gate: the merge queue runs it on
   the exact candidate on every required platform. Platform, runtime and
   performance obligations that the queue does not cover (for example a
   benchmark comparison under ADR 012) remain yours to run and record.
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

Focused evidence binds to the task head. An advancing `main` does not void it:
the merge queue validates the combination. A new commit on the task does, so
verify again after any change. Deeper correctness/design analysis can expand
the checks at any point; cost controls never waive the resulting obligations.

## Report The Result

Record the focused checks with `verify`, then hand the task to integration
with `finish` (rollup, native merge-when-ready, or queue entry). Report "focused validation passed; complete suite deferred to
the merge queue", with the exact source, risk tier and rationale, tests,
before-fix evidence, platforms and retained logs. A complete-suite failure in
the queue is attributed to the change that introduced it; helpers may assist
after establishing the intended invariant. For example:

```text
Validation:
- reproducer fails on parent and passes on patch
- owning semantic suite passes
- affected MIR/LLVM tests pass
- relevant runtime tests pass
- complete suite deferred to the merge queue
```

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

The merge queue's `CI gate` is the required integration gate. Do not poll it or
duplicate it locally; report the confirmed handoff as awaiting CI or queued, and report a queue failure
honestly rather than as a pass.
