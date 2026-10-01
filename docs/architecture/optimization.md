# Optimization

Status: implemented verified MIR pipeline, reusable local dataflow and SCCP,
bounded loop/range and place alias/escape analyses, bounded closed-call safety
summaries, proof-carrying safety optimization families, snapshot-scoped
analysis caching, and optimizer observability.

GTI optimization has one executable-body authority: optimized MIR. The
frontend still runs the earlier HIR constant analysis as a transitional shadow
check for the existing literal-fold family, but its replacement table is not a
backend or non-body representation input. No new optimizer pass may use AST
shape, source spelling, HIR replacement tables, emitted C++, or public
standard-library names as transformation authority.

## Language-Growth Conformance Gate

`optimization/coverage.h` and `optimization/coverage.cpp` own one exhaustive
vocabulary review gate. Each instruction, operation, intrinsic, terminator,
place root/projection, operand, body kind, lifecycle event, synchronization
operation/order, unsafe operation, defined-failure code/detail/propagation,
analysis kind, transform/proof kind, and safety-check family names its existing
bounded consumer, conservative barrier, or structurally inapplicable role.
Both missing-case warning families are errors in that translation unit,
including for enums without `Count`. Count sentinels, the reserved failure code
14, and out-of-range values remain unclassified.

This catalog is **not** another effect table, type system, or transform
authorization. `Supported` means the named consumer covers the family, not
that every instance is optimizable. `Barrier` describes that consumer's
conservative treatment; it does not declare new language effects. All purity,
trapping, memory, ownership, loan, drop, synchronization, and escape facts
continue to come from `mir_effects` and verified semantic/MIR metadata. In
particular, registering a new member does not supply its transfer function,
effect summary, backend representation, or replay proof.

The audit boundary is:

| Surface | Current handling and enforcement |
| --- | --- |
| Instruction/operation/intrinsic effects | Existing count-checked effect tables and exact call summaries remain authoritative. Scalar interpreters whitelist typed operations; all other results are overdefined or conservatively invalidated. |
| CFG, dominance, loops, bodies | Explicit body inventory and terminator routing include normal, invoke, and terminal failure edges. Vocabulary preflight rejects unknown kinds before a default arm can mistake them for exits. |
| Use-def and operands | Stable value IDs and the existing canonical use rebuild include receiver, instruction, terminator, and edge arguments. Copy/move/address/loan operands retain their effects and cannot become generic scalar substitutes. |
| Places, alias, escape, ranges | Structural roots/projections and verified exact call escape facts govern memory reasoning. Unknown projections/roots produce `may-alias`, even across apparently different roots; raw and unresolved indirect memory remains conservative. |
| Failure, cleanup, ownership | Exact outcomes and schedules survive every edit. Only the implemented bounds, storage-state, overflow, and remainder-domain proofs discharge checks; other failure families remain checked and are reported. |
| Analysis preservation | Every current cache has an explicit invalidation owner. Local safety-cache preservation is independent of preserving the closed-call cache; dropping closed-call facts also invalidates all dependent body proofs. |
| Editor, identity repair, evidence | Existing typed edits repair derived identity/use/CFG facts, verify atomically, and replay from canonical source MIR. Transform/proof enum growth also requires explicit coverage. |
| Lowered program and backends | Body verification includes vocabulary admission, so independent replay and the existing lowered-program seal reject unclassified MIR before backend consumption. No backend semantics or new input path is introduced. |

Vocabulary admission runs even when optional full optimizer verification is
disabled. Direct dominance, CFG, SCCP, loop, alias, and safety entry points also
fail closed; callers cannot obtain proofs by bypassing the pipeline. O0 still
runs no transformations and retains identical source/output MIR.

Range transfer also checks authoritative effects before interpreting a
`Compute`: raw/unsafe access, place/unknown-memory writes, user-code invocation,
or synchronization clears mutable-memory facts instead of treating the opcode
as an implicit purity promise. A focused metadata-extension probe verifies
that such an operation cannot preserve a previously provable array read.

`optimizer_conformance` exercises malformed vocabulary, unknown projections,
cache preservation, O0 identity, independent replay, and opaque-call barriers
on an unrelated user record. `optimizer_coverage_growth` compiles temporary
header overlays that add a new member to each of the seven principal language
families; the actual classification translation unit must fail exhaustiveness,
not merely an unrelated effect-table assertion. This guard forces a review on
enum growth; new fields or changed semantics still require focused cross-phase
tests and the ordinary MIR verification contract.

