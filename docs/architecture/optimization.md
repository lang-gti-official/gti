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

Measurements and validation runs below are recorded evidence for their named
checkpoint/workload/platform, not a fresh certification of every later head or
all supported toolchains. Current source and tests define the implemented
profiles; a new change still requires its own correctness and performance
acceptance.

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

The SCCP solver's dense per-block states hold shared immutable facts rather
than one fact copy per block. A transfer or join that leaves a value's fact
unchanged keeps the existing allocation, and a join that adopts the incoming
fact shares it; the stored facts are never mutated in place, so every state
exposes exactly the contents the copying representation produced, and the
public `MirSccpAnalysis` is unchanged. This bounds the solver near the number
of distinct facts plus one pointer per block and value instead of B*V complete
facts, each carrying a semantic type and three evidence vectors. Measured on
the `stdlib_surface_json.gti` fixture, whose `main` body has about 63,000
blocks and 600 values, the pass fell from about 18.5 GiB of transient footprint
and 36 s to about 0.7 GiB and 2 s, and the whole `-O3 --emit-cpp` compile from
roughly 11 GiB peak resident memory to roughly 3 GiB with byte-identical C++.
The remaining peak is the safety discharge analyses' per-block abstract states
on the same body; `json_stdlib` guards the optimized compile's peak resident
size with generous headroom.

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

`dead-unused-computations` removes only a neutral `Compute` whose result has
no MIR use. One consumer lives outside the body's use model: a constructor
record publishes an unstaged field initializer, one that no `Initialize`
instruction carries, from the MIR value its single HIR argument defines, and
the backend writes the field from that value. A literal-backed
`std::string_view` field default is the common instance, and a user-defined
borrowed-state field initialized from any constant takes the same path.
`mirRecordPublishedConstructorInitializer` is the one rule naming those values;
the canonical dead-instruction rewrite refuses them, so the editor, the pass
and coherence replay agree, and the pass reports the retained value as a
missed candidate rather than silently skipping it. The C++ emitter publishes
from exactly the same query. Before this rule, `-O1` and above removed the
literal and every such field read back empty.

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
names, or trusts backend emission. The same analysis applies to dormant library
artifacts: exports are open roots, and other adapter/item/dispatch escapes still
open their targets. Artifact kind alone does not prove a function closed.

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
  `0 <= index < size`;
- public logical-size checks of an unsigned index that a dominating comparison
  orders strictly below the size. A comparison of two values of one integer
  domain records an order on each edge it takes, for `<`, `<=`, `>` and `>=`
  alike. An order relates immutable MIR values, so no write retires it;
  redefining either value in a later loop iteration does, and a join keeps
  only the orders that every incoming path establishes, strict only when all
  of them are. A check matches an order of its own domain when its operands
  denote the same current scalars: one value through inert staging or an
  identity, loads of one place that no possibly aliasing write or barrier has
  retired, or one still-current owned field. The size may instead be a checked
  addition of a proved nonnegative amount to the ordered size, because that
  addition either produced the larger value or failed before the check. A
  state keeps at most 64 orders. This is RT-5's body-local slice: the
  comparison and the check share a body, so a caller's loop condition does not
  reach the check inside an indexing accessor it calls; and
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
and unsupported effects retain checks. This address-relay rule does not by itself
create field facts absent from the caller's represented places, export an
initialized interval, or associate a lifetime witness with its pointee. The
separate constructed-cell and bounded prefix profiles below supply their own
content proofs.

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
The sealed zero-argument integer construction establishes zero at that same
exact address, including through an ordinary source wrapper. It does not infer
the contents of vacant bytes; explicit-value construction retains its selected
value, and wrong-address, native-write and destroy/reconstruct controls retain
their arithmetic checks.
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

Direct stores through ordinary owned receiver fields invalidate the selected
subtree but preserve scalar facts for disjoint siblings. Materialized receiver
places and the analysis-only implicit receiver obey the same rule. This does
not extend to stored-reference or raw dereferences, unknown/native effects,
pointee snapshots or initialized-element summaries. An unrelated source
`Ledger` exercises publication through a nested member followed by a sibling
store, with direct/borrowed/raw/native mutation negatives and canonical replay.

A checked non-byte integer store through an exactly relayed address updates
that cell's value on the successful edge. Compound assignment reads the same
current cell fact before computing its checked result. The store cannot rebind
live pointer/reference carriers, but retires other possible pointee aliases;
it does not certify a region or neighbouring cells. Its call summary remains
an unknown live-memory mutation, not vacant construction: caller memory is
invalidated before the exact postcondition is applied. Raw/native writes and
unresolved addresses retain the conservative path. The checked-address-store
fixture tests replacement, compound assignment, overflow, native mutation and
a distinct target, including forged-discharge rejection and O0 parity.

