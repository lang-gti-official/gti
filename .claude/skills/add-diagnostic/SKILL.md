---
name: add-diagnostic
description: Add, revise, or review a GTI compiler, source-loader, parser, semantic, driver, backend, or LSP-published diagnostic. Use when introducing a diagnostic code, improving wording or source spans, adding related information, hints, recovery behavior, or a mechanically safe fix-it/code action.
---

# Add A GTI Diagnostic

Use the existing structured diagnostic pipeline and keep the error owned by the
earliest phase that knows the rule.

## Workflow

1. Run `git status --short`. Read
   [`docs/architecture/diagnostics.md`](../../../docs/architecture/diagnostics.md)
   and the language/architecture document for the rule.
2. Reproduce the invalid input with the smallest focused test. Confirm which
   phase currently detects or misses it.
3. Search nearby codes and construction helpers with `rg`; extend the existing
   phase family instead of creating a parallel registry.
4. Produce a `Diagnostic` with:
   - stable code, correct phase, and severity;
   - actionable primary span using half-open source byte offsets;
   - concise rule-focused wording;
   - related declaration/include/instantiation spans where useful;
   - hints only when they add information;
   - a fix-it only when one edit is correct for every triggering program.
5. Preserve recovery. Parsing/semantic analysis should continue safely where
   the surrounding source remains meaningful, and code generation must stay
   disabled for invalid input.
6. Add assertions for the code, meaningful message fragment, exact source
   identity/span, related data, hint, and replacement as applicable. Avoid
   brittle whole-rendered diagnostic snapshots.
7. If a fix-it or publication changes, run the LSP protocol tests and verify
   stale generations, UTF-16 conversion, and current-source matching. Code
   actions must come from `Diagnostic::fixes`, never message matching.
8. Update the owning language/architecture doc if the diagnosed rule or
   recovery contract changed. Run focused tests and `git diff --check`.

After local validation, use the `finish-release` skill when the completed
diagnostic change is ready to commit or changes shipped behavior. Do not wait
for GitHub CI/CD after the release push or workflow dispatch succeeds.

Do not use `GTI-B0001` for invalid user source; it is the internal MIR/backend
integrity diagnostic.