Conformance validation used a Release compiler (AppleClang 21, LLVM 22.1.8,
arm64 macOS). The 49-test optimizer/MIR/lowered/backend/corpus/differential/CLI
selection passed, including independent replay and optimization-report checks.
After the final preservation-mask and remark refinements, the nine focused
conformance/growth/foundation/analyses/safety/runtime/header/backend/pipeline
tests passed again. Formatting checked 383 C++ files. Linux, Windows, and
additional native toolchain versions were not exercised in this local run.

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

The compatibility result is compared with admitted MIR literal folds and is
otherwise discarded before lowered-program construction. Backends never
receive it. Non-body constants and declaration initializers are represented
from canonical semantic and lowered-program facts rather than optimizer
replacement entries. Because one source expression can produce several
concrete HIR values, the shadow analysis reports one source replacement only
when every instance has the same value.

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
2. `exact-receiver-dispatch`;
3. `discharge-bounds-checks`;
4. `discharge-storage-state-checks`;
5. `discharge-arithmetic-overflow-checks`; and
6. `discharge-arithmetic-domain-checks`.

Both levels currently derive the same GTI proofs. They still select their
corresponding native optimization levels. `-O0` and `-O1` do not attach safety
discharges.

### Bounded Exact-Receiver Dispatch

The `interface-exact-dispatch` update workload is the first devirtualization
client. `optimization/devirtualization` proves the concrete receiver from
verified MIR places, confined local loans, initialization dominance, resolved
inheritance/override identities, and authoritative function effects. It does
not infer a closed world from the number of implementations or recognize
public library names. Unrelated user-defined interfaces exercise the same rule.

The admitted receiver is one immutable, complete, initialized local class
object, optionally seen through one non-escaping read-only local interface
loan. Its single direct base is a root interface. The exact source-defined,
read-only override accepts and returns scalars and retains its defined-failure
channel. The object requires no active/custom cleanup. Formal receivers,
mutable or projected objects, multiple/escaping loans, complex inheritance,
borrowed/callable arguments or results, native/recursive/unknown target
effects, allocation, synchronization, and unsupported failure/cleanup shapes
remain conservative. This is a deliberately bounded client, not a general
devirtualizer or interprocedural specialization engine.

`MirEditor::queueExactReceiver` re-derives the proof on a candidate snapshot,
refines the existing receiver borrow to the concrete place, and changes only
the selected target/dispatch and corresponding direct-call propagation fact.
It retains the receiver value and instruction identities, argument evaluation,
CFG/invoke edges, failure origin, allocations, and cleanup schedules. Stable
instruction IDs are not used as a substitute for within-block order. Touched
use facts are rebuilt, the candidate is verified atomically, and all affected
analysis caches are invalidated. Independent optimization coherence replay
re-derives the same `devirtualize-call` / `exact-receiver` record from canonical
source MIR after scalar/branch folding and before safety-check proofs.

