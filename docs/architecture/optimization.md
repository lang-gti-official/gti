# Optimization

Status: implemented verified local MIR pipeline, reusable local dataflow and
SCCP, bounded loop/range and place alias/escape analyses, proof-carrying safety
optimization families, snapshot-scoped analysis caching, and optimizer
observability.

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

`-O2` and `-O3` add the following deterministic passes between literal folding
and cleanup:

1. `sparse-conditional-constants`;
2. `discharge-bounds-checks`;
3. `discharge-storage-state-checks`; and
4. `discharge-arithmetic-overflow-checks`.

Both levels currently derive the same GTI proofs. They still select their
corresponding native optimization levels. `-O0` and `-O1` do not attach safety
discharges.

The fold pass replaces admitted identity chains and exact non-trapping literal
computations, then replaces branches whose dominating condition is a proved
literal. Every fold candidate is compared by `HirValueId` with the
compatibility HIR constant analysis; disagreement blocks the rewrite and is
reported. That shadow result is transitional validation for this pass only.
MIR provenance and transformation evidence remain production authority.

Sparse conditional constant propagation uses the reusable forward-dataflow
solver described below. It propagates exact typed literals through admitted
scalar, non-owning SSA computations and executable branch edges. Exact
constants become proof-carrying MIR literals and exact branch outcomes use the
ordinary branch-rewrite path. Unsupported operations become overdefined;
unknown branches defer their outgoing edges; switch and invoke routing remain
conservative. The pass does not propagate through places, memory, ownership,
loans, cleanup, trapping operations, or user-code effects.

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

## Proof-Carrying Safety Checks

`analyzeMirSafetyChecks` is the canonical body-local abstract interpreter for
the first check-elimination slice. It propagates checked fixed-width integer
intervals through CFG edges, refines those intervals from branch predicates,
tracks fixed-array extents, and tracks exact initialized slots in private
sparse storage. It is a client of the shared deterministic monotone
forward-dataflow solver; its existing typed lattice, join, and bounded widening
remain safety-analysis policy. Unknown or opaque calls, raw/unsafe memory
effects, and other unclassified mutation invalidate mutable facts instead of
being guessed through. Every canonical normal edge participates, including a
switch default edge; omitted infeasible branch edges retain their terminator
slot identity when they later become executable.

The admitted operations are deliberately narrow:

- fixed-array scalar reads whose complete index interval is within the known
  extent;
- private sparse-storage reads and mutable reads whose exact index is both in
  capacity and definitely initialized by a successful construct not invalidated
  by a destroy or mutation barrier; and
- checked integer add, subtract, multiply, and unary negate whose complete
  operand intervals prove the result remains in the declared GTI domain.

Array writes and borrows, prefix-storage operations, division, remainder,
shifts, conversions, cross-call range facts, and unproved dynamic indices
remain checked. Place alias facts refine mutable safety state: duplicate
`must-alias` place IDs share one deterministic fact key, and a write preserves
facts only for places proved `no-alias`; `must-alias` and `may-alias` facts are
invalidated. Alias facts do not themselves discharge a check or infer a
container invariant.

Each proof identifies one exact local defined-failure outcome by family,
origin, code, and detail. The optimizer attaches a `MirSafetyDischarge` to the
instruction but retains the source instruction and failure CFG. This keeps the
proof inspectable and lets each backend select an unchecked representation
only after `LoweredProgramBuilder` has independently replayed and sealed the
transformation.

## Rewrite And Proof Authority

Durable destructive-edit identity is
`{MirBodyAddress, MirInstructionId}`; `MirBlockId` identifies a block.
Vector indexes are implementation details and never appear in optimizer
evidence. `MirProgramEditor` exposes only current client-backed local-fold and
sparse literal, terminator, place-promotion, safety-discharge,
dead-instruction, and unreachable-block edits.
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
selection, SCCP literal proofs, exact safety proofs, unreachable cleanup, and
dead-computation removal. Canonical source MIR containing a pre-attached safety
discharge is rejected. Replay takes one canonical SCCP snapshot before sparse
edits and exact-compares every replacement literal and related MIR identity.
It also reruns the canonical safety analysis and exact-compares the proof's
outcome and related MIR blocks, instructions, values, and places before
attaching it. It then exact-compares complete MIR structures, including
initialization and failure schedules. Missing, duplicate, unsorted, stale,
malformed, or unauthorized records fail closed.