Checked non-byte integer stores additionally preserve facts about live ordinary
scalar fields of a different integer width. This relies on GTI's exact live
pointee and non-overlapping-object obligations, not the C++ backend's aliasing
rules. Equal-width types (including signedness variants and transparent/native
aliases) receive no disjointness inference. A source-call profile replays all
operations and nested selected source calls, admitting one nonlocal integer
write width while excluding independent scalar locals. Raw/native, byte,
aggregate, unknown and lifetime-changing writes, unsupported calls and
synchronization reject that profile. Only owned non-byte scalar field facts
survive; no region, allocation or initialized-element fact follows. The
unrelated metadata fixture covers direct and called writes, with same-width,
raw, native and destruction/reconstruction negatives and forged-proof replay.

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
remain in MIR. The bounded initialized-prefix coverage described above is an
independent content relation; an address-offset summary alone supplies none.

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

Exact source-call summaries also intersect ordinary owned pointer-field values
across all successful exits. Callers substitute the actual input addresses
before applying effects, then republish only those independently proved output
field identities. An unobserved local field can receive a call-site-versioned
current-value snapshot; it establishes neither freshness, extent nor initialized
contents and cannot be exported as the function's original input identity.
Reobserving that call site after invalidation retires its earlier pointee facts,
including when the call executes repeatedly in a loop. Retargeting, native/raw
writes and differing successful exits cannot recover an obsolete cell value.
Unrelated pointer-holder records exercise both direct and member-mediated
construction, invalidation, repeated observation and forged-proof rejection.

Exact generated construction of ordinary scalar-field records can publish
separate integer facts at their typed subobject paths. A checked borrow of the
record, including a subsequent field borrow, preserves the actual pointer plus
that path; the witness does not supply the address. Nested ordinary owned fields
are supported, but union overlap, capability backing, base-class traversal,
stored references and array indexing do not enter this profile. A checked field
write can retain disjoint sibling subobject facts at the same exact address.
Unknown/native writes and destructive lifetime changes still retire them.

These construction postconditions may describe exact equality to an owned
integer field of a readonly by-value record formal. Caller substitution reads
the actual prepared argument's field, not the joined interval of all calls to
the helper. Concrete parameter-pack elements use their verified occurrence in
the flattened signature, shared with ordinary scalar-formal identification;
the source pack binding is expanded through its verified concrete element
types, including zero elements, before matching an invocation ordinal.
Equal element types do not make different occurrences interchangeable. Passive
cleanup of independent by-value parameter/local storage can retain already
constructed cells at original entry addresses, not arbitrary observed local
addresses or the retired object's own storage. General source-constructor
argument-to-field equality remains a separate goal.

Closed source append operations can also establish compact initialized-prefix
coverage. The relation names the current owned pointer field, its current
endpoint field, and an optional typed scalar subobject path. Every successful
exit must construct at the old endpoint, advance it by exactly one without
wrapping, and preserve the pointer field. The typed mutation profile must
exclude writes at the payload's integer width; unsupported, native, destructive
and synchronizing effects retain their barriers. This is neither a nominal
container rule nor an allocation/capacity/freshness assertion.

A zero endpoint supplies vacuous coverage, not readable elements. Loop joins
intersect coverage and join value envelopes; missing nonempty-path coverage
retires the relation. Its storage budget depends on field relationships, not
element count. Exact-address reads, including readonly formal index intervals,
require their entire interval to lie below the proved endpoint. Nonwriting
closed calls, checked reference-carrier publication and independent scalar
local/parameter writes preserve these relations; other mutations retire them.
An unrelated 5,000-element sequence exercises first/last and varying-index
reads, with forged-proof and native controls for gaps, conditional construction,
different values, retargeting, native writes and destructive replacement.

A checked non-byte integer assignment can retain coverage at the same
current pointer base and exact scalar subobject. It weakly joins the stored
value with the previous prefix envelope: assigning one element does not imply
that every element acquired that value. The endpoint must have a different
integer width so the existing typed-store rule preserves its live metadata.
Compound arithmetic uses the same exact-address/region query as a separate
load and arithmetic operation, including its body-local parameter identity.
Only a proved result envelope may update coverage. No disjointness is inferred
from different pointer fields; unmatched prefixes, unknown results and
unsupported mutations lose coverage. Prefixes for an owned record field that
neither contains nor lies within the written subobject describe other storage
of every element and stay unchanged.
Native effects and destructive replacement retain their ordinary barriers.