The backend consumes the resulting static call and emits an explicitly
qualified override; it does not discover or authorize devirtualization.
O0/O1 do not run this pass. Deterministic missed remarks explain the rejected
receiver, loan, target-effects, inheritance, cleanup, or failure-channel shape.
Existing closed-call safety analysis stays conservative for virtual method
declarations; static dispatch does not independently grant a safety discharge.
The clean `2c8d4561` implementation's paired local-interface benchmark drops
from 223.175 to 120.008 ms without a material control regression. Exact
identities, candidate selection, compiler-time/size/memory controls, and the
arm64 macOS/workload limitations are recorded in the
[performance campaign](../plans/performance-tooling.md#o-measure-02-exact-head-runtime-facility-campaign-2026-10-01).

### Local Scalar Transformations

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

`analyzeMirSafetyChecks` is the canonical safety-proof query. Its baseline is a
body-local abstract interpreter that propagates checked fixed-width integer
intervals through CFG edges, refines those intervals from branch predicates,
tracks fixed-array extents and one scalar element envelope per non-escaping
fixed-array root, and tracks exact initialized slots in private sparse storage.
It is a client of the shared deterministic monotone forward-dataflow solver;
its typed lattice, join, and bounded widening remain safety-analysis policy.
Unknown or opaque calls, raw/unsafe memory effects, escaping roots, aliasing
writes, lifetime changes, and other unclassified mutation invalidate mutable
facts instead of being guessed through. Every canonical normal edge
participates, including a switch default edge; omitted infeasible branch edges
retain their terminator slot identity when they later become executable.
On a comparison edge, the refined interval also carries an edge-local widening
envelope bounded by that comparison. A later loop join may widen the place,
but replaying the dominating true edge restores its proved range before a call
or checked operation. This envelope is derived only from the existing MIR
predicate and exact comparison operand; it is not a relational solver or a
source-name rule.

Block transfer retains a pre-instruction abstract-state snapshot only for the
instruction named by an `Invoke` terminator. Its failure edge consumes that
snapshot; normal edges consume the completed block state. Other intermediate
states have no consumer and are not copied. This is storage/work elimination,
not reduced proof precision, skipped failure analysis, or a cross-run cache.
Independent optimization-evidence replay uses the same unchanged transfer
semantics. An analysis regression contrasts the successful result fact with
the pre-instruction failure-edge fact.

Exact nonzero integer remainder and bitwise-and with one exact nonnegative mask
have dedicated interval transfer. Remainder contributes a result-domain
envelope independent of the changing dividend; the mask contributes
`[0, mask]`. Dynamic fixed-array writes join their scalar value into the root
envelope and never create per-element truth.

For one bounded loop-carried accumulator family, safety analysis first solves
the ordinary lattice, then derives a threshold from the existing reducible
natural-loop forest and alias/effect facts. Every containing loop must have one
literal initialization, one positive literal step, one latch, and a finite
nonnegative upper-bound interval proved at its dominating exit comparison. The
comparison may follow exact source-defined calls only when their signatures,
effects, and closed summaries prove that they cannot mutate or escape the
counter; native, virtual, raw, unsafe, synchronizing, allocating, or otherwise
opaque calls remain barriers. The accumulator must have one dominating literal
initialization and one additive write and cannot escape or have another
may-alias write. The product of the containing maximum trip counts and the
already-proved per-update interval forms a checked GTI integer threshold. A
second run of the same solver widens to that threshold; ordinary branch
refinement supplies the bounded edge narrowing needed by the client.
Irreducible CFGs and arithmetic used to derive an unrepresentable threshold
fail closed. This is not a second solver, general recurrence engine, symbolic
loop optimizer, or call transform.

The admitted operations are deliberately narrow:

- fixed-array scalar reads and writes whose complete index interval is within
  the known extent. A multidimensional access folds every dimension into one
  element place carrying one bounds site, so the proof must hold for the whole
  chain: each nonterminal index is checked against its own sibling array
  place's extent, and an unresolved or unproven dimension anywhere leaves the
  site checked;
- private prefix-storage reads whose storage origin and scalar index origin
  exactly match an earlier successful logical-size check derived from that
  same storage's length, with no intervening storage mutation, index write,
  aliasing write, opaque call, or control-flow path lacking the check;
- private sparse-storage reads and mutable reads whose exact index is both in
  capacity and definitely initialized by a successful construct not invalidated
  by a destroy or mutation barrier; and
- checked integer add, subtract, multiply, and unary negate whose complete
  operand intervals prove the result remains in the declared GTI domain; and
- public logical-size checks whose integer intervals prove
  `0 <= index < size`; and
- integer remainder whose divisor is an exact nonzero value. The proof removes
  only the zero-divisor outcome; the C++ backend still uses a GTI-owned helper
  so signed-minimum remainder `-1` retains GTI's defined zero result instead of
  invoking C++ undefined behavior.

The whole-program `analyzeMirClosedCallSafetyChecks` layer adds one bounded
contextual family. It starts from executable/module initialization roots and
propagates exact scalar arguments and returns, mutable receiver/argument object
fields, private prefix-storage capacity/length/content, and structurally
admitted indexed-container element envelopes through concrete source-defined
static calls and constructors. Indexed elements are admitted only when every
relevant `operator[]` body is verified to return the exact receiver-owned
prefix-storage element loan through one matching field path; public names never
participate. A scalar fact returned by reference crosses the call boundary only
when the returned loan identifies one exact prefix-read producer with the same
source and access. Prefix length is always constrained to the storage invariant
`0 <= length <= capacity`; contradictory abstract facts fail closed. Contexts
join monotonically and use the same typed integer/object widening policy as the
body-local client after four prior per-body context updates. The global
32-round limit and per-body eight-update limit remain fail-closed budgets; a
body exceeding either boundary contributes no contextual proof. Proofs are
published only after convergence. A body may retain contextual proofs
established before a conservative call barrier even when that barrier prevents
the body from exporting a complete return or mutation summary. The barrier
still clears mutable facts for every later instruction. Recursive SCCs, native
or virtual calls, owning-argument transfers, synchronization, raw/unsafe
memory, unsupported cleanup, open call families, and conflicting facts remain
conservative.

Discharged checked `+=`, `-=`, and `*=` may emit the equivalent native C++
compound operator. Remainder and shifts remain on GTI-owned helpers even after
a local failure is discharged because native C++ does not represent every GTI
defined-semantics boundary, including signed-minimum remainder by `-1`.

Array borrows, unproved public logical-size checks, division, dynamic-divisor
remainder, shifts, unsupported conversions, and unproved or mismatched dynamic
indices remain checked. A proved public logical-size check and its matching
private prefix read are discharged independently; proving only the first does
not imply storage identity or initialization. A discharged public check emits
no target operation and its existing failure edge becomes an unconditional
success edge. Prefix append, insert, erase, pop, relocation, storage/index
aliasing, opaque calls, and unsupported joins discard the exact relation.
Place alias facts refine mutable safety state:
duplicate
`must-alias` place IDs share one deterministic fact key, and a write preserves
facts only for places proved `no-alias`; `must-alias` and `may-alias` facts are
invalidated. Alias facts do not themselves discharge a check or infer a
container invariant.

Each proof identifies one exact local defined-failure outcome by family,
origin, code, and detail. The optimizer attaches a `MirSafetyDischarge` to the
instruction but retains the source instruction and failure CFG. This keeps the
proof inspectable and lets each backend select an unchecked representation
only after `LoweredProgramBuilder` has independently replayed and sealed the
transformation. For prefix reads, the C++ backend's unchecked helper is private
representation vocabulary: it has no GTI declaration, intrinsic identity, or
trusted-source spelling and is selected only from the verified discharge.

## Rewrite And Proof Authority

Durable destructive-edit identity is
`{MirBodyAddress, MirInstructionId}`; `MirBlockId` identifies a block.
Vector indexes are implementation details and never appear in optimizer
evidence. `MirEditor` exposes only current client-backed local-fold and
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
It also reruns the canonical safety analysis once for the immutable candidate
snapshot and exact-compares every proof's outcome and related MIR blocks,
instructions, values, and places before attaching metadata. The editor likewise
uses one source and one candidate safety snapshot per batch. It then
exact-compares complete MIR structures, including
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
MIR places. Equal places are `must-alias`; distinct precise roots, including
roots of different local, receiver, global, and temporary kinds, plus disjoint
fields and constant indices are `no-alias`; indirect, loan-rooted,
value-rooted, dynamic-index, raw-memory, or otherwise unresolved relationships
are `may-alias`. Distinct fields of a verified ordinary record are disjoint,
while fields of a union overlap and remain `may-alias`. Escape reasons record
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
instruction effects, scalar-promotion inputs, canonical body and whole-program
closed-call safety facts, place alias/escape facts, loop/range facts, and sparse
constants. A semantic mutation to any body invalidates the program-wide summary
and every derived body safety result. A pass may preserve them only for a
metadata-only change that cannot affect analysis inputs; an unknown or omitted
claim invalidates conservatively. Cache entries have stable addresses and no
interior MIR pointer survives invalidation. Memory SSA, inlining,
devirtualization, and loop or memory transformations remain absent until a
bounded measured client requires them.

Preserving `ClosedCallSafety` alone does not implicitly preserve the local
`SafetyChecks` cache. Preservation claims are valid only when the preserved
analysis's complete inputs, including body-local facts stored in the closed-call
result, remain unchanged; they are not permission to reuse a stale summary
after a semantic edit.

Safety-discharge passes hand the controlled MIR editor that same manager-owned
closed-call snapshot. The editor exact-matches every queued claim against the
canonical proof and may reuse it for candidate replay only when the complete
edit batch attaches safety metadata and changes no semantic MIR input. A mixed
batch independently analyzes the candidate, the standalone editor entry point
independently analyzes its source, and final optimization-coherence verification
still derives a fresh whole-program snapshot from canonical source MIR for any
batch claiming a safety discharge. O0/O1 batches with no safety claim skip that
inapplicable analysis. This avoids repeating the bounded interprocedural
analysis for each safety family without turning cached pass claims into backend
authority.

## Per-Instance Effects

`mir_effects.h` supplies exhaustive conservative effects for every
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

Missed remarks distinguish raw/unsafe memory, open calls, possible
synchronization, unknown alias effects, cleanup/lifecycle schedules, loan
dependencies, ownership transfer, allocation, and trapping. Promotion remarks
also identify escaping places from the alias/escape analysis. The bounds pass
reports general conservative analysis boundaries once and names unsupported
failure-discharge code/detail pairs. These explanatory remarks are not extra
check candidates or claims that a nearby barrier caused an unrelated missing
proof; safety counts and transformation evidence keep their existing meaning.
Unknown facts are reported conservatively, never interpreted as purity.

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