`LoweredProgramBuilder` consumes canonical source MIR, optimized MIR, and the
coherence evidence before sealing a `LoweredProgram`. Source MIR and
transformation records do not cross the backend frontier. Current backends and
a future LLVM backend therefore receive the same self-contained optimized
representation without reproducing optimization decisions.

## Local Dataflow And Structural Analyses

`runMirForwardDataflow` owns deterministic FIFO scheduling, stable executable
edge identities, update counts, configurable bounded widening, and a hard
iteration limit. Edge identity is the canonical source-terminator slot rather
than the order in which a client happens to emit feasible edges. Each client
owns its typed state, transfer, monotone join, and widening meaning. Malformed
block identities, targets, block-argument arity, duplicate or noncanonical edge
identities, executable-edge retraction, or failure to converge fail closed.
The safety regression suite and an independent FIFO oracle preserve the
pre-migration fixed point; SCCP is the first new client.

`analyzeMirLoops` derives natural loops from dominance backedges, merges
multiple latches with one header, builds a deterministic strict-containment
loop forest, and marks irreducible multi-entry SCCs. Within reducible loops it
records side-effect-free scalar invariants and a bounded single-latch affine
induction family carried by MIR block parameters. An induction requires one
literal entry value, one positive literal add/subtract step, and may retain one
invariant relational branch bound. These facts are cached analysis output; no
loop transformation, symbolic recurrence solver, polyhedral model, or
unbounded widening is implemented. If one predecessor has repeated CFG edges
to the header with different block arguments, the recurrence is ambiguous and
no induction fact is produced.

`analyzeMirPlaceAliasEscape` derives a body-local dense relation over stable
MIR places. Equal places are `must-alias`; precise disjoint roots, fields, and
constant indices are `no-alias`; indirect, dynamic-index, raw-memory, or
otherwise unresolved relationships are `may-alias`. Distinct fields of a
verified ordinary record are disjoint, while fields of a union overlap and
remain `may-alias`. Escape reasons record
address taking, returns, retained borrows, non-local stores, raw memory, and
calls. Only closed exact static call summaries may project receiver or
argument escape; unresolved, native, virtual, and recursive boundaries remain
conservative. The analysis does not assert interprocedural pointer identity,
and raw memory never becomes `no-alias`. Safety-state invalidation is its first
client. There is no MemorySSA or load/store elimination pass.

## Pass And Analysis Contract

`MirPass` exposes a stable name and a run operation over `MirPassContext`
and `MirBodyAnalysisManager`. An outcome contains its deterministic report,
explicit preservation claim, transformation records, and verification.

The body analysis manager binds to one `MirProgram` snapshot identity and is
then keyed by stable `MirBodyAddress` plus a monotonically increasing revision.
It rejects cross-snapshot queries rather than returning a same-address stale
fact. It caches CFG/reachability and acyclicity, dominance, value uses, exact
instruction effects, scalar-promotion inputs, canonical safety facts, place
alias/escape facts, loop/range facts, and sparse constants. A pass explicitly
preserves analyses; an unknown or omitted claim invalidates every cached result
conservatively. No interior MIR pointer survives invalidation. The safety
analysis reuses the manager-owned alias snapshot. Memory SSA, inlining,
devirtualization, and loop or memory transformations remain absent until a
bounded measured client requires them.

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

The same closed exact-summary query is shared with escape analysis; it exposes
only verified MIR facts and never guesses across an open call boundary.

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

The JSON schema is `gti-optimization-report`, version 3. It includes the
selected level, pass statistics, actionable remarks, invalidated analyses,
stable MIR/source identities, proof kinds, transformation records,
verification, exact safety outcomes and related places, and exact
source/emitted/statically-discharged counts per safety family. Counts represent
local defined-failure outcomes rather than whole instructions. It contains no
timing or pointer data, so identical inputs produce identical output. GTI proof
remarks are labelled separately from native Clang/GCC optimization remarks.
Version 3 adds the `sparse-constant` proof/provenance vocabulary; it does not
serialize transient lattice, loop, or alias snapshots.

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

The bounded follow-on milestones are numbered in
[`implementation-sequence.md`](../plans/implementation-sequence.md). New
transforms must consume one of these analyses through typed evidence and
independent replay; analysis availability alone never authorizes a rewrite.
MemorySSA is introduced only when a measured memory transformation cannot be
proved from existing place/effect facts.

See [`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md) for the
supporting transform and observability designs.