Verified structural argument staging writes a fresh independent temporary,
not arbitrary caller memory. Its invalidation retires that slot's old local
facts without discarding unrelated live-region facts. Successful source
constructor-result publication uses the same rule only after applying the
constructor's actual effects. This does not make the constructor pure or
preserve facts across its unknown/native writes. Generated movement and passive
cleanup of independent value parameters/temporaries can also preserve owned
receiver fields and entry pointer identities; retargeted or existing-place
pointee identities remain excluded. The typed mutation profile accepts exact
concrete-pack value parameters and passive prepared temporaries, not borrowed
projections or active destructors. An unrelated record sequence exercises these
rules without a container-name exception.

Guard predicates contribute independent widening thresholds rather than
preserving an earlier contextual singleton as a loop invariant. A threshold
which itself expands on a widening edge is discarded. This lets affine length
updates converge without enumerating the trip count or enlarging analysis
budgets, while preserving bounded constructor/getter-loop proofs.

A returned element whose index is a caller-local value, such as a loop
counter, has no exact symbolic address. The caller instead records an address
region for that loan: the exact substituted base and the index's current
interval. A region answers initialized-prefix envelope reads and
region-preserving writes over its whole interval. It never names one exact
cell, so it never publishes or consults constructed-scalar, field-address or
retained-loan identities. Regions share the invalidation of exact borrowed
addresses; at joins an exact index-free address becomes its one-displacement
region, and region intervals join and widen like other integer facts.

Closed-call transport recognizes three further general shapes:

- **Constructor-result adoption.** A binding initialization that reparents a
  constructor result's own drop obligation adopts that SSA object, so the
  constructor's exit facts, including initialized prefixes, stay attached.
- **Frame-local callees.** A complete summary whose typed-store profile has
  no store width writes only the callee's own frame: independent locals,
  local transfers and drops, and equally closed callees. Such a callee relays
  its exact returned borrow and preserves caller memory, even though MIR
  effects report its local writes. Invoking exact closed callees is not itself
  a write to a tracked loop counter or accumulator; a by-value input carrying
  borrowed state, such as a closure capturing a reference, keeps that barrier.
- **Source construction in final storage.** Final-address construction that
  runs an exact source constructor of an ordinary record applies that
  constructor's closed summary. Its exit field facts become constructed cells
  at the exact pointee, and vacant storage cannot overwrite a live object.
  Constructor and field-initializer bodies count their own vacant receiver
  initialization as no live-memory store. Shared MIR effects for such
  construction take synchronization and unknown effects from the exact
  constructor summary, not from the intrinsic's conservative table entry.

Writing an independent local scalar keeps exact borrowed addresses and
regions, because checked references never reseat. A checked write is typed by
its physical destination, not by its canonical alias representative. The
place alias analysis treats a local binding that is never borrowed,
address-taken or escaping in the body as disjoint from every dereference, raw
access and loan referent. Reducible bodies widen only at loop headers; values
inside the loop are recomputed from the widened header state, and irreducible
bodies keep widening at every block.

