# ADR 016: Single Lowered Backend Input

Status: Accepted

Implementation note (2026-08-22): the source-body portion is complete and the
first production `LoweredProgram` boundary is implemented. The reusable driver
constructs and verifies that immutable, pointer-free value after MIR
optimization, and `MirBackend` is the first client that reads only this
contract. It currently owns optimized MIR, target layout, an active declaration
tree with resolved declaration payloads, a complete semantic symbol vocabulary,
concrete class/function/constructor/destructor/lambda instance metadata,
source/body identities, and the exact hosted-entry, program-initialization, and
native-callback generated-item graph. The C++ representation consumer,
remaining generated-item families, and native-header cutover remain in
progress; transitional `BackendInput` fields are therefore still present.

## Context

`BackendInput` currently carries six coupled representations: the AST
`Program`, the `SemanticModel`, the `HirProgram`, the optimized `MirProgram`,
the pre-optimization source `MirProgram`, and the HIR optimization
replacement table. `CppBackend` verifies their mutual coherence on every run
and the transitional `CppEmitter` consults all of them during emission. This
was the correct shape while AST/HIR body emission was the production
authority and MIR families were being proven one at a time, but it leaves the
backend with multiple executable authorities: a body's meaning can still be
re-derived from AST or HIR at emission time, which is exactly the phase
violation the architecture table in
[`overview.md`](../architecture/overview.md) forbids, and every new
representation surface (naming, layout, declaration ordering) is currently
rediscovered inside the 15,000-line compatibility emitter rather than stated
once.

The backend-authority migration needs a stated end state so that emission
work converges instead of accreting more cross-representation consultation.
An external architecture review of the migration reached the same
conclusion and asked for the decision to be recorded rather than scattered
through the MIR architecture document and the implementation plan.

## Decision

Executable C++ generation converges on one immutable lowered-program input.

- The lowered program contains the verified optimized MIR bodies;
  deterministic backend-neutral declaration, symbol, type, instance, source,
  layout, ABI, ordering, and generated-item facts; canonical failure metadata;
  and the program initialization and hosted-entry plans. C++ names, type
  spellings, include selection, and source syntax remain C++ backend policy.
  The private `CppMirRepresentationSnapshot` and `CppMirProgramPlan`
  components are migration inputs whose target-independent facts move into
  this compiler-owned contract; they are not the final API.
- AST, semantics, and HIR remain upstream compiler stages with unchanged
  ownership: syntax, resolved meaning, and concrete instance discovery. They
  stop being consulted by executable body emission. HIR is not deleted; it
  stops being a backend execution authority.
- During the migration, `BackendInput` keeps its transitional fields, and the
  compatibility emitter remains the authority for bodies MIR does not yet
  own. Every migrated surface must move consultation out of AST/HIR and into
  the lowered tables; new emission code must not add AST/HIR consultation.
- At cutover, the backend API narrows to the lowered program plus target
  policy, and the coherence seals that currently guard six representations
  reduce to the frontier between the frontend and the lowered program.

Whether the lowered program is realized by extending `MirProgram` with the
representation tables or by a thin `LoweredProgram` aggregate that owns them
beside the `MirProgram` is a naming decision deferred to the first change
that materializes the tables in production; this ADR fixes the boundary, not
the spelling.

## Consequences

- Emission progress is measured against one convergence target: the set of
  facts a body's emission still reads from AST/semantics/HIR is its remaining
  migration debt.
- The differential oracle against the compatibility emitter remains
  regression evidence only. The compatibility emitter knowingly violates
  evaluation-order, temporary-cleanup, initialization, and defined-failure
  contracts, so agreement with it cannot be the primary correctness proof;
  specification traces, MIR mutation tests, cleanup and failure invariants,
  and the runtime matrices stay primary.
- Representation-table extraction is ordinary refactoring of the
  compatibility emitter and must not change emitted bytes while both paths
  coexist; the oracle and the family runtime gates hold that line.
- The multi-representation coherence checks in `CppBackend` are retained
  until cutover: they are the guard that the transitional inputs agree, not a
  design goal of their own.
