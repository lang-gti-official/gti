# ADR 016: Single Lowered Backend Input

Status: Accepted

Implementation note (2026-08-22): implemented. The reusable driver constructs
and verifies one immutable, pointer-free `LoweredProgram` after MIR
optimization. `Backend::generate` accepts only that value; `CppBackend`,
`MirBackend`, and `NativeHeaderBackend` do not receive the AST, semantic model,
HIR, source MIR, optimization result, or generated C++. `LoweredProgram` owns
optimized MIR, target facts, the active resolved declaration tree, symbols,
concrete class/function/constructor/destructor/lambda instances, source/body
identities, and the exhaustive hosted-entry, program-initialization,
structural-operator, callable, lifecycle-cleanup, native-callback, and concrete-
instance generated-item graph. Ordinary C/runtime boundaries remain resolved
ABI declaration/body rows rather than a duplicate generated-wrapper family.

`LoweredProgramBuilder` is the only frontend-aware construction boundary. It
proves source/optimized coherence and exact declaration, symbol, instance, body,
and generated-item inventories before publication. The consumer header exposes
no frontend representation or builder API. C++ declaration/source assembly,
private representation snapshots, whole-program planning, and body emission
all derive from the lowered value; C++ spellings and helper choices remain
backend policy. `BackendInput`, frontend-backed snapshot overloads, and the old
AST/HIR emitter routes have been removed.

## Context

Before this decision was implemented, `BackendInput` carried six coupled
representations: the AST `Program`, the `SemanticModel`, the `HirProgram`, the
optimized `MirProgram`, the pre-optimization source `MirProgram`, and the HIR
optimization replacement table. `CppBackend` re-verified their mutual
coherence on every run and `CppEmitter` consulted all of them during emission.
That shape was useful while AST/HIR body emission was production authority and
MIR families were being proven one at a time, but it left the backend with
multiple authorities: meaning could be re-derived from AST or HIR at emission
time. That is exactly the phase violation the architecture table in
[`overview.md`](../architecture/overview.md) forbids, and every new
representation surface was rediscovered inside the compatibility emitter
rather than stated once.

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
- The compiler realizes the boundary as a `LoweredProgram` aggregate that owns
  optimized MIR beside backend-neutral representation tables. Construction is
  separate from consumption: frontend-aware declarations live in
  `lowered_program_builder.h`, while backends include only the consumer
  contract.
- The backend API is the lowered program plus explicit backend policy. The
  coherence seal guards the frontier between frontend construction and the
  backend rather than requiring each backend to reconcile upstream phases.

## Consequences

- A future backend can inspect the complete lowered contract without linking
  frontend representations or parsing generated C++.
- Unsupported or incomplete lowered representation fails before emission.
  Backends verify the construction seal and inventory but do not reconstruct
  frontend coherence.
- Deterministic printing, verifier mutations, a complete shipped-corpus census,
  backend rejection tests, and an independent contract client guard the
  boundary. Existing MIR, native, runtime, FFI, C++20/C++23, and installed-
  toolchain matrices remain the behavioral proof.
- Internal compatibility with the deleted multi-representation route is not a
  constraint. New target-independent emission facts belong in
  `LoweredProgram`; C++ spelling and ABI syntax belong in the C++ backend.
