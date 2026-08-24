# Optimization

Status: implemented verified local MIR pipeline and optimizer observability
foundation.

GTI optimization has one executable-body authority: optimized MIR. The
frontend still runs a compatibility HIR constant analysis for non-body
representation surfaces and as a shadow check for the existing literal fold,
but no new optimizer pass may use AST shape, source spelling, HIR replacement
tables, emitted C++, or public standard-library names as transformation
authority.

## HIR Constant Analysis

`run(const HirProgram&, OptimizationLevel, TargetInfo)` performs constant
folding at `-O1` and above and stores proven replacements by `HirValueId`.
`checked_integer.h`, `binary_float.h`, and the shared constant evaluator define
fixed-width GTI numeric behavior. Private `llvm::APInt` and `llvm::APFloat`
computations implement those GTI-owned contracts. Integer overflow, zero
division/modulo, invalid shifts, out-of-range conversions, or any uncertain
outcome keep the checked operation rather than becoming a host C++ constant.
Explicit checked-result add/subtract/multiply instead fold to a GTI-owned
expected-state constant containing either the exact integer or the selected
out-of-range error. That operation is non-trapping; it does not weaken the
failure effects of ordinary checked operators.
Binary32 and binary64 literals, unary and binary arithmetic, mixed-width and
mixed integer/floating operations, comparisons, and numeric conversions may
fold only to the exact retained `BinaryFloat` bit pattern in the selected
width. Floating exceptions produce their specified IEEE default result rather
than an integer-style failure.

The C++ representation emitter still consumes this replacement table for
non-body source surfaces such as declaration initializers. It does not control
executable body text. Because one source expression can produce several
concrete HIR values, a source replacement is available only when every
instance has the same value.

## Owned MIR Path

`run(OptimizationRequest)` takes ownership of a `MirProgram`, verifies it, and
returns an `OptimizedProgram` containing both the unchanged canonical
`sourceMir` snapshot and the possibly transformed `mir`. `-O0` schedules no
transform and keeps pre/post MIR structurally identical. `-O1` schedules the
following deterministic local pipeline:

1. `scalar-promotion`;
2. `fold-literal-identities`;
3. `unreachable-block-cleanup`; and
4. `dead-unused-computations`.

`-O2` and `-O3` intentionally run the same GTI pass set until measured
dataflow and interprocedural clients justify distinct middle-end pipelines.
They still select their corresponding native optimization levels.

The fold pass replaces admitted identity chains and exact non-trapping literal
computations, then replaces branches whose dominating condition is a proved
literal. Every fold candidate is compared by `HirValueId` with the
compatibility HIR constant analysis; disagreement blocks the rewrite and is
reported. That shadow result is transitional validation for this pass only.
MIR provenance and transformation evidence remain production authority.

Scalar promotion is the first block-parameter client. MIR blocks may receive
typed scalar, non-owning parameters, and each predecessor edge carries exact
arguments. The pass is confined to mutable, initially uninitialized,
non-address-taken, non-projected scalar places in acyclic bodies. It rejects
owners, classes, references, arrays, closures, storage records, loans, drops,
lifecycle state, raw memory, synchronization, and failure-sensitive bodies.
The canonical rewrite inserts merge parameters and edge arguments, replaces
eligible loads with SSA identities, tombstones eliminated place operations,
and retains source provenance. It is not memory SSA.

Unreachable cleanup removes only blocks independently proved unreachable and
not required by initialization, failure, cleanup, loan, or lifecycle
schedules. Dead-computation removal deletes only unused instructions whose
complete refined effects prove them removable, non-trapping, and neutral for
memory, ownership, loans, cleanup, runtime behavior, user code, and
synchronization.

## Rewrite And Proof Authority

Durable destructive-edit identity is
`{MirBodyAddress, MirInstructionId}`; `MirBlockId` identifies a block.
Vector indexes are implementation details and never appear in optimizer
evidence. `MirProgramEditor` exposes only current client-backed literal,
terminator, place-promotion, dead-instruction, and unreachable-block edits.
Each edit batch validates every guarded target before mutation, rewrites a
copy, repairs derived facts, verifies the complete candidate, and commits
atomically. Destructive instruction removal leaves a canonical inert
tombstone, so instruction/result identities and proof references remain
stable. Unaffected identities are never compacted.

