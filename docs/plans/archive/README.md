# Archived Plan Records

The documents in this directory preserve completed delivery plans, migration
records, and implementation checkpoints. They remain useful design and audit
evidence, but they do not describe current GTI behavior or the active work
queue.

For current behavior, use [`docs/architecture/`](../../architecture/) and
[`docs/language/`](../../language/). For active work, use the plans linked from
the [documentation map](../../index.md#plans), especially the
[implementation sequence](../implementation-sequence.md),
[systems-readiness roadmap](../roadmap-to-1.0.md), and
[language restriction ledger](../language-alignment.md).

Archived records retain their original milestone names, version checkpoints,
measurements, and implementation rationale. Those historical details should
not be updated merely to match a later compiler state. Correct a record only
when its own evidence or links are wrong.

[`gitea-issue-map.md`](gitea-issue-map.md) maps every migrated Gitea issue
number to its GitHub issue.

The bug-finding record also retains a
[legacy redirect](../compiler-bug-finding-and-triage.md) because repository
debugging and fuzzing tools cite that established path.

## Contents

- [C interop target surface](c-interop-target-surface.md)
- [Compiler bug-finding and triage tooling](compiler-bug-finding-and-triage.md)
- [Compiler library migration](compiler-library-migration.md)
- [Compiler roadmap checkpoint](compiler-roadmap-status.md)
- [Systems-readiness roadmap checkpoint (2026-10-11)](systems-readiness-roadmap-2026-10-11.md)
  preserves the earlier milestone structure and measurements; the active
  roadmap reconciles delivered capabilities rather than restarting them
- [Compiler structural refactor](compiler-structural-refactor.md)
- [Conditional compilation and configuration flags](conditional-compilation.md)
- [Constructor-local type generics and constraints](constructor-local-type-generics.md)
- [Mutable field groups](mutable-field-groups.md)
- [Mutable temporary receivers](mutable-temporary-receivers.md)
- [Read-only receiver diagnostic precision](read-only-receiver-diagnostics.md)
- [Static assertions](static-assertions.md)
- [Gitea Actions workflows](gitea-actions/README.md), archived 2026-10-11 when CI
  hosting moved to the `gti-lang-official/gti` GitHub repository; the earlier
  [GitHub Actions workflows](github-actions/) they replaced sit beside them

That hosting event is historical. The current canonical compiler repository is
`lang-gti-official/gti`; current CI and publication contracts belong in
[the workflow architecture](../../architecture/agent-workflow.md).