Opaque effects, including process allocation, keep facts about uncaptured
locals; see [Capture Analysis](#capture-analysis).
A lifetime witness is still never a pointee or an allocation identity.
Different pointer fields never imply different allocations.

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

An exact scalar result-only `Move` retires the source's place and derived
origins into an independent SSA result; an integer source also passes its
envelope to that result. The scalar leaves are those of generated records:
integers, floating values, `bool`, `char`, null and raw pointers, and integral
enums. It changes no ownership events: source reads after movement remain
invalid, and reinitialization establishes a new envelope. A uniquely
identified, unprojected by-value scalar formal has independent parameter
storage; moving it may preserve typed non-byte owned-field facts on an implicit
receiver. A reference's projected scalar type is not that proof. Stored
references, arrays, unions, capability carriers and unresolved paths receive
no exemption. Moving a receiver field invalidates that field and overlapping
paths, not safe siblings. Ordinary user records, nested relay methods and
overflow after parameter/field reinitialization exercise this transfer without
container names or changes to call-input admission.

Exact scalar final-address construction has a narrow transfer profile. The
pointee may be any scalar leaf above. The shared predicate takes the pointee
type rather than an integer domain, and integral enums need the program's
enum table.
Shared MIR effects retain its unknown-memory write and initialization, but do
not invent a selected user constructor or synchronization. Construction still
requires the unsafe caller to provide a vacant, nonconflicting complete extent;
it cannot implicitly replace another live non-byte object. Consequently,
existing live non-byte integer facts along exact ordinary owned-field paths
may survive this operation. Byte fields, arrays/indexes, union overlap, stored
reference traversal, compiler-capability backing carriers, unresolved roots
and loaded-origin/element facts are discarded. No pointer validity, region
extent or fresh allocation is inferred. The exact scalar construction rule
above can independently publish the newly constructed object's value when the
pointee is an integer; other leaves publish no value.

That same profile admits exact generated copy/move construction of ordinary
scalar-field records into vacant final storage. The shared MIR-effects query
reuses the verifier's complete construction shape and the same recursive
generated-record transport classifier used for prepared values. A selected
source constructor, replacement, unsupported state or extra raw access cannot
claim this profile. Only represented transfer-out events may accompany the
construction; argument staging and actual cleanup retain their own effects.
It preserves the same unrelated non-byte live-field facts. The exact-address
subobject rule above can additionally publish supported constructed field values,
but does not establish a larger initialized region. Source wrappers can relay this profile;
destroy/reconstruct still invalidates the old object's facts.

The closed typed-store profile can combine vacant structural construction with
checked non-byte scalar bookkeeping writes of one width and checked floating
stores. Independent scalar parameter moves and independent local floating
writes affect only their own invocation storage. Existing non-byte
pointee facts of a different width survive that exact profile, just as live
ordinary fields do; a construction-only profile preserves all such live scalar
pointees. A checked floating store lands on a live floating object, so it
changes no live integer object and no pointer or reference carrier. In the
body it keeps every live non-byte integer fact, address identity and region,
and the prefix envelopes of disjoint subobjects; it publishes no integer
value. A callee whose profile has floating stores still writes caller memory,
so it is never treated as frame-local. Pointer stores are not in this class:
they can replace a live pointer carrier. This follows the existing nonoverlapping-live-object contract, not an
assumption that different pointers name different allocations. Same-width live
writes, unknown/native access, destructive lifetime changes and unsupported
operations prevent preservation. The successive-publication regression includes
replacement, native mutation and destroy/reconstruct failure controls.

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
This lifetime-preservation profile alone supplies neither a raw-backed
element envelope nor logical bounds. The independently described prefix and
source-predicate profiles establish those facts for their admitted shapes;
broader source-storage performance acceptance remains a separate campaign.

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
records, recursively through owned record fields. A closed payload enum
participates when every alternative's payload does, it holds no borrowed state
and needs no active cleanup: its compiler-defined transfer and drop act only on
that payload. The bounded profile excludes borrowed state, compiler backing
capabilities, overlapping or inherited layouts, actively cleaned payloads,
arrays and any non-generated selected transfer. Raw pointer fields
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

For a structurally passive drop, an unprojected, non-borrowing local binding
with an exact owning initialization is independent of ordinary receiver-owned
fields.
The optimizer can retain those non-byte field values while retiring the local
storage; it does not keep pending result correlations, pointer-region facts,
or arbitrary aliases. Initially available inputs retain the separate exact
by-value formal rule. The scoped `Counter` regression's void and record result
controls prove the later addition again (RT-12): dropping a passive result is a
structural drop, so the field fact published by its tag test survives the
scope. A result carrying a custom destructor still performs its native mutation
and reports the later arithmetic overflow, for the prelude's result class and
for a user-defined payload whose destructor makes the drop active.

### Payload-Enum Alternatives

The closed-call layer also correlates postconditions with the alternative a
payload-enum carrier holds (RT-12). A carrier is an immutable value or a root
place; its state records the alternatives still possible and, per alternative,
facts about still-current owned integer fields. Payload construction yields
one exact alternative, a whole-object store or load carries the carrier to its
destination, and a payload-enum switch narrows its subject on each case edge
(the default edge excludes every listed case). A source call or constructor
exports, per alternative of its returned or constructed carrier, the receiver
and checked-reference postconditions of the exits that can produce it. An exit
whose carrier it cannot know contributes its facts to every alternative, and
exits that place the payload at different paths export nothing.

A boolean result that a callee computes from its receiver's or a reference
argument's alternative is exported as a tag predicate: per alternative, the
value every possible exit returns, with disagreeing or unknown alternatives
left out. Nested forwarding and negation preserve it. A branch on such a value
removes the alternatives whose result differs from the taken edge, publishes
the facts every surviving alternative shares, and makes the edge unreachable
when none survives. A missing alternative is impossible, so joins take the
union of alternatives and keep a fact only where every incoming path that may
hold that alternative agrees.

Every write that the existing owned-field rules treat as overlapping retires
the overlapping conditional facts, a carrier whose payload path it overlaps,
and the predicates over that carrier; unknown, global, raw and vacant
construction barriers retire all of them, keeping only an immutable value's
alternatives. Direct writes to by-value frame storage cannot reach the
implicit receiver. A predicate refers to its carrier's alternative after the
call that produced it; storing a new object into the carrier retires it. Each
state keeps at most 64 carriers and 64 facts per alternative. Nothing here
names a library type: the prelude's result class, `std::vector`'s and
`std::detail::prefix_owner`'s allocation results and an unrelated user result
class over a user payload enum share the rule. The user control
(`optimizer_payload_result_postconditions.gti`) proves its relayed, negated
and directly switched success additions and keeps the failed-path, reassigned,
later-call, native-write, mixed unknown-exit, wrong-case and active-destructor
negatives checked, with independent replay and runtime overflow evidence.

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

CFG argument transfer snapshots every incoming fact before publishing any
destination block parameter. This includes integer intervals, loaded/field
origins, entry-relative updates and actual addresses. A backedge may swap
parameters; sequential publication would lose an old input and could produce
an unsound safety proof. Only the argument facts are copied, not the whole
abstract memory state.

Context discovery distinguishes an admitted source summary still pending from
a permanently unsupported operation. The first postpones/retries its caller;
the second is an opaque transfer with a normal continuation, allowing later
independent source contexts to be discovered after mutable facts are retired.
An allocator/raw-lifetime barrier still cannot export invented content or
effect-closure facts. A generic missed remark (`check retained: canonical MIR
facts do not prove arithmetic-overflow safety`) is not evidence of a specific
allocator cause; it may describe other absent contextual facts too.

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
indices remain checked. A proved public logical-size check does not imply
raw storage identity or initialization. The private prefix-read detector and
its unchecked backend helper belonged to the retired prefix capability; they
are not a current source-container optimization path. A discharged public
check emits no target operation and its existing failure edge becomes an
unconditional success edge. Source lifetime operations, storage/index aliasing,
opaque calls and unsupported joins retain the independently documented
invalidation rules; only admitted content/postcondition profiles republish
facts.
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
transformation. No public or trusted-source unchecked-container spelling grants
a discharge. Source-defined raw access keeps its unsafe obligations and actual
load/store even when separately proved downstream arithmetic is discharged.

### Capture Analysis

An unknown effect can reach only storage whose identity reached it. The
closed-call interpreter therefore keeps facts about a local binding across an
opaque call, synchronization, native call, unknown destructor or raw write,
provided no earlier instruction captured the binding and the instruction does
not take it as an input. It keeps the binding's own non-byte integer fields
and scalar values. It does not keep facts about storage the binding points to,
its prefix envelopes or its address identities, and a reference binding
designates other storage, so it keeps nothing. The interpreter still marks the
effect opaque. A constructed container's live length therefore survives
building a second container through the opaque process allocator, and a loop
counter survives printing.

The capture analysis extends the place alias and escape analysis in
`alias.cpp`. It has two layers.

- **Callee summaries.** Each exact function, constructor, destructor and
  field-initializer body classifies its receiver and each reference or
  borrow-carrying parameter:
  - *None*: the body does not expose the input's identity.
  - *ReturnOnly*: the identity reaches the caller only through the result,
    which the caller tracks as its own value or loan. A returned borrow of the
    receiver is the common case.
  - *Captured*: any other route. That includes a raw address, a store into any
    place, a closure or synchronization operation, and an intrinsic that
    writes memory or runs code. It also includes a dynamic, native, polymorphic
    or otherwise unknown callee.

  Summaries are a least fixpoint over a caller worklist. Each records the
  classes on paths that return normally separately from the classes on any
  path. The caller applies the first on an invoke's success edge and the
  second on its failure edge, so cleanup on a constructor's failure paths does
  not make its successful result captured. A constructor whose receiver is
  captured, or a function returning such an object by value, marks its result
  captured, and a caller adopting that object as a local captures the local.
  A body without an exact summary captures every input.
- **Local flow.** A forward may-capture dataflow over the caller's control
  flow finds, for each local binding, the instructions it may already be
  captured before. Loop back edges carry a capture from one iteration into the
  next. A loan that escapes the body captures its source from entry.

A value carries a binding's identity when it comes from an address-forming
operand, a receiver, a reference, or a borrow-carrying aggregate, including
through recorded loan carriers. A scalar or raw pointer loaded from storage
carries none, because storing an address into any place is itself a capture.
The one exception is a binding that MIR records as the carrier of a loan of
the same storage, whose every later use still names the loan.

An intrinsic whose shared MIR effects write no memory, run no code and do not
synchronize only inspects or reborrows its operands; its result may carry
them. GTI lambdas capture only value snapshots and never references, so a
closure cannot carry a local's reference into unknown code. Malformed MIR
fails closed: an instruction whose operand provenance the scan cannot follow
(an undefined value, or an empty or cyclic staging chain) captures and
involves every binding. An unknown staged call input preserves nothing.

The rule names no library type. Its tests use unrelated user records and
containers, functions declared without a body, and native calls. Removed
checks still replay through the ordinary proof verification. That replay
re-runs the same analysis, so the negative fixtures carry the soundness
evidence:

- an address passed to native code;
- an aggregate holding a borrow;
- a constructor that registers its own field;
- an exposure before the use;
- a capture on an earlier loop iteration.

Process allocation itself still reaches an opaque native call in the safety
interpreter. Facts about memory the allocator could reach, such as the first
container's element envelopes, are still erased. The declared process-
allocator runtime contract (see
[standard-library-and-runtime.md](standard-library-and-runtime.md)) now gives
those calls precise shared MIR effects. Consuming it in the interpreter, which
needs an exact argument-pointee write, is a separate goal.

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

Whole-program verification is a deterministic function of program content,
so an operation verifies a given program once. The canonical dead-instruction
rewrite verifies its input and then its result. The dead-computation pass,
the editor's claim validation and atomic mutation, and coherence replay
instead verify their unchanged program once and use the internal
`rewriteVerifiedMirDeadInstruction`, which skips only that input check: a
failed rewrite leaves its input unchanged, and a successful one verifies its
result. Coherence replay does not re-verify an optimized program or replay
snapshot identical to one it has already verified. The optimizer marks its
verified input, and `LoweredProgramBuilder` marks the source and optimized
programs it has just verified, so neither is checked twice. Every
stage-boundary verification remains. A default build verifies the whole program
10 times instead of 21; on the larger standard-library examples, GTI's own
compile time drops by about 17% at `-O0` and `-O1`.

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

The FIFO stores a solver-private work-item wrapper rather than a bare block
ID. This keeps its C++ container instantiation distinct from prebuilt LLVM
libraries: otherwise the linker may coalesce annotated libc++ queue operations
with uninstrumented operations for the same integer element type. The wrapper
changes neither queue order nor update/widening cadence, and sanitizer container
checks remain enabled. Capture-flow, capture-summary and persistent-storage
effect queues use their own private wrappers for the same linkage boundary,
without sharing phase-specific scheduling policy. A repeated-scheduling
regression crosses queue storage boundaries while checking the exact fixed
point and update counts.

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
conservative. The analysis does not assert interprocedural pointer identity.
One body-local refinement proves a binding never borrowed, addressed or
escaping in that body disjoint from dereferences/raw accesses/loan referents;
it does not prove arbitrary raw pointees disjoint from each other.
Safety-state invalidation is its first
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
interior MIR pointer survives invalidation. Bounded local and closed-world
exact-parameter devirtualization are implemented as described above. Memory
SSA, inlining, broader devirtualization, and loop/memory transforms remain
unimplemented extensions. Their general consumers, cost model, invalidation
and replay obligations justify selection; ADR 012 does not require a measured
speedup before beginning that infrastructure.

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

### Process-allocation facts

The native adapter preserves the existing sealed process-memory contract on
each verified LLVM call edge. POSIX and Windows allocation results carry
`noalias`: a successful result denotes fresh storage; null remains a possible
failure result. The POSIX status pointer is noncapturing (`captures(none)` on
LLVM 21+, `nocapture` on earlier supported versions). These facts come from
the exact target-selected runtime operation, not a public allocator spelling,
pointer type, or an arbitrary foreign function's symbol.

The shared physical declaration remains unannotated, so an ordinary foreign
call does not inherit those guarantees. Custom resource providers retain their
existing alias contracts. No extent, nonnull, no-unwind, memory-effect, or
speculation promise is added: in particular, the raw provider's fixed-width
byte count is narrowed to native `size_t`, so it is not an unconditional LLVM
`allocsize` contract. Address-alignment queries do not receive a noncapture
promise merely because MIR says that their arguments do not escape; their
results expose address-bit information.

The owning `llvm_memory_contracts` test checks actual alias-analysis results,
including prior storage, separate allocations, derived aliases and unclassified
calls, while the existing target-family matrix rejects cross-family adapters.
Native execution remains limited by the backend's macOS arm64/Linux x86_64
admission; the Windows provider mapping is not Windows execution evidence.
Fresh allocation does not imply that pointers copied from it are disjoint from
one another. Nor does this annotation alone export fresh output fields through
an outlined source helper;
inlining and interprocedural representation remain separate optimization work.
See LLVM's [return-value alias contract](https://llvm.org/docs/LangRef.html#parameter-attributes).

### Failure-edge likelihood

The native source-body adapter supplies a successful-execution likelihood for
boolean ABI failure-status edges of direct calls, function-pointer calls,
memory operations and cleanup. `Failures::branchOnSuccess` centralizes this
predicate convention. Both continuations remain present, with relative weights
of 2000 for success and 1 for failure. These are backend heuristics, not measured
profile counts, semantic assumptions, or permissions to remove a check. They
let LLVM's block layout and cost models retain the expected preference when a
failure merely propagates and has no directly visible cold originator call.
The boundary is the ABI transport operation; a callee can still inline later.

Direct checked-operation detectors receive no explicit branch weights. Their
failure successor already calls a cold originator, allowing LLVM to infer the
preference from the visible control flow. Explicit weights there can change hot
loop placement without reducing work; a controlled reverse-sort comparison
exposed that sensitivity. Keeping the hint on status transport avoids overriding
this existing inference path without introducing a workload-specific rule.

The helper creates only these specific failure-status branches. Ordinary MIR
branches, recoverable result payloads, ownership-liveness branches and decisions
already inside the exceptional cleanup machinery do not acquire this preference.
Existing profile metadata is not rewritten. Body lowering still owns the exact
failure record, source identity, continuation and cleanup schedule; the
representation hint changes none of those contracts. LLVM's existing cold
originator and terminal helpers retain their separate attributes.

Before optimization, `llvm_optimization_pipeline` checks the status predicate
polarity, a nonzero failure weight, cleanup-status edges, and the absence of
explicit hints on direct detectors, ordinary control and liveness branches.
LLVM may infer or transform metadata in its subsequent optimization pipeline.
Native callable and cleanup regressions exercise failing inputs as well as
successful execution. Performance remains a measured acceptance obligation:
this preference is not a claim about the frequency or speed of every
application's failure path.
LLVM lowering preserves two kinds of successful-execution facts for its normal
optimization pipeline: explicit verified safety discharges become assumptions
on their exact detector, and checked scalar arithmetic yields a no-wrap result
only after the overflow guard or its verified discharge. These are representation
choices over sealed MIR, not additional source-level proofs or new MIR passes.
After LLVM optimization, matching dominating overflow-result lanes are reused
to keep checked arithmetic from becoming duplicate machine operations. Recovery
also handles complete edge-wise PHI matches and exactly equivalent full-domain
constant-range detectors, retaining every failure predicate and CFG edge.
Reconstruction stays in the same block or on an immediate guarded-success edge;
cheap single-comparison guards remain unchanged unless needed for complete PHI
reuse. Compound biased predicates remain eligible. These conservative frequency
and profitability boundaries preserve conditional-counter scheduling.
The backend contract and wrapping/poison boundary are documented under
[LLVM scalar execution](backend.md#llvm-scalar-execution). Broader benefits must
be measured per workload; neither mechanism establishes general bounds-check
elimination, alias freedom, or vectorization by itself.

### Guarded checked reductions

At LLVM O2/O3, a bounded representation-level pass runs after inlining and
before vectorization. It recognizes innermost traversals whose integer index
starts at a constant and advances by one. The latch continues on inequality
or a strict signed/unsigned ordering comparison: each stops when the index
equals the target, and an ordering comparison may stop earlier. The independent
iteration bound comes from modular unit-step traversal, not an overflow exit,
`nsw`/`nuw` flags, or a circular scalar-evolution proof. Comparing the current
index permits at most `bound - initial` backedges; comparing the next index
permits `bound - initial - 1`. Both subtractions use the index's modular width
before widening. A next-index target equal to the initial value can require a
complete traversal of that domain.

A separate reduction starts at a nonnegative literal or an invariant input and carries only
nonnegative bounded additions, PHIs and selects back to its header. Uniform
increment envelopes come from constants, constant masks, unsigned remainders
by nonzero constants, constant logical shifts, zero extensions, truncations, nonwrapping
bounded additions, and complete PHI/select alternatives. The bounded analysis
uses the operations' bit-level meaning, never nowrap flags or a check that it
will remove; cyclic or unknown bounds remain unsupported. Its
maximum addition per iteration bounds every intermediate in that update DAG.
A separate structural target envelope can prove that the unit-step index
traverses a nonwrapping segment. The index and its next value can then serve as
bounded increments, supporting conditional index-weighted sums. A next-index
latch requires every target to be strictly above the initial index; a
current-index latch permits equality but includes that final index. Dynamic
target envelopes add explicit lower/upper checks on the frozen target, so a
freeze of a poison expression cannot escape the assumed range. Truncation
retains a narrower source envelope only when it fits the destination; otherwise
it yields the complete unsigned destination range.
The initial profile optimizes one reduction per transformed loop.
The pass uses the conservative nonnegative signed range even for unsigned
values. If the traversal's whole domain does not fit, one preheader guard
selects a fast loop or an unchanged checked fallback. Exit comparisons and
signed/unsigned addition overflow lanes proved constant for the admitted range
are replaced. An addition's successful value can be separate from its detector,
as happens when LLVM sinks arithmetic across a PHI join; the proof matches the
detector's exact operands. Overflow aggregates and extracts outside the fast
loop remain intact, including observations merged with the fallback. Other checks, calls,
memory effects and cleanup edges retain their original order. This applies to
ordinary counters and conditional weighted reductions without recognizing
library declarations or type names. LLVM can then vectorize eligible loops;
vectorization and a speedup are not guaranteed for every admitted shape.

For an invariant initial value, the guard first requires its unsigned value to
be at most the signed maximum. With maximum per-iteration increment `K > 0`,
it then requires `backedges < (signedMax - initial) / K`. This admits the first
update without overflowing a `backedges + 1` calculation. Subtraction remains
unflagged and division has a nonzero constant divisor, including on rejected
paths. The fast header range is conservatively `[0, signedMax - K]`; exact
intermediate offsets still need their own proof. This supports sums carried
from an outer loop without assuming the outer computation is bounded.

The guard and fast latch share one frozen bound so `undef` cannot select a
small bound at entry and a different target each iteration. A dynamic reduction
initial value has a separate freeze shared by its capacity guard and fast
header; the fallback retains the original input. The guard proves the first
update safe too, preserving paths that exit before a poison input would
originally be consumed. Cloning repairs every external PHI,
and each edit invalidates and reacquires function analyses. The pass rejects
nonduplicable/convergent operations, local allocas, EH/address-taken blocks,
already-versioned/vectorized loops, and size-optimized functions. It admits at
most 128 instructions/16 blocks per loop, 1,024 cloned body instructions per
function, and 32 transformations. Increment and recurrence tracing each have
a 128-node work bound. Decreasing or dynamic induction steps, unbounded or
negative increments, reset recurrences, outer loops containing subloops, and
non-strict relational latches remain outside
this profile.

`llvm_checked_reductions` exhaustively compares small-width checked and
transformed executions, including fallback, wraparound, effects and poison
controls. `llvm_checked_reduction_runtime` checks native results, exact failure
locations and destructor order at LLVM O0/O2/O3 against the existing C++ oracle.
These are correctness gates; comparative performance workloads remain manual.

### Guarded checked scans

LLVM O2/O3 also specializes bounded unit-step scans after inlining. The proof
uses integer recurrence structure, independent of overflow-check exits,
`nsw`/`nuw`, scalar-evolution exit counts, source names, or container identities.
The initial profiles are:

- Increasing: `next = index + 1` and a latch continuing while `next != stop`,
  or a header continuing while `index != stop`. The header success must dominate
  both every removed check and the latch. A guard `start < stop && stop < length`
  proves current/next indices fit every selected unsigned `index < length` check.
- Decreasing: `next = index - 1` and an unsigned `next > stop` latch.
  `start > 0 && start < length` proves current indices lie in `[1, start]` and
  next indices in `[0, start - 1]`: the latch cannot continue from zero.

Each specialization selects an unchanged checked fallback when its guard fails.
Empty-loop behavior, other early exits, arithmetic checks, memory effects and
cleanup retain their order. The same frozen start/stop/length values feed the
selector and every corresponding fast-loop operand, so poison is not newly
observed at entry and undef cannot vary the proof inputs across iterations.
Loop-varying lengths, dynamic strides, signed/reversed latch families outside
these profiles and noncanonical loop exits remain conservative.

The reduction and scan passes share bounded cloning, version markers and exit
PHI repair. Admission requires LoopSimplify and LCSSA; EH, address-taken blocks,
nonduplicable operations, loops containing subloops, allocas, tokens and
size-optimized functions are excluded. Each pass admits at most 128 instructions
and 16 blocks per loop, 1,024 cloned instructions per function and 32
transformations; a versioned
loop is not cloned again by either pass. Analyses are invalidated after each edit.

After a scan transformation, normal LLVM GVN load partial-redundancy elimination
runs with backedge splitting permitted. This can expose a value carried through
adjacent-element swaps instead of reloading it every iteration. LLVM's ordinary
alias and memory analyses own that proof; GTI adds no alias promises and moves
no memory operation in its scan proof. Unchanged functions do not receive this
extra cleanup. Other loop-store transformations remain future work.

`llvm_checked_ranges` compares small-width executions exhaustively, including
shared exits, effects, first failing checks, wraparound, early/empty exits and
poison/undef controls. `llvm_checked_range_runtime` compares user-defined element
scans at LLVM O0/O2/O3 and C++ O3, including exact diagnostics and destruction
order. Comparative benchmarks remain manual; specialization does not promise
that a particular workload becomes faster or matches another language.

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
