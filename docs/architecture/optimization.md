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

## Runtime Performance Contract

[ADR 012](../decisions/012-outcome-first-systems-readiness.md#runtime-performance-requirement-2026-10-09)
makes efficient execution an architectural and delivery requirement. General
optimization infrastructure may be justified by its operation families, cost
model and information requirements without a prior benchmark or proxy speedup.
Measurements validate the implementation and its tradeoffs, rather than grant
permission to build the architecture. Current conservative analyses remain
sound implementation boundaries, not permanent reasons to defer generality.
This changes development policy, not the implemented pass inventory above.

## Canonical Proof-Evidence Bookkeeping

SCCP and safety analysis maintain their internally produced block, instruction,
value and place evidence IDs in sorted unique vectors. The private
`optimization/evidence_set.h` helpers insert by ordered position and merge by
set union; repeated/subset evidence does not grow or re-sort the destination.
Only analysis-owned canonical sets use this path. External evidence and loan
source lists do not acquire a new canonicality assumption. Public proof
records retain their existing vectors, ordering and complete identities.

SCCP joins detect changes per fact instead of copying the entire destination
state for a final comparison. Equality includes type, lattice
value and all evidence; equal constants with different provenance still merge
their evidence and schedule propagation. Unknown/constant/overdefined rules,
fixed-point budgets and failure edges are unchanged. These are compiler-cost
changes, not new transformations or permission to skip independent replay.

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
| Failure, cleanup, ownership | Exact outcomes and schedules survive every edit. Only the implemented bounds, overflow, and remainder-domain proofs discharge checks; other failure families remain checked and are reported. |
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
4. `discharge-arithmetic-overflow-checks`; and
5. `discharge-arithmetic-domain-checks`.

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

The admitted local receiver is one immutable, complete, initialized local class
object, optionally seen through one non-escaping read-only local interface
loan. Its single direct base is a root interface. The exact source-defined,
read-only override accepts and returns scalars and retains its defined-failure
channel. The object requires no active/custom cleanup. Mutable or projected
objects, multiple/escaping loans, complex inheritance, borrowed/callable
arguments or results, native/recursive/unknown target effects, allocation,
synchronization, and unsupported failure/cleanup shapes remain conservative.
Formal reference-parameter receivers use the closed-world form described
below. Neither form is a general devirtualizer or a specialization engine.

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

### Closed-World Call-Site Facts

`optimization/closed_world` is the first interprocedural fact family and the
closed-world contract that interprocedural work is gated on. A function is
**closed** when every runtime invocation is a visible static MIR `Call` in some
program body. It is **open** when anything else may invoke it: an entry or
hosted-startup root, an embedding export, a native-callback or thread-task
adapter, a `FunctionItem` value, a callable-parameter signature or forwarding
binding, an intrinsic or native-callee call, virtual dispatch (virtual,
override, and pure-virtual methods), non-GTI linkage, or a non-source
definition. Tombstoned references still open a function. The analysis reads
verified MIR only: it never counts implementations, recognizes public library
names, or trusts backend emission. Library programs are closed except for
their embedding exports.

The first fact is the **exact referent class** of a class-reference parameter.
Each visible call contributes to a three-point lattice (unset, exact class,
unknown). An argument borrowed from a binding or temporary object, or a field
of one, contributes its static class, which is its dynamic class: a complete
object or member subobject cannot be a more-derived object. An argument that
reborrows the caller's own reference parameter contributes that parameter's
current fact, so exactness flows through forwarding chains and recursion. A
reference parameter forwards only when its body never replaces it, borrows it
mutably, or takes its address. Every other argument shape, a conflicting class,
a parameter with no visible call, or an unstable reference is unknown. The
solver is monotone and stabilizes after at most `2 * |parameters| + 1` rounds.
It runs once per pass, once more in `MirEditor::apply`, and once in coherence
replay. Each run reads a single program snapshot, and refining a receiver
changes no direct call's arguments or a function's openness.

### Exact Parameter Receivers

The `exact-receiver-dispatch` pass also admits a virtual call whose receiver is
the enclosing function's own reference parameter, when the closed-world fact
gives that parameter an exact class. Inheritance, override selection, target
signature, effects and failure-channel limits are shared with the local form.
The referent's cleanup is irrelevant, because the receiver borrow, its place
and its loans are unchanged. The rewrite makes the call static to the exact
override, records the owner in `dispatchOwner`, and switches failure
propagation to `DirectCall`. The result is an **exact receiver presentation**:
a static member call whose receiver place has a direct interface base of the
target's owner as its type. Semantics never selects such a member.
`isMirExactReceiverPresentation` is the shared admission: the MIR verifier
requires one read-only interface borrow, and the backend emits
`static_cast<const Owner &>(receiver).Owner::method(...)`. The transformation
record is `devirtualize-call` / `exact-parameter-receiver`, with the
`closedWorld` required analysis. Coherence replay re-derives the closed-world
facts from the folded canonical candidate, then replays the record.

On the `interface.gti` runtime probe (an interface helper taking `Update&`,
called with a local `Linear`), arm64 macOS O3 medians drop from 374.97 to
257.02 ms. That equals the local-receiver variant (257.41 ms) and is within 9%
of the direct-call proxy (237.70 ms). Control probes emit byte-identical C++
modulo checkout-path instance digests. GTI-only compile time is unchanged
within noise. See
[performance-tooling.md](../plans/performance-tooling.md#closed-world-exact-parameter-receivers-2026-10-09).

Still conservative: receivers reached through pointers, owning pointees (the
allocated `std::function` models), `this`, base subobjects, local reference
bindings, unknown or mixed callers, and function-item or callback escapes. The
same inheritance limits as the local form also remain.

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
fixed-array root. Sparse and prefix bookkeeping are ordinary source code:
the private initialized-set and prefix capacity/length/content lattices are
retired. General raw lifetime calls and unsupported source state checks remain
conservative barriers; no automatic container-state optimization is claimed.
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
fields and direct-index aggregate element envelopes through concrete
source-defined static calls and constructors. Public container names never
participate. The retired private prefix accessor/borrowed-return profile does
not infer summaries for source-defined raw-address owners. Such calls remain
conservative until their general effects, provenance and value facts are
represented and independently replayable; native optimization is not GTI proof.
Contexts
join monotonically and use the same typed integer/object widening policy as the
body-local client after four prior per-body context updates. The global
worklist schedules only a changed context or a caller of a changed summary,
including each constructor's exact declaration-field initializer dependency.
Every body has a 32-analysis limit and an eight-consumed-context-update limit;
exceeding either rejects that body's contextual facts and invalidates its
callers. These are fail-closed budgets over the finite body inventory, not an
unbounded scan. A rejected body withdraws all its earlier contextual proofs;
canonical replay cannot resurrect them. Rejection does not hide subsequent
independent clients. Long ordinary source chains no longer exhaust 32 whole-program
rounds merely to propagate already fixed facts. The reported iteration count
is the maximum work performed for any one body. Proofs are published only
after the dependency worklist is stable. A body may retain contextual proofs
established before a conservative call barrier even when that barrier prevents
the body from exporting a complete return or mutation summary. The barrier
still clears mutable facts for every later instruction. A permanently
unsupported call is an opaque transfer with a normal continuation, rather
than a pending summary that prevents discovery of subsequent clients. It
supplies no callee result/postcondition facts. Only an admitted source summary
not yet available postpones and retries its caller. A verified terminal source
call is still discovered for analysis of its own instructions, but ends only
that normal path. It never waits for a successful-return postcondition or
prevents summarizing a separate successful branch. Its original failure edges,
cleanup and terminal behavior remain in MIR; no successful result is invented.
Integer comparisons whose two same-domain operands are exact singleton values
also discard their impossible analysis edge. This includes the non-equality
edge that an interval intersection alone cannot represent; it does not replace
or reorder the executed comparison. Native-selected regression branches keep
mutation and alias negatives reachable independently of proved comparisons.
Opaque synchronizing
effects invalidate mutable memory even when the effect summary reports no
direct write; they cannot establish a closed recurrence edge. Recursive SCCs, native
or virtual calls, unsupported owning-argument transfers, synchronization, raw/unsafe
memory, unsupported cleanup, open call families, and conflicting facts remain
conservative.

A concrete source body containing raw/unsafe instructions can now receive its
ordinary closed-call input context. Unhandled raw pointee/lifetime operations
and unsupported unsafe calls remain opaque barriers: they clear mutable-memory
facts and mark the path opaque before ordinary load/store transfer.
This permits replayable proofs before an unsupported operation without
declaring the body effect-closed. Earlier
immutable SSA results and actual readonly scalar binding copies remain
immutable; later referent/global reads receive no stale facts. This preservation
does not apply to readonly reference carriers, projected fields, arrays or
borrowed state. Exact initialization/replacement still invalidates its own
destination. Foreign/open calls, unsupported owning transfers, synchronization and recursive
families retain their separate admission boundaries. An unrelated source
function proves a bounded scalar addition before writing through a raw pointer;
arithmetic on the changed referent and in its caller remains checked.

A bounded address-provenance transfer additionally handles a raw scalar load
whose actual address is an exact MIR `AddressOf` of an existing integer local
or ordinary owned field. It follows typed pointer value copies, assignment,
inert preparation and CFG arguments; joins retain only the same place on
every incoming edge. The pointee type must match exactly. The load consults
the place's current integer fact, not the pointer's lifetime witness, and
retains the actual raw load and unsafe obligation. This does not make the body
effect-closed or justify speculative execution. Pointer replacement follows
the replacement identity; overlapping mutation, raw writes, lifetime operations
and unknown effects retire the existing-place relation. Distinct conditional
addresses and pointer conversions remain outside this transfer. An exact
nonnegative element displacement may preserve address identity, but a nonzero
displacement cannot read the original place's scalar fact.
In particular it is not an initialized
prefix or allocation-freshness proof. These are prerequisites of transporting
element facts for the source-defined storage client, not nominal exceptions
for its public type.

Selected source calls may return the exact address passed to one raw-pointer
formal. The same address lattice distinguishes an existing pointee place from
a symbolic entry-pointer value; the formal's own pointer-variable storage is
never its pointee. Inert copies, parameter reassignment and nested relays
preserve that identity only when every successful return agrees. Summaries
export the formal ordinal, never a callee-local place ID. Applying the summary
rebases evidence to the caller's actual typed operand and call instruction.

The relay must have no opaque, global, unknown-memory or synchronizing effect.
Authoritative `writesPlace` effects also describe callee-local pointer-variable
assignment; a bounded typed-MIR check admits only plain assignment to an exact
by-value raw-pointer formal, without raw access, reference projection, cleanup
or user/runtime code. Other writes retain the barrier. An address escape from
that exact no-pointee-write relay does not itself overwrite the pointee. This
rule provides no initialized region, allocation freshness,
pointer-arithmetic contract or lifetime permission; actual raw reads
and the caller's unsafe obligations remain unchanged.

Wrapping add/subtract/multiply carry an ordinary integer envelope only when
checked evaluation of the complete input envelope proves that no wrap can
occur. Proven add/subtract also reuse the existing entry-relative field rule,
so bounded modular counter updates do not lose their receiver relation. A
potential wrap yields no ordinary-arithmetic fact; it does not introduce a
failure edge or change the modular operation. Canonical replay rederives the
same condition, and actual-wrap negatives reject forged downstream overflow
discharges.

The same returned-input descriptor also transports an ordinary owned pointer
field of a receiver or checked-reference parameter. All successful exits must
return that exact entry address. Return transport and construction postconditions
share caller substitution: the caller must already retain the prepared input's
current typed field-address fact. No field identity is invented after mutation
or an opaque call. Distinct or ambiguous fields, pointee writes, synchronization
and unsupported effects retain checks. This bounded prerequisite does not yet
create field facts absent from the caller's represented places, export an
initialized interval, or associate a lifetime witness with its pointee.

Successful exact integer construction additionally establishes the value at
one typed address: the root identity plus a constant nonnegative element offset.
Only non-byte integer objects participate. This is neither a region extent nor
coverage of intervening or neighbouring slots. Offset addition/subtraction is
checked in the analysis; unsupported, negative or overflowing displacements
yield no identity. The actual pointer operation and its unsafe obligations are
unchanged. CFG joins intersect exact addresses and join their value envelopes.
Each abstract state retains at most 1,024 such addresses; excess distinct
addresses remain unproved. This bounds expansion through source call trees
without treating missing entries as an initialized interval.
Raw writes, unknown effects, synchronization, destruction and potentially
aliasing checked writes invalidate constructed values. A subsequent vacant
structural construction cannot overwrite a previously live non-byte object
under the existing GTI non-overlap contract. It therefore preserves that
object's value; it does not infer disjointness merely from different bases.

An ordinary pointer-bearing object can also publish the scalar contents at one
exact pointer-field value. Extraction requires the stored address to match a
successful scalar-construction fact on every exit. Constructor-result
publication retains that snapshot only at its verified matching destination;
ordinary shell copying, movement and replacement do not transport it. Later
member/checked-reference contexts bind it to the current typed entry pointer
field, checking the integer domain. These mutable-memory facts are distinct
from the shell's own integer fields and are limited to 64 paths per object.
Writes retire transported snapshots conservatively; raw/native/global,
borrowed writes, synchronization and lifetime changes retain their barriers.
An exact later postcondition can establish a new snapshot. No neighbouring
slot, initialized prefix, allocation extent, or pointer-field disjointness is
inferred. The unrelated `Cell` regression retains its actual unsafe load and
checks overwrite, retargeting, native/global aliases, a checked mutable borrow
and destroy/reconstruct negatives with forged-proof rejection.

Exact source summaries export constructed addresses rooted in raw-pointer
formals or an ordinary pointer field of a receiver/checked-reference formal,
relative to the receiver or formal ordinal, owned field path and element
offset. Every successful exit must establish the fact. Failure edges retain
the pre-operation state. A field postcondition applies only when the caller
retains the actual prepared object's current field address; a callee field path
cannot invent an entry value after replacement or unknown memory effects.
An initializer exactly equal to a read-only by-value integer formal can retain
that argument identity through neutral typed staging, including a verified
concrete pack element. Applying the summary substitutes the caller's prepared
argument; safe and overflowing calls to the same helper do not share a single
invented constant. Mutable/replaced formals cannot use that equality. Evidence
is rebased to the call rather than exporting body-local identities.

Symbolic entry-pointer SSA values survive pointee invalidation as addresses
only; read-only independent pointer bindings can retain those same values.
Neither preserves initialized contents, liveness or dereference permission.
Loads consult the exact surviving construction fact, keep their raw access,
and remain opaque.

Within a function, the same address lattice can name the entry value of a raw
pointer field in an ordinary receiver or checked-reference parameter. Only an
owned field path is transparent: nested owned records are supported, but stored
references, unions, arrays, capability backing storage and raw dereferences are
not. Constructor receivers are not seeded before initialization. Repeated exact
paths load the same entry value even when the conservative alias matrix cannot
classify two indirect places as MustAlias; different fields establish no
pointee disjointness. A verified vacant construction cannot overwrite a live
pointer field, so the relation survives that operation. Field replacement,
unknown memory and potentially aliasing writes retire it; a later arbitrary
load cannot recreate the entry relation. Exact scalar construction through the
field can therefore justify subsequent scalar arithmetic, including an exact
raw-parameter initialization helper, without removing the raw load or granting
an unsafe operation new permission.

Current field addresses are keyed by typed owned paths independently of the
body's materialized place table. A getter-only wrapper can therefore relay a
nested field without directly loading that field itself. Function entry walks
ordinary receiver/checked-reference records with a 1,024-node, 32-level budget;
only raw-pointer leaves receive address identities. Exact assignment updates
the stored address, joins retain equal addresses, and overlapping writes or
unknown effects invalidate the map. Constructor receivers, stored-reference
traversal and raw pointee contents are not seeded. This is address transport,
not initialized-region coverage or proof that an incoming pointer is valid.

The same typed address may flow into a verified raw-to-checked reference
formation and its exact source-return relays. The analysis records the actual
pointer operand against the produced loan; it never uses that loan's lifetime
witness as a pointee. Explicit loan transfers and local reference carriers
preserve this identity, while loan endings and unproved writes retire it.
Scalar loads consult the still-current construction/value fact at that address,
not a value cached when the reference was formed. Read-only pointer qualification
preserves identical pointee types; erasure/retyping does not. Unknown effects,
differing return addresses, lifetime replacement and checked writes retain the
normal failure checks. Exact non-writing source address relays may return a
borrow without treating that return escape as a write to the witness.

Exact address summaries also carry a constant element offset or one symbolic
index from a read-only integer formal/owned entry field. Return and construction
summaries share caller substitution, including reordered arguments. Equal
calling intervals do not equate different parameters, and one indexed
construction does not cover holes or its entire observed interval. Preparing
an already evaluated pointer value is an inert argument transfer, even when
its source expression was unsafe; the real producer and all its obligations
remain in MIR. General contiguous initialization coverage remains separate.

This rule and its exact call postcondition do not initialize unknown pointer
contents at entry. Successful vacant construction may additionally retain up
to 64 exact non-byte owned fields already known live at construction time. A
later write to that exact field cannot overlap the newly constructed object.
This relation is captured once, not inferred from a field value learned after
construction. Stored-reference/raw traversal, parent replacement and unknown
writes do not use it. Joins intersect the relations. Call application captures
only caller fields surviving complete input invalidation, before publishing
callee postconditions; a field rebuilt by the callee therefore cannot become
an invented disjointness proof. Callee-local field roots never enter a summary.
Independent readonly pointer bindings retain their address across a checked
referent write; exact replacement still retires the binding's value. Actual
raw loads and unsafe obligations remain unchanged.

The remaining source-owner work needs initialized-index coverage through
repeated calls, record-pointee envelopes, constructor pointer-field transport
and checked returned-element provenance. A lifetime witness is still never a
pointee or an allocation identity. Different pointer fields never imply
different allocations.

Inert typed address arithmetic and pointer conversion neither read nor write a
pointee, so they preserve unrelated memory facts. Apart from the exact typed
offset rule above, they introduce no address, extent, lifetime or
initialized-element proof. Empty full-expression lifecycle
markers can inherit an expression's unsafe classification but do not execute
its operation again; actual lifecycle events and raw accesses remain distinct.
A selected unsafe source function is analyzed under the ordinary exact-call
rules, including its actual raw barriers. Its declaration's caller obligations
remain unchanged. Unrelated record and scalar regressions distinguish pure
address computation and a pure unsafe declaration from a real raw write.
Neutral SSA-value call staging no longer repeats its producer's unsafe
classification. Unhandled raw loads, stores and unsafe calls still reach their
own barriers before that stage. Unrelated ordinary records distinguish inert
address relay from raw reads, raw writes and an unsafe callee's write. The
source-prefix client's original exact bounds-and-read proof is recovered;
raw-backed element envelopes are not implied by this preparation rule.

Exact owner-witness reference formation reuses the verifier's raw-borrow
query. It preserves memory facts because forming the checked loan does not
modify memory; its lifetime/access assertion supplies neither an address
identity nor a pointee value. The result receives no value/field/aggregate
origin from its witness. Checked Borrow/ActivateBorrow/EndBorrow bookkeeping
with no raw access or modifying effects likewise preserves memory, even when
it retains its producer's unsafe source classification. The actual reference
write still invalidates may-alias storage. No loan schedule is removed or
reordered. Unrelated user-defined witnesses exercise read-only and mutable
formation, mutation through the alias, and a different pointee under the same
witness; forged arithmetic proofs for the latter two are rejected.

The trusted scalar bounds detector has no memory transfer: its exact void
call takes two staged `uint64_t` values, compares them and may raise its defined
failure. Successful execution therefore preserves unrelated scalar/object
facts and does not make a source body opaque. Its existing Invoke failure edge
still uses the pre-instruction state. This is not a source-container length or
initialized-element invariant; those require independently represented facts.
An unrelated record regression distinguishes the pure detector from an
unknown field, conflicting field writes and an unreached call context.
The existing one-boolean defined-failure detectors likewise preserve unrelated
facts: their input condition is source-computed, not a container invariant.
On the successful Invoke edge, the same comparison refinement used by ordinary
branches can consume that predicate. A bounded 32-stage walk follows only
inert boolean value staging, identity and negation; arbitrary source calls or
loaded boolean bindings do not become predicate contracts. The failed edge
receives no successful-predicate refinement. Evidence retains the detector,
comparison and staged identities for independent replay. Exact later writes
invalidate the place's earlier bound. This does not invent a relation between
unknown container fields or initialize a raw region.

An exact builtin-integer result-only `Move` copies the source's scalar envelope
into an independent SSA result while invalidating the source's place and
derived origins. It changes no ownership events: source reads after movement
remain invalid, and reinitialization establishes a new envelope. A uniquely
identified, unprojected by-value integer formal has independent parameter
storage; moving it may preserve typed non-byte owned-field facts on an implicit
receiver. A reference's projected integer type is not that proof. Stored
references, arrays, unions, capability carriers and unresolved paths receive
no exemption. Moving a receiver field invalidates that field and overlapping
paths, not safe siblings. Ordinary user records, nested relay methods and
overflow after parameter/field reinitialization exercise this transfer without
container names or changes to call-input admission.

Exact integer-scalar final-address construction has a narrow transfer profile.
Shared MIR effects retain its unknown-memory write and initialization, but do
not invent a selected user constructor or synchronization. Construction still
requires the unsafe caller to provide a vacant, nonconflicting complete extent;
it cannot implicitly replace another live non-byte object. Consequently,
existing live non-byte integer facts along exact ordinary owned-field paths
may survive this operation. Byte fields, arrays/indexes, union overlap, stored
reference traversal, compiler-capability backing carriers, unresolved roots
and loaded-origin/element facts are discarded. No pointer validity, region
extent or fresh allocation is inferred. The exact scalar construction rule
above can independently publish the newly constructed object's value.

That same profile admits exact generated copy/move construction of ordinary
scalar-field records into vacant final storage. The shared MIR-effects query
reuses the verifier's complete construction shape and the same recursive
generated-record transport classifier used for prepared values. A selected
source constructor, replacement, unsupported state or extra raw access cannot
claim this profile. Only represented transfer-out events may accompany the
construction; argument staging and actual cleanup retain their own effects.
It preserves the same unrelated non-byte live-field facts, not a value envelope
for the newly constructed region. Source wrappers can relay this profile;
destroy/reconstruct still invalidates the old object's facts.

The interpreter records whether a successful path contains only that supported
opaque lifetime transfer or an unknown live-memory barrier. Reachable joins
retain the stronger barrier. Exact source wrappers can forward the supported
profile without losing unrelated live-field facts, but remain opaque and cannot
establish recurrence invariance. Normal global, mutable-reference and receiver
writes still invalidate their protected origins before proved postconditions
are republished. Actual raw writes, unsupported reads, destruction, relocation,
unsupported calls and synchronization retain full invalidation. Failed paths do not become
successful-path postconditions. Unrelated public-wrapper, nested-object,
byte/array and destroy/reconstruct controls exercise independent proof replay.
This profile does not yet recover the outstanding raw-backed element-envelope
or logical-bounds migration requirements.

Ordinary class construction and owned class returns transport independently
proved scalar field envelopes, including parameterized member initializers.
The analysis tracks an implicit receiver when MIR contains `this.field` places
without a whole-object `this` place. This analysis-only root never becomes a
MIR or published proof identity. Exact receiver field writes invalidate
overlapping paths; raw/unknown memory effects discard the state. Nested class
facts are relative to the selected subobject, not its enclosing owner's field
path. Constructor destination publication and return summaries retain those
same facts after sanitizing them against the exact concrete type. These are
ordinary class facts, not initialized raw-allocation or container invariants.

Shared MIR effects classify exact generated copy/move of ordinary scalar-field
records, recursively through owned record fields. The bounded profile excludes
borrowed state, compiler backing capabilities, overlapping or inherited layouts,
payload enums, arrays and any non-generated selected transfer. Raw pointer fields
are transported as addresses only; they provide no pointee or alias facts.
Generated movement does not execute a source destructor; actual cleanup remains
a separate operation. Cleanup of a recursively scalar record with no source
destructor has exact storage/lifecycle effects, while real destructors and
unsupported cleanup keep their existing effects and barriers. The optimizer
uses these shared classifications, not a parallel type or library-name rule.

Admitted prepared by-value inputs carry independent field snapshots, without
retaining an alias back to the copied/moved source. A later argument's mutation
therefore cannot alter an earlier prepared copy. Movement invalidates its source;
the result may transport the old value but never makes the source available
again. Exact by-value parameter storage is independent of the receiver's owned
fields, including nested fields without a materialized whole-object receiver.
Only those typed non-byte owned-field facts survive parameter-local structural
transfer/cleanup; stored references do not gain this exemption. Callee by-value
parameter mutations are never republished as caller postconditions. Only checked
reference parameters use that outward path. Ordinary nested records, argument
ordering, a mutating copy and an actual user-destructor control exercise these
rules with independent proof replay. This supplies no raw initialized-element
coverage or returned raw-backed element envelope.

The same drop classification admits a structurally passive result shell:
every `expected`/`unexpected` alternative is a scalar or an ordinary generated
scalar-field record (with `void` allowed only as the success alternative).
Nested result shells recurse with a 1,024-node budget. User destructors,
borrowed payloads, owning capabilities and unsupported layouts still fail
closed. This does not expand generated copy/move admission. Shared effects
retain the drop's storage/lifecycle operation and never mark it removable.

For such a passive drop, an unprojected, non-borrowing local binding with an
exact owning initialization is independent of ordinary receiver-owned fields.
The optimizer can retain those non-byte field values while retiring the local
storage; it does not keep pending result correlations, pointer-region facts,
or arbitrary aliases. Initially available inputs retain the separate exact
by-value formal rule. The scoped `Counter` regression checks both void and
record results, while a result carrying a custom destructor still performs its
native mutation and reports the later arithmetic overflow.

Exact readonly source members may also return a still-current ordinary owned
integer field. The interpreter records its typed receiver-relative path only
when every successful return identifies the same field. Nested readonly
forwarding preserves that identity. Inert SSA identity/value staging and block
arguments transport it; joins retain only identical origins. Comparison with
an exact integer refines both the independent SSA value and its current field,
so a getter-based loop can establish its exit field bound. A proved nonwriting
getter also preserves a caller's narrower field interval rather than replacing
it with the callee's joined calling-context interval.
Refinement updates every existing exact owned-subobject representation of that
field, as well as represented scalar places. A nested receiver summary cannot
overwrite a loop-exit refinement with its older, wider interval when the
enclosing object is extracted. This rule also applies to unrelated nested
records; it does not infer aliasing through a stored reference or raw pointer.

Field identity is not numerical equality or a persistent borrow. Overlapping
writes, ancestor replacement, movement, raw/unknown memory and lifetime
barriers retire the relation without changing the earlier scalar value. Exact
disjoint owned-field writes retain it and can recover that field's correlated
interval after conservative whole-root invalidation. Stored references, arrays,
unions, byte fields and capability backing paths are excluded from this bounded
profile. A scalar copied into a local binding does not retain receiver identity;
mutable getters and by-value object inputs do not export this return contract.
Implicit receiver roots remain analysis-only and never appear in proof records.

Successful ordinary member summaries additionally retain bounded entry-relative
integer-field updates: one exact owned receiver field equals its own entry
value plus a proved constant offset. The relation is derived from typed loads,
same-domain add/subtract, inert SSA staging, exact field assignment and nested
closed calls, never by subtracting interval endpoints. It remains useful when
different invocations have different starting values. Four calls that each
increment a counter once therefore preserve four increments rather than using
one joined absolute postcondition at every call. The source prefix owner's
repeated append length is a client of this general rule.

Only the existing non-byte ordinary owned-field profile participates. Entry
identity does not require a known numerical value; traversal is bounded to
1,024 visited type/field nodes and depth 32, with excess paths omitted. Offsets
must fit checked signed 64-bit arithmetic, while applying an update still uses
the field's actual checked integer domain. Every successful exit and CFG join
must agree on field, domain and offset. Conditional/differing updates, arbitrary
replacement, writes through unproved paths, movement and unknown/raw/lifetime
barriers retire the relation. Distinct field IDs are not disjointness evidence
for stored references. The existing independently proved vacant-construction
and by-value parameter-storage rules can preserve eligible owned fields.

At an exact call the update transforms that caller's current field facts, with
evidence rebased to the call rather than exporting callee-local MIR identities.
Nested wrappers compose only exact same-field updates. Existing absolute
postconditions can retain an independently proved widening envelope; exact
constructor-initializer values carry a stable entry envelope, unlike arbitrary
loop-produced singleton values. Closed non-writing, non-synchronizing and
non-escaping calls do not erase caller memory merely because their numerical
postcondition is absent. Opaque calls retain their barriers. No instruction,
failure edge or cleanup schedule is removed by these summary rules: even a
net-zero update cannot erase an overflowing intermediate operation. Independent
canonical replay derives the relation again; O0 remains transformation-free.

Fallible source-call summaries may partition ordinary owned scalar-field
postconditions by the returned result alternative. The first structural
observer profile is MIR's two-alternative `expected` result: a successful
payload/void return and an `unexpected` return are distinct exits. Forwarding
an already summarized result retains its alternatives; an unknown result joins
both possibilities. No public library identity supplies these facts. Checked
reference arguments and receivers map exact owned paths to the caller's
canonical field roots, including a reference-taking forwarding helper.

A fresh independent local result shell and its loads can retain that guarded
relation. `ExpectedHasValue` on the still-current result publishes only the
selected alternative's fields, with call/branch evidence. At joins, unknown
results lose the relation; common alternatives join their field envelopes.
This is bounded to 64 result identities and 64 fields per alternative, with
excess facts omitted. Writes (including exact indexed writes), moves, drops,
structural construction and unknown/native/synchronizing effects retire the
pending memory correlation conservatively. An earlier status value cannot
restore a field after mutation. This profile does not transport payload values,
allocation freshness, initialized-region coverage, or other sum representations.
The unrelated `Ticket` regression covers success/error forwarding and stale
results after direct, source-call and native mutations. Forged safety metadata
is independently rejected; O0 operations and failure scheduling are unchanged.

CFG argument transfer snapshots every incoming fact before publishing any
destination block parameter. This includes integer intervals, loaded/field
origins, entry-relative updates and actual addresses. A backedge may swap
parameters; sequential publication would lose an old input and could produce
an unsound safety proof. Only the argument facts are copied, not the whole
abstract memory state.

Context discovery is also bounded by complete summaries: an earlier
source-allocation or raw object-lifetime call can prevent completion of a
required closed-call summary, leaving later call contexts on that path
undiscovered even when their local work is independent. Proofs established
before the barrier remain valid. The current missed-optimization remark for
this case is the generic `check retained: canonical MIR facts do not prove
arithmetic-overflow safety`; it does not yet distinguish an allocator barrier
from other missing contextual facts.

At a complete closed-call boundary, a scalar or object result summary is not
proof that other caller storage was unchanged. Mutable reference/pointer
inputs and mutable or consuming receivers invalidate their protected origins
before independently proved object postconditions are republished. One bounded
source walk follows staged `CallInput`, `Load`, and identity values and reuses
the MIR loan's physical source and stored-owner dependencies. Missing,
unsupported, or cyclic identity fails closed. Invalidation expands aliasing
once from the actual origins; a `may-alias` relation is not transitive.
A verified reference's pointer representation is not an additional unknown
origin: its loan supplies the protected sources. A reference to a borrowed-state
object still retains both the physical object and its external owner dependency.
Calls with place writes also invalidate represented global-symbol facts.
Interpreted direct global writes and transfers carry a successful-path global
write barrier through ordinary call summaries and constructor declaration-field
initializers, including writes through checked local aliases/reborrows. The
barrier clears object/field correlations and possible referent facts even when
the written global is absent from the caller's place table. Independent scalar
local/parameter storage and earlier SSA values survive; actual raw/unknown
effects still use full invalidation. This private transfer flag is not an exact
written-global set: current MIR effect summaries do not enumerate that set.
Recurrence
proofs likewise treat possible global writes and synchronization as barriers.
The existing closed-body admission remains authoritative: supported private
storage and numeric-conversion effects are interpreted by that analysis, not
mistaken for opaque calls solely from their coarse effect traits. Native,
unknown, unsafe, and unsupported call boundaries retain their conservative
behavior. None of this transports a new scalar-reference postcondition or
adds a second effects model.

An unhandled call or an unknown-effect compute, drop, or other instruction
both clears mutable-memory facts and marks that abstract execution path as
opaque. This marker is retained at control-flow joins; any successful exit
carrying it prevents an effect-closed summary or recurrence proof. A separately
proved return value may still be transported, but is never evidence that caller
memory was unchanged. Caller invalidation still uses the authoritative exact
invocation effects; opacity is not a separate source of write authority.
Returning a constant after the barrier does not restore
effect closure. Cleanup confined to a terminating failure path does not taint
the distinct normal-return summary. Explicit atomic synchronization and runtime
intrinsics whose effects lack a supported transfer rule have separate negative fixtures;
an interpreted private-storage read is not evidence that other runtime calls
are safe to cross.
Current authoritative effects classify some native-only owning cleanup as
opaque, including a cleanup with no source destructor target. A caller cannot
retain earlier mutable object facts across that boundary merely because the
callee's scalar return is known. The record-envelope controls cover this
conservative case separately from an exact chain whose object is constructed
after the barrier. Refining native cleanup effects requires its own structural
proof in `mir_effects`; the safety analysis does not infer one from a library
name or emitted deallocation code.

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
MemorySSA or another memory-version representation may be introduced when
general memory transformations need facts existing place/effect analysis
cannot express soundly and maintainably. Explain its consumers and invalidation
rules; no prior measured speedup is required. Validate the resulting production
path and performance separately from transformation correctness.

See [`docs/plans/optimization.md`](../plans/optimization.md) and
[`docs/plans/performance-tooling.md`](../plans/performance-tooling.md) for the
supporting transform and observability designs.