Every accepted edit produces a sorted typed `MirTransformRecord` retaining
pass, transform and proof kinds, affected MIR identities, HIR/source
provenance, exact related identities, replacement payload where applicable,
and required analysis facts. These records are untrusted claims.
`verifyMirOptimizationCoherence` starts from canonical source MIR and
independently replays scalar promotion, literal/compute folds, branch
selection, unreachable cleanup, and dead-computation removal. It then
exact-compares complete MIR structures, including initialization and failure
schedules. Missing, duplicate, unsorted, stale, malformed, or unauthorized
records fail closed.

`LoweredProgramBuilder` consumes canonical source MIR, optimized MIR, and the
coherence evidence before sealing a `LoweredProgram`. Source MIR and
transformation records do not cross the backend frontier. Current backends and
a future LLVM backend therefore receive the same self-contained optimized
representation without reproducing optimization decisions.

## Pass And Analysis Contract

`MirPass` exposes a stable name and a run operation over `MirPassContext`
and `MirBodyAnalysisManager`. An outcome contains its deterministic report,
explicit preservation claim, transformation records, and verification.

The body analysis manager is keyed by stable `MirBodyAddress` plus a
monotonically increasing revision. It caches only facts consumed by current
passes: CFG/reachability and acyclicity, dominance, value uses, exact
instruction effects, and scalar-promotion inputs. A pass explicitly preserves
analyses; an unknown or omitted claim invalidates every cached result
conservatively. No MIR pointer survives invalidation. Loop information, range
and predicate analysis, alias analysis, memory SSA, inlining, and
devirtualization remain absent until a concrete pass needs them.

## Per-Instance Effects

`optimization/effects.h` supplies exhaustive conservative effects for every
MIR instruction, scalar operation, intrinsic, synchronization operation, and
directly callable concrete function, constructor, destructor, and lambda
instance. Summaries cover place and unknown-memory reads/writes, allocation,
runtime calls, traps, user-code invocation, moves, drops, loans,
synchronization, and receiver/argument escape.

`deriveMirProgramEffectSummaries` computes the least summaries justified by
MIR through a deterministic call-graph fixed point. An exact acyclic static
target may refine its call instruction through that proved summary. Recursive
SCCs, virtual dispatch, declarations, native/runtime boundaries, and
unresolved targets remain maximally conservative unless an existing trusted
intrinsic or synchronization identity supplies a complete contract. Source
names never participate.

`verifyMirEffectSummaries` independently derives the least result and rejects
every under-approximation while permitting conservative over-approximation.
These effects are optimization safety facts, not predictions about what the
native compiler happens to remove.

## Observability

Optimization output is opt-in:

```text
--dump-mir-before <path>      canonical pre-optimization MIR
--dump-mir-after <path>       verified optimized MIR
--optimization-remarks       GTI human-readable applied/missed decisions
--optimization-report <path> deterministic versioned JSON report
```

The JSON schema is `gti-optimization-report`, version 1. It includes the
selected level, pass statistics, actionable remarks, invalidated analyses,
stable MIR/source identities, proof kinds, transformation records,
verification, and exact source/emitted/statically-discharged safety-operation
counts supported by current MIR identities. It contains no timing or pointer
data, so identical inputs produce identical output. GTI proof remarks are
labelled separately from native Clang/GCC optimization remarks.

Ordinary compilation captures none of these outputs and remains unchanged when
the options are absent. The reusable driver provides equivalent opt-in pre-MIR,
post-MIR, and structured-report capture without choosing file paths or
presentation.

## Ownership Rules

- Language semantics and checked arithmetic come from the frontend/evaluator.
- Host `float`, `double`, and C++ constant evaluation are not proofs of a GTI
  binary32 or binary64 replacement.
- New CFG, propagation, reachability, use-def, place, and loan passes operate
  on MIR.
- MIR mutations require typed evidence, centralized repair, post-pass
  verification, and independent source replay before backend publication.
- Source provenance and resolved identities must survive transformations.
- Native C++/LLVM optimization remains responsible for target instruction
  selection and machine-level work.

The next planned clients are sparse propagation, predicate/range facts, and
proof-carrying check elimination. They can extend the current pass, analysis,
rewrite, report, and `LoweredProgram` contracts without changing optimizer
ownership or backend inputs.
See [`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md) for the
supporting transform and observability designs.
