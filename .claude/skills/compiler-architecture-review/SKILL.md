---
name: compiler-architecture-review
description: Review or design structural changes to the GTI compiler and tooling, including frontend ownership, semantic representation, HIR/MIR boundaries, optimization, backends, runtime capabilities, driver layering, source locations, caching, or compiler/LSP coupling. Use when proposing a refactor, moving responsibility between stages, introducing an IR or subsystem, or assessing architectural debt.
---

# Review GTI Compiler Architecture

Produce an evidence-based ownership review, not another architecture reference.

## Review Procedure

1. Run `git status --short` and read
   [`docs/index.md`](../../../docs/index.md) plus the affected architecture,
   decision, language, and plan documents.
2. Trace the live call/data path through source and tests. Record any
   documentation contradiction.
3. State the concrete problem, the user-facing program/API/workflow it
   unlocks, and the invariant the proposal must preserve. For
   infrastructure-only work, name the imminent client or correctness defect.
4. Map each relevant fact to its producer, owner, lifetime, consumers, and
   invalidation rule.
5. Compare the smallest change that solves the current problem with the
   proposal. Do not add a subsystem only because a mature compiler has one.

## Questions To Answer

- Which layer owns this behavior now, and which layer should own it?
- Is syntax being confused with semantic meaning?
- Is semantic logic leaking into parsing, HIR/MIR repair, codegen, runtime, or
  LSP protocol handling?
- Is compiler magic being added for behavior ordinary GTI/stdlib can express?
- Is proof or restriction machinery being generalized beyond what the named
  outcome needs? Could a sound bounded slice establish the extension seam
  sooner?
- Are invariants, identities, types, ranges, effects, or ownership state
  duplicated across phases?
- Does data have a coherent owner and lifetime? Can pointers/IDs escape their
  snapshot or body?
- Are AST/HIR/MIR boundaries being preserved where they exist, or is a later
  stage compensating for missing earlier facts?
- Does the dependency direction remain frontend -> IR -> backend -> driver,
  with CLI/LSP as adapters?
- Is the change coupling target/build/runtime policy to language semantics?
- Does it improve incomplete-source recovery, diagnostics, testing, and future
  backend/tooling reuse?
- Is any complexity speculative, premature, or inherited from C++/clang rather
  than a GTI problem?

## Classification

Classify findings as:

- **fix now** — correctness, duplicated authority, broken ownership, or an
  imminent feature blocker;
- **systems-ready** — a capability needed by the accepted systems-readiness
  workloads;
- **prepare for** — preserve a seam/interface without implementing the future
  subsystem;
- **defer** — no demonstrated current problem or readiness client.

For an implementation request, make the smallest coherent change and update
the canonical architecture doc. For a review-only request, cite concrete
files/types/functions and keep proposed work under `docs/plans/`. Create an ADR
only when the rationale is significant and durable.

Validate architectural changes with focused structural tests plus the relevant
matrix in
[`docs/architecture/verification.md`](../../../docs/architecture/verification.md).
For implementation work, use the `finish-release` skill after local validation
to commit and initiate any required version/tag/release path. Do not publish a
review-only report, and do not wait for asynchronous GitHub CI/CD after a
successful dispatch.
