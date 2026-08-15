---
name: verify-change
description: Plan, run, and report proportionate validation for a GTI working-tree, staged, commit, or branch change. Use when verifying an implementation before handoff, commit, push, release, or PR review; select focused CTest and tooling gates; confirm tests, documentation, VERSION, and generated artifacts; or audit whether a change has sufficient evidence.
---

# Verify A GTI Change

Validate the behavior owned by the change and report concrete evidence. Keep
source code, tests, and canonical documentation authoritative; this workflow is
not a second semantic or test-selection authority.

## Establish The Scope

1. Run `git status --short` and preserve unrelated work. Do not clean, reset,
   stage, or reformat files outside the requested change.
2. Read [`docs/index.md`](../../../docs/index.md) and
   [`docs/architecture/verification.md`](../../../docs/architecture/verification.md),
   plus the owning language, architecture, decision, and plan documents.
3. Use the user-specified base when present. Otherwise verify the working tree
   against `HEAD`, including staged, unstaged, and untracked files.
4. Run the deterministic surface planner from the repository root:

   ```sh
   python3 .claude/skills/verify-change/scripts/plan_checks.py
   ```

   For staged changes, pass `--staged`. For a branch or commit range, pass
   `--base <ref>`. Treat its candidates as a lower bound; inspect the diff and
   expand them from the canonical verification document.

## Select Evidence

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

## Run Checks

1. Run the narrow owning test first, followed by every affected cross-phase or
   tooling gate. Use exact CTest names or a deliberately bounded `-R` regex.
2. Build before running a target when its executable may be stale or absent.
   Do not destructively reconfigure an existing build tree or fetch new
   dependencies without the authority required by the task.
3. For a substantial compiler change, use the broad sequence in the canonical
   verification document after focused checks pass. Keep
   `scripts/local_language_audit.py --full` non-gating; turn any bug it finds
   into a minimal deterministic regression in the owning suite.
4. Run `git diff --check` and inspect the complete scoped diff for unintended
   generated, vendored, user, or semantic changes.
5. If a check fails, preserve its raw command and output, identify the owning
   failure, and fix only when the task authorizes implementation. Never hide a
   failure by weakening, deleting, or skipping the test.

## Report The Result

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
