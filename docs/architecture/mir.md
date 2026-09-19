# MIR

Status: implemented structural CFG, scalar block parameters, verified
per-instance effects, ownership, synchronization, lifecycle, defined-failure
control flow, and sole production executable-body authority.

MIR lowers each concrete HIR body into body-local control flow, values, places,
resolved calls, ownership operations, and cleanup. Verified optimized MIR is
the executable component of the backend-neutral `LoweredProgram`; `MirProgram`
alone is not the complete backend API.

## Representation

Plain MIR records and deterministic model queries live in `include/gti/mir.h`.
Operational contracts are separate: `mir_lowering.h`, `mir_verifier.h`,
`mir_effects.h`, and `mir_editor.h` own lowering, verification, program-effect
derivation, and transactional edits. The implementation mirrors those
contracts: body-local verification remains beside the model primitives in
`mir.cpp`, while cross-body/program verification, defined-failure derivation,
optimization-proof replay, and shared private verification queries are
compiled independently. Deterministic printing remains in `mir_printer.cpp`.

Lowering has three lifetimes. `mir_body_lowering.cpp` owns body-local IDs,
scopes, loans, drops, CFG, and ordered source lowering;
`mir_lowering.cpp` owns program inventory and the bounded effect fixed point;
and `mir_hosted_startup_builder.cpp` owns the generated entry schedule. No
backend reconstructs hosted-startup policy.

Core MIR also owns `MirBodyAddress`, exhaustive deterministic body inventory,
and const/mutable body lookup. Inventory visits the module first, then each
class's field and static-field initializer bodies, functions, constructors,
destructors, and lambdas in concrete-instance order. Owner zero is valid only
for the module; a zero, out-of-range, or owner absent from the addressed kind
resolves to no body. Optimizers and coherence checks use this shared navigation
authority rather than maintaining their own body-family switches.

`LoweredProgramBuilder` consumes that exhaustive inventory exactly once after
optimization. It verifies source-to-optimized coherence and publishes each
address, place domain, definition provenance, body role, source identity, and
generated-item requirement beside the optimized MIR. The resulting value has
no AST or HIR pointer. An implicit initializer is data-only only for the exact
empty one-block `Exit` shape, except `Module/0`: its verified implicit-zero or
constant storage stages remain `DataOnly`, while any merged `Initializer` step
makes the module row executable.

The private C++ whole-program planner subsequently consumes the sealed lowered
inventory. Its representation snapshot owns an exact canonical `MirProgram`
copy, so planning checks structural equality rather than treating the
deliberately normalized MIR printer as an identity hash. Coordinated omission
or staleness is incoherent even when the remaining copied graph is internally
consistent. This is a fail-closed representation plan, not a new semantic
authority.

The generic body emitter is the production text step established by
ADR 016. `buildCppMirBodyEmissionMapRows` copies deterministic
representation rows — type, field-symbol, body-name/call-target, and
enum spellings plus payload-variant lifecycle facts drawn only from the extracted
`cpp_representation` authorities — and `CppMirBodyEmitter::emitBodyText`
emits body text from MIR and those rows alone, with no Program, semantic, or
HIR input. The former scalar-leaf, scalar-direct-call, class-default-cleanup,
owned-lifecycle, and hosted-failure body families are dissolved into the
general route. Admission is analysis-driven: fail-closed readiness plus the
`supportsBodyText` vocabulary probe decide every body before output. An
unsupported body rejects the whole compilation; it cannot select an AST/HIR
writer. Receiver-place handling and operation order are derived from MIR, and
the body-emitter suite requires every reviewed corpus identity to be text-
ready.

The lowered-program builder derives one `ProgramInitialization` item exactly
when the verified merged module plan contains executable initializer work;
exact `Module/0` is its body root. Each exact entry body roots one hosted-entry
item, which depends on program initialization when required. Declarations that
do not live in MIR body inventory, including enums, aliases, ABI/opaque/union
types, and unused generic templates, are copied into the active lowered
declaration tree before the builder seals the whole contract. Public backend
callers cannot supply individual rows or claim support.

Before publication, construction requires semantic and HIR analysis seals to
agree with the supplied `Program` and target, validates ordered source-unit and
active-declaration provenance, verifies program initialization and hosted
entry, exact-compares HIR/MIR storage and initializer plans, and verifies the
optimized MIR. This coherence evidence is not retained as backend input; a
successfully constructed `LoweredProgram` is the backend frontier.

`MirProgram` copies the selected execution profile from `HirProgram` as
immutable program metadata. Body lowering and optimization do not rediscover
it from host/backend flags. The current profile fact constrains frontend
global/static validity. The MIR declaration inventory includes unused,
non-generic standard-library method bodies. When one of those compiler-trusted
bodies contains an exact private thread or atomic intrinsic, lowering marks
that body as a dormant synchronization wrapper. MIR verification exact-checks
the marker, rejects it on module/hosted bodies or unrelated synchronization,
and otherwise rejects represented synchronization in the single-threaded
profile. Thus unused `<std/thread>` and `<std/atomic>` imports remain valid
without granting a backend or transform executable concurrent authority.

The deterministic serialization is currently `mir-v54`/`mir-body-v47`.
Program v54/body v47 add the nominal function ID to function-item semantic
types and serialize the new zero-state `FunctionItem` operation with its exact
concrete namespace target. Program v53 adds the closed payload-enum lifecycle
table: exact enum identity, folded traits, enum-wide active cleanup, dense
variants, ordered field types, and per-field active-cleanup facts.
`PayloadConstruct` exact-checks that table, transfers every active class
operand once, and initializes one enum value
obligation; `PayloadExtract` and switches observe an active enum place without
copying the enum. Program v52 serializes ordered type and `uint64_t` value arguments on every
concrete function row. HIR-to-MIR lowering and the detached lowered verifier
exact-compare both vectors, so a nondeducible value-only function or method
cannot collapse into another instance or delegate identity recovery to native
parameter types. Program v51 extends the version-28 benign-event rule to the
trivial-commit
assignment: a `Reinitialize` that leaves an always-available, trivially
destroyed destination available on a reachable edge is equally
state-preserving, because the checked write helper leaves the destination
untouched when it fails. A dynamic fixed-array element write therefore keeps
the `Invoke` edge that publishes its `index_out_of_bounds` status; before
v51 the eligibility predicate rejected the write's `Reinitialize` event, the
edge was never routed, and the emitted checked write discarded its failure
status — an out-of-bounds write silently no-opped while the matching read
raised. Program v50 adds the bounded two-phase mutable loan for plain
source-defined subscript and dereference stores described below. Program v49
and body v45 add the explicit early-exit full-expression
marker role used by expected propagation. Each propagation failure edge retains a
path-exclusive boundary alongside the one ordinary continuation boundary, so
verification can prove exact temporary and lexical cleanup without reopening
HIR. Program v48 and body v44 serialize the exact designated stored-reference
field on a by-value carrier's `Stored` entry loan. Verification binds that
field to the unprojected formal-parameter source and uses it to prove the exact
runtime referent or returned carrier transfer without backend reconstruction.
Program v47 and body v43 exposed the lowering-owned dormant trusted-wrapper
marker on every body header. Program v46 added the ordered managed-thread
task-adapter inventory. Each row
binds one exact task type and once-invocation contract to its HIR spawn value,
MIR body/instruction, and mandatory worker-failure/native-exception firewall.
Verification requires every concrete private thread-spawn intrinsic to retain
its adapter reference, so deleting both that reference and its inventory row
cannot create a self-consistent forged program. Synchronization records remain
backend-independent operation identities rather than private-intrinsic tags.
One shared constexpr validity table admits relaxed/acquire/seq_cst loads,
relaxed/release/seq_cst stores, all five read-modify-write orders, and exactly
nine strong-CAS success/failure pairs. Semantic resolution selects the record
from exact private declaration identity, HIR copies it, MIR lowering retains
it, and both program verification and backend preflight consume the same
operation-specific legality contract.
Missing, duplicate, reordered, retargeted, or inexact adapter rows are rejected
before backend entry.
Program v45 adds concrete fixed-array drop schedules: every array drop retains
its ordered extents, exact leaf type, and leaf class instance when applicable.
This lets verification and backends consume reverse element cleanup without
recovering array lifecycle meaning from a source type or target spelling.
Program v44/body v42 add exact proof-carrying safety discharges to instructions
and transformation records. A discharge names one bounds, storage-state, or
arithmetic-overflow family plus the exact local defined-failure origin and
outcome; records retain all related MIR block, instruction, value, and place
identities used by independent replay. Program v42 adds complete verified
effect summaries for functions,
constructors, destructors, and lambdas. Program v43/body v41 add scalar block
parameters, edge arguments, optimizer instruction tombstones, and edge-value
use provenance. Program/body v41/v40 added the verified range-iteration
inventory; its access and loan fields carry both read-only L-RANGE-01 plans and
mutable L-RANGE-02 parent/child plans without a further schema change. Program
v40 added ordered constructor-local type/value arguments to concrete MIR
constructor identities while body v39 was unchanged. Program/body v39 added
verified call-scoped loans for fresh read-only temporary arguments; v38 added
caller-expanded default-argument provenance to ordered call inputs and
explicit-argument counts to constructor initializers.

Every optimizer rewrite carries replayable proof in the rewritten node
itself, so a backend consumes optimized MIR without recovering
transformation authority from HIR. A literal produced by an identity
fold retains its dominating source value (`IdentityFold`); a literal
produced by folding a comparison or logical operation retains the folded
operation and its exact operand values (`ComputeFold`), and the verifier
re-evaluates the fold through the single `evaluateMirComputeFold`
authority — the same function the optimizer used — over the dominating
operands' literals. A `Branch` rewritten to a `Goto` retains its
condition value (`BranchFold`); the verifier re-reads the dominating
literal condition, and the optimization-coherence replay re-selects the
taken target from the exact source branch and recomputes block
reachability, which the body verifier keeps truthful at all times. The
optimizer applies every fold under shadow agreement with the HIR constant-
analysis replacement for the same HIR value, and the
fold pass iterates queue-and-apply rounds to a bounded fixpoint so
comparison, identity, and branch folds cascade inside one verified
transform report. Branch folds are currently scoped to function bodies
without loans, drop obligations, cleanup boundaries, failure records, or
frozen program-initialization steps; widening into those schedules is
recorded work, not an implied capability.

Safety-check optimization does not erase the checked source operation from
MIR. `MirInstruction::safetyDischarges` records only the exact local failure
outcomes proved unreachable by canonical body-local analysis. Verification
requires sorted unique records, a matching failure origin and outcome, and an
operation/family combination supported by the MIR contract. A checked
instruction remains failure-capable for every outcome not listed. Canonical
source MIR must have no discharges; optimization coherence derives each one
again before the optimized program can be published.

Every destructive rewrite also emits a sorted typed `MirTransformRecord`.
The stable instruction address is `{MirBodyAddress, MirInstructionId}`, not a
block-vector index. Records retain the pass, affected identities, source
provenance, proof kind, required analysis facts, and, for a safety discharge,
the exact outcome and related places. They remain untrusted:
`verifyMirOptimizationCoherence` independently replays every record from
canonical source MIR and exact-compares the result before
`LoweredProgramBuilder` can seal it. Instruction removal preserves its
identity as a canonical inert tombstone, while unreachable-block removal
retains the stable identities named by its record.

Block parameters provide the first explicit normal-edge value merging
vocabulary. Each parameter owns a new typed MIR value with source provenance;
`Goto`, `Branch`, `Invoke`, and every `Switch` edge carry ordered
arguments. Verification enforces scalar non-owning parameter types, exact
arity/type, predecessor-edge coverage, definition/use indexing, and dominance.
Class, owner, reference, array, closure, and storage parameters are rejected.
The current scalar-promotion client is deliberately acyclic and excludes
failure, ownership, loan, raw-memory, synchronization, cleanup, and lifecycle
state.

Each concrete callable instance also stores one `MirEffectSummary`. The
summary spans memory/place access, allocation, runtime and user-code calls,
traps, moves, drops, loans, synchronization, and receiver/argument escape.
`deriveMirProgramEffectSummaries` uses a deterministic call-graph fixed point.
Exact acyclic static calls may consume proved target summaries; recursive
components, virtual dispatch, declarations, native boundaries, and unresolved
calls remain conservative. Verification independently derives the least
summary, rejects under-approximation, and accepts conservative
over-approximation.
Version 20 introduced function-definition provenance and the first MIR-owned
defined-failure effect summary. Each concrete function records
`DefinitionKind::Source`, `RuntimeBinding`, or `Declaration` and a
`mayRaiseDefinedFailure` bit. Version 21 extends definition provenance and that
bit to concrete constructors and destructors and makes
`MirDefinedFailureEffects` the canonical three-kind result. Source constructors
and destructors are distinguished from declarations; a runtime definition kind
is not valid for either lifecycle body. Program lifecycle state records an
explicitly defaulted default constructor or
destructor as `Generated`, so neither declaration creates a concrete MIR body
instance. Construction and structural drops then use the same verified
operations as the equivalent omitted declaration.

Version 22 adds the pointer-free merged program-initialization unit/step plan
and serializes each block's exact
`ProgramInitializationStepId` tag. Version 23 adds the pointer-free
`MirHostedStartupPlan`, the compiler-generated `HostedStartup/<entry>` body,
its dense operation inventory, and exact generated-entity provenance tags on
places, values, drops, instructions, and terminators. It also serializes the
source `main` anchor and the exact target and program-initialization calls used
by that hosted boundary. Version 24 adds that boundary's Stage-E containment
operations -- `RouteOperationFailure`, `DropFailureCleanup`,
`RouteCleanupFailure`, `EndFailureCleanup`, `ContainFailure`, and
`TerminateCleanupFailure` -- together with the generated failure-record and
cleanup-boundary provenance tags each one owns. Version 25 makes checked
failure-edge eligibility position-independent: the same verified
Invoke/record/cleanup contract that covered full-expression roots and
prepared-call-argument detectors now covers every eligible checked scalar
computation, load, and ordinary call in any nested value position of a
failure-control-flow body, and the verifier requires exactly one edge wherever
the shape is eligible. Version 26 lowers a same-domain scalar compound
assignment or increment/decrement to its ordered read/operate/commit schedule
-- `Load` of the place, the exact checked arithmetic `Compute`, then an
ordinary `Assign` commit -- instead of one closed compound instruction, so the
update uses only MIR's primitive vocabulary and its check routes through the
ordinary failure edge. Version 27 gives an ordered `Construct` the same
failure-edge contract as an ordinary call: a failure-capable construction
carries its own `Invoke`/record/cleanup edge, and a full-expression-root
construction with a cleanup-owning result initializes that result only on the
success edge, so failure cleanup can never drop an object whose construction
did not complete. Version 28 stops treating an ownership event as a blanket
disqualification: a state-preserving read (`Read`, available to available, on
a reachable edge) leaves nothing for a failure edge to unwind and no longer
blocks routing, while any event that moves or reinitializes ownership still
does. Version 29 admits a checked assignment whose destination is trivially
destroyed and which runs no lifecycle event: a failure writes nothing and
leaves no state to unwind, so its edge needs only the ordinary temporary and
scope cleanup. This covers the narrowing compound forms that keep a closed
instruction. An assignment that replaces an owning value still requires the
destination's own unwinding rule. Version 50 completes that admission for the
element write: a `Reinitialize` ownership event on a trivial-commit
assignment that leaves an available destination available on a reachable edge
is state-preserving in the same sense, so a dynamic fixed-array element write
routes the `Invoke` edge that consumes its checked helper's
`index_out_of_bounds` status. Version 35 records the exact binding place
for a failure-capable lexical result used as a direct initializer. Its
`Invoke` success successor must contain the unique matching `Initialize`,
which arms the binding only after the call succeeds; the failure successor
therefore cannot observe or drop an uninitialized destination. This is not a
general owning-result exemption: results without that exact destination proof
still require their own success-edge drop obligation. The defined-failure bit
deliberately covers only GTI
defined failure; it is not a summary of allocation, arbitrary user code,
synchronization, or the other future O-MIR-02 effect dimensions.

Version 36 makes local reference initialization self-contained. A `Loan`
operand used by a binding `Initialize` retains both the provenance loan and the
exact runtime address-source place. These facts may intentionally differ for a
borrow-carrying value: the loan protects the owner while the place names the
object being aliased. Verification requires the source place to be available,
type-correct, and paired with the destination carrier. Backends consume that
place directly and do not reconstruct source expressions from loan parents,
full-expression roots, or carrier order.

Version 37 makes a by-value stored-reference carrier return similarly
self-contained; version 48 completes its independent proof. Lowering gives the
carrier parameter one body-local entry `Stored` loan whose unprojected formal-
binding source is the opaque identity of the external dependency proven by
semantics, and the loan retains the exact designated reference-field symbol.
Returning the carrier or its exact stored referent publishes a fresh escaping
`Return` loan through one `Borrow` operand that names that entry loan. A carrier
return also names the exact SSA value loaded or moved from one of that loan's
carrier bindings. A reference return carries a binding-rooted runtime place
whose only projections are the designated field and its dereference. The entry
loan ends at that transfer. The verifier independently rejects a projected or
wrong-field entry, a shell or unrelated same-typed carrier, an ordinary call-
result place, a reused or multiply produced return loan, a wrong payload or
referent type, or a terminator that does not carry the same loan and exact
runtime value/place.

The bounded mutable-carrier extension reuses this explicit loan vocabulary
rather than adding backend-only lifetime inference. A mutable parameter or
local origin owns one active loan. Constructing a direct mutable carrier emits
a non-escaping mutable `Stored` child; moving or returning the carrier
transfers the exact runtime carrier and origin, and a relay result remains a
child of the transferred stored loan. The mutable parent is suspended until
all child reborrows and the carrier child end. Structural and flow verification
require the selected constructor/call operand to resolve through the declared
parent, require child-first retirement on every path, and reject copy,
reseating, forged ancestry, parent use while suspended, or an escaping child
shape. The C++ backend merely represents the already-verified reference field
and schedule.

Version 49 makes a summarized reference return runtime-exact. When a body
returns the borrowed result of a reference-returning call (a transient
`CallResult` chain whose tail matches the declared receiver, argument, or
global return origin), lowering publishes the fresh escaping `Return` loan
through one `Borrow` whose single `Loan` operand names the immediate
call-result loan, then retires that chain and any summarized entry loan at
the same instruction boundary. The `Return` loan renames only provenance for
the caller; the operand carries the exact runtime address the callee
published. The verifier admits exactly this forward — matching source place,
non-widened access, and an immediately following `EndBorrow` of the forwarded
loan — and continues to reject any other parentless `Borrow` from a `Loan`
operand. Backends copy the forwarded call-result pointer into the return
loan's pointer local; re-borrowing the provenance place would re-point the
published reference at the receiver's storage instead of the returned
element.

Version 50 adds a bounded two-phase mutable loan for source-defined plain
subscript and dereference stores rooted in a local, parameter, receiver, or
existing loan. The target call produces a non-escaping `CallResult` loan in the
reserved state and preserves target-first defined-failure control flow.
Read-only right-operand loans may coexist with that reservation; writes, raw-
memory effects, synchronization, and use through the reserved loan are
rejected. One explicit `ActivateBorrow` instruction requires all overlapping
loans to have ended, suspends the mutable parent, and must be followed
immediately by the one `Assign` through that loan. Failure cleanup may end the
reservation without activation. Symbol-rooted targets retain the ordinary
exclusive loan because body-local MIR cannot prove that an intervening call
does not mutate the same global. The C++ representation consumes the verified
schedule and erases activation as a no-op; it does not reconstruct evaluation
order or loan state from source syntax.

Because the entry loan is self-sourced at the carrier binding — MIR cannot
name the caller's owner storage — its canonical protected place collides with
the carrier object itself. Verification therefore exempts whole-carrier
accesses from that collision in both access modes: reading the carrier as a
receiver (`first != last` on a by-value mut cursor parameter), mutating it,
and moving it are all independent of the protected referent, exactly as they
are for a local carrier whose loan protects the distinct owner binding. A
projection that still carries borrowed state receives no exemption, so a read
or write of the stored reference field outside the loan, and any reseat of
it, still conflict. Accesses through the stored reference resolve through the
loan itself and follow the ordinary reborrow rules.

The C++ representation snapshot keeps `copyConstructible` distinct from
`copyable`. The former permits initialization of a fresh lifetime slot from a
copy and is sufficient for read-only forward-iterator carriers. The latter
also requires copy assignment and is used only when an already initialized
destination may be replaced. Backends consume these verified lifecycle facts;
they do not decide whether a stored dependency may be duplicated or reseated.

`deriveMirDefinedFailureEffects` starts every function, constructor, and
destructor conservatively at `true`. Its function component retains version
20's source-defined, closed, acyclic scalar-CFG/static-call proof and also
recognizes the exact `class-default-cleanup-v1` construction and normal-cleanup
schedule. The proof additionally admits passive string-view parameters,
places, values, literals, and call operands, including `float` and `double` (a
trivially droppable passive value cannot raise), admits reference parameters
together with terminal passive field loads through their dereference carriers
(reading through a compile-proven borrow cannot raise, exactly like a
read-only receiver's passive field load), accepts a C-linkage or
runtime-binding call target by its
`FailurePropagationKind::None` language contract instead of recursing into
a body that does not exist, and accepts the six wrapping/saturating
integer-arithmetic intrinsic calls, which carry no failure channel.
Optimizer-produced literals remain classified by their retained source
operation, so folding cannot silently strengthen this effect summary. Its
destructor component can prove `false` for bounded source bodies
over the admitted scalar/class operations. A second closed proof admits exact
source constructors whose base-free passive-scalar class has one verified
initializer stage per declared field, their matching source destructors, and
free-function graphs over those scalar/class values. This is only the defined-
failure dimension by itself. Every function, constructor, or destructor whose
definition provenance is a declaration (and every runtime-bound function) is
verified as an exact bodyless skeleton: one empty reachable entry block ending
in `Return` for `void`, or `Unreachable` for non-`void`, with only its exact
formal-parameter places admitted. This prevents declaration provenance from
masking an executable MIR schedule. General body admission and failure-form
selection consume these proofs together with the body's verified operation and
cleanup schedule.
`deriveMirScalarDefinedFailureEffects` is the legacy function-only accessor for
version 20's vector. Bodyless declarations, runtime bindings,
recursive cycles, unsupported signatures or operations, and open call targets
therefore remain `true` in the applicable vector. Program verification
independently recomputes the proof and rejects an unproved `false` claim while
permitting a conservative `true` claim. Lowering itself is stricter: it first
builds and verifies a provisional all-conservative program, derives the
three-kind summary, lowers a second time with those facts, then requires the
final re-derived result and stored bits to agree before accepting the program.
An exact static GTI call to a proved-failure-free target carries
`FailurePropagationKind::None`; a static GTI call to a target that remains
failure-capable carries `DirectCall`.
Verification rejects either direction of propagation drift for the recorded
target summary. C-linkage and intrinsic calls remain `None`, while virtual
dispatch retains its distinct `VirtualCall` channel.

This semantic effect proof is intentionally independent of C++ representation.
For example, a concrete internal, constrained, or `constexpr` source body can
validly summarize false regardless of whether its native representation uses a
template specialization, internal symbol, or ordinary definition.

Version 19 made `Compute/Literal` origin explicit: directly lowered
instructions carry `source` provenance, while an optimizer-created literal
carries an `identity-fold` source value. Verification requires that value to
strictly dominate the rewrite and trace through same-typed MIR identities to
the exact literal, so a backend can consume the optimized value without
consulting the legacy HIR replacement table.
`verifyMirOptimizationCoherence(source, optimized)` is the structural authority
gate between that canonical pre-optimization program and production MIR. It
accepts exact equality plus only an in-place, provenance-bearing identity fold
at the same body/block/instruction position, with the same IDs, type, and every
other instruction field. The expected instruction is copied from source and
patched independently of `MirEditor`; the derived value-use index is
rebuilt before an exact whole-program comparison. There is no authorized CFG,
branch-target, call-target, operation, operand, lifecycle, layout, or header
rewrite today. This comparison is structural rather than based on
`MirPrinter`, whose deterministic diagnostic format intentionally does not
encode every snapshot-local identity or declaration field.

Version 18 added caller-owned `PreparedParameter` obligations for eligible
class-value inputs to ordinary calls. Class copy inputs initialize their stage,
class move inputs reparent the exact source obligation when one exists, and the
final `Call` transfers each stage exactly once when the callee begins. The same
schedule now admits a direct active-cleanup payload enum for the bounded
ordinary callable family. Enum copy inputs initialize their stage only when
the folded traits permit it; move inputs reparent a named value obligation,
while a fresh variant reparents or transfers its value-rooted obligation
directly. Parameter entry owns one lexical enum obligation, and an enum return
has one initially inactive value-rooted drop that only the paired `Invoke`
success edge activates. Version 18 also added
normal-success lifecycle events for one bounded cleanup-owning ordinary-call
result shape, leaving that result uninitialized on the failure edge. The same
schema now supports an exact local scalar argument detector or un-sited static
direct-call propagation after one or more prepared stages without adding
duplicate failure or cleanup metadata. It also represents an exact
cleanup-owning class producer handed directly to a by-value call: successful
production initializes the source drop, the adjacent `CallInput` reparents
that identity into its matching prepared drop, and the `Call` transfers it
out. For an owning `Expected` extraction, the unprojected Value-root source
place is only the drop identity for that chain; it does not imply independent
storage publication. Verification and backend admission require one consumer,
matching types and drop descriptors, exact success/failure edges, and no
additional place or loan use. Version 17 added bounded
`Invoke`/`PropagateFailure` control flow, body-local
fixed failure-record parameters, and deterministic failure cleanup for
eligible full-expression-root scalar operations in function and lambda bodies.
Version 16 added the immutable failure artifact descriptor, exact one-based
detector-site mappings, function-instance declaration identity, and an explicit
`PackFold` operation with ordered exact element-call metadata. Version 15 added
exact local defined-failure origin
sets, snapshot-local source anchors, and call-like propagation channels. It
also extended the ordered-input
schedule to a concretely resolved class `operator()` receiver, using a
`MoveValue` checkpoint for an explicitly moved receiver or exact
trailing-`&&` target. Version 14 added
exact synchronization operation identity and atomic order metadata to call
instructions. It retained version 13's exact ordered-input
schedule for concrete ordinary constructor invocations with supported
arguments. A scheduled constructor has
one exact constructor target, no receiver, source-ordered argument checkpoints,
and a final `Construct` that consumes only those one-use prepared values.
Version 12 added exact global/static borrow-origin places to function summaries
and call instructions. Version 11 added exact class-copy and class-move
parameter checkpoints to the
ordered ordinary-call stage. A copy checkpoint retains the source place; a
move checkpoint retains the already materialized value and transfers any exact
active temporary obligation at that checkpoint rather than at the final call.
It retains version 10's explicit call-input roles and materialization order,
including exact owned-callable
return/field transport, complete declared-field metadata, and constructor
parameter-to-field move evidence introduced alongside that stage. It retains
version 8's ordered closure capture operands, copy/move modes, exact environment
symbols and capture-place projections and version 7's consuming
callable capabilities and full concrete semantic type on lambda-instance
records, so deterministic output exposes enclosing generic identity as well as
physical closure shape. It also retains the version-6 exact
read/mutable invocation requirements, selected and call-site capabilities,
and concrete function receiver mutability; the version-5 explicit callable
boundaries; and the version-4 body-local full-expression identities, exact
ordered cleanup membership, standalone boundary markers, and active-cleanup
metadata.

A `MirBody` owns:

- basic blocks with `goto`, branch, switch, return, unreachable, or exit
  terminators;
- one concrete `PlaceDomain` copied from HIR;
- typed values with one defining instruction and indexed uses;
- places rooted in bindings, symbols, `this`, temporaries, values, or loans,
  with field/index/dereference projections and, where semantics supplied one,
  the corresponding value-owned `PlaceKey`;
- a field projection from a class-valued SSA temporary is rooted in that
  producer value, never in the projected load result. The producer dominates
  the place-root use and the following `Load`, so verification cannot admit a
  self-use-before-definition cycle;
- explicit initialize, assign, modify, move, borrow, call, construct, drop, and
  end-borrow instructions, plus carried read/move/reinitialize ownership events;
- for the bounded ordinary-invocation slices, one non-removable,
  non-reorderable `CallInput` checkpoint per receiver and argument. Each
  checkpoint retains the source HIR value, call-site identity, receiver or
  exact argument-index role, selected parameter type, and value/class-copy/
  class-move/read-borrow/mutable-borrow/read-temporary-borrow mode. Argument
  checkpoints additionally
  retain whether the value came from an omitted source default; verification
  requires those markers to form one trailing parameter suffix. For an
  ordinary `Call`, a
  class-copy checkpoint initializes one exact caller-owned prepared-parameter
  place and a class-move checkpoint reparents the exact materialized source
  obligation into such a place, or initializes it when the type has trivial
  cleanup. The prepared owner remains active until the final `Call` transfers
  it exactly once. Scheduled constructors retain the earlier bounded checkpoint
  contract and do not yet use prepared-parameter places. Every checkpoint's
  single result has exactly one executable use by the matching `Call` or
  `Construct`, so the invocation consumes only prepared inputs rather than
  reevaluating a source operand;
- typed lexical/value drop obligations and per-instruction initialize, move,
  reparent, replace, transfer-out, and drop lifecycle events;
- resolved call targets, static/virtual dispatch, constructor targets,
  intrinsic identity, C linkage, and external symbols;
- an explicit `MirOperation::PackFold` for the bounded call fold. It retains
  the source HIR fold identity, fixed named-place operands, selected generic
  declaration, source pack identity and argument position, and one concrete
  element type plus exact resolved call target per pack element in source
  order. An empty pack retains an empty element sequence instead of
  disappearing during lowering;
- exact defined-failure identity on checked instructions. Each local detector
  origin retains a sorted unique outcome set and a snapshot-local
  `SourceUnitId` plus line/offset anchor and has one parallel artifact-local
  `FailureSiteId`. `MirProgram` owns the immutable canonical descriptor, source
  mapping, origin assignments, and SHA-256 artifact identity. Direct, virtual,
  constructor, callable, and managed task-join propagation are separate
  channels and never copy a callee's possible category set. The verifier
  rejects invalid vocabulary, anchors, duplicates, instruction placement, or
  propagation that disagrees with the exact target and dispatch. For scalar
  `Compute`, it also independently derives the checked fixed-width-integer
  contract for `Add`, `Subtract`, `Multiply`, `Divide`, `Remainder`,
  `ShiftLeft`, `ShiftRight`, `Negate`, and integer-to-integer `Convert` from the
  operation and operand/result domains. The represented local origin must have
  exactly that canonical outcome set and one matching artifact assignment/site;
  an eligible detector must name exactly one producer record and preserve it
  through its `Invoke`, failure parameter, and `PropagateFailure` endpoint;
- one bounded failure-control-flow family for every eligible failure-capable
  scalar `Compute`, `Load`, or `Call` in a full-expression root or nested value
  position, including an exact scalar selected as an indexed ordinary-call
  argument after at least one earlier owning parameter has been prepared. The
  nested operation is either a local `Compute`/`Load` detector or a static
  direct `Call` propagation with no local site, nested owning-parameter
  transfer, or active-drop result. A passive `Expected<scalar, E>` call result
  is also eligible before a contextual condition conversion: its transformed
  value out-parameter publishes only on success and carries no MIR drop to arm.
  Every eligible operation has no destination, loan, state-changing ownership
  event, reference result, or active-drop result and occurs in a
  failure-control-flow body. The operation is the final instruction in an
  `Invoke` block. Its normal
  successor has no failure state; its dedicated failure successor receives one
  body-local `MirFailureRecord` parameter, ends active loans, destroys active
  full-expression temporaries, prepared caller-owned parameters, and lexical
  owners in reverse construction order, and terminates with `PropagateFailure`
  carrying the same record ID.
  The record points back to the exact detector/propagating instruction, so a
  local origin retains its artifact site and a call propagation edge cannot
  re-site the callee record;
- exact backend-independent synchronization records on resolved calls. The
  verifier accepts thread spawn/join and mutex lock/unlock without an atomic
  order; requires a legal operation-specific order for atomic load, store, and
  read-modify-write; and requires a legal success/failure order pair for
  compare-exchange. Release loads, acquire stores, release/acquire-release
  failure orders, and a failure order stronger than success are rejected.
  Synchronization metadata on a non-call instruction is invalid;
- one `MirOperation::FunctionItem` for each contextual GTI function item. The
  result type carries the nominal `FunctionId` and exact signature, and
  `functionTarget` carries the concrete MIR function instance. It has no
  operands, place, ownership event, capture environment, or failure behavior.
  Verification requires the target to exist, have GTI linkage and a body, be
  namespace-owned rather than class-owned, and exactly match the nominal ID,
  result, and parameter rows. A missing target or forged identity/signature/
  owner is invalid before optimization or backend entry;
- one ordered `MirThreadTaskAdapter` for every HIR thread-task adapter. The row
  retains the task type, HIR spawn value, exact MIR spawning body/instruction,
  exactly one concrete function or lambda entry, its selected receiver
  capability, outer `Once` invocation, worker-failure catch, and native-
  exception firewall. Program verification requires a one-to-one HIR/MIR
  inventory, an entry whose type/signature/body match that row, and an exact
  `ThreadSpawn` instruction/synchronization pair; duplicate, reordered,
  retargeted, unrooted, capability-forged, or non-firewalled rows are invalid.
  A function-item task additionally requires its spawning operand to be the
  exact `FunctionItem` target carried by the adapter;
- exact confined invocation and confined/owned argument-boundary records. The
  verifier permits this metadata only on calls; requires descriptors to be
  ordered, unique, within the call's operand list, and identical to the
  concrete target contract; and requires a confined invocation boundary if
  and only if the receiver traces to the matching enclosing callable-parameter
  binding. An argument carrying owned transport must be the exact moved
  parameter operand. An exact-return transport may coexist with a confined
  boundary only when the parameter has repeatable signatures and no forwarding
  edges. The callee must either return that exact moved binding as a single-use
  result transferred out of local cleanup, or return the exact owner
  construction whose declared field and constructor initializer prove the
  corresponding parameter-to-field move. Direct result/field materialization
  is distinguished from a local temporary that requires an explicit
  transfer-out event. Class records retain all declared fields so this proof
  is not inferred from the lexical-drop subset;
- read-, mut-, or once-callable invocation on each concrete callable call,
  plus the required and selected capability for each exact generic signature.
  Mutable invocation requires an exclusive or owned receiver. Once invocation
  requires the receiver value to trace to an exact MIR `Move` carrying the
  matching available-to-moved ownership event. Every SSA value in that move
  and optional identity chain has exactly one use, ending at the verified call
  receiver or forwarding operand, so one move proof cannot authorize two
  consuming calls. Program verification checks the required capability against
  formal parameter access, binds the selected capability and full
  parameter/result signature to an exact lambda or `operator()` target, and
  validates each forwarding target, parameter index, concrete call edge,
  operand provenance, and consuming move back to the recorded source
  callable-parameter binding. Source move-state CFG analysis remains
  authoritative for path cardinality;
- the program-entry kind and exact concrete startup-append target for the owned
  command-line argument form;
- the compiler-generated `MirHostedStartupPlan` and `HostedStartup/<entry>`
  body. Their exact `main` anchor and, for the owned-arguments entry form, two
  adapter-local origins lower from HIR rather than being synthesized by a
  backend. The dense operation inventory orders program initialization,
  source-entry parameter transfer, and the exact vector/string constructor and
  append calls while preserving those callees' allocation records; no local
  `allocation_failure/hosted_arguments` origin is produced. MIR now owns this
  hosted schedule and provenance, including the Stage-E terminal containment
  and cleanup-failure control flow: every failure-capable startup stage routes
  one contained failure record through `RouteOperationFailure`, reverse
  `DropFailureCleanup`, `EndFailureCleanup`, and `ContainFailure`, and a
  failure-capable destructor adds `RouteCleanupFailure` plus
  `TerminateCleanupFailure`. Partial-construction rollback and generic backend
  emission of that cleanup remain open; `CppMirBodyEmitter` still reports the
  hosted schedule as `MissingFailureCleanupMir` and
  `MissingPartialConstructionRollbackMir` debts;
- lexical scopes, cleanup edges, loans, carrier bindings, and source/HIR
  provenance.

IDs are body-local and start at one.

## Lowering And Verification

For each body, MIR lowering creates an entry block and root scope, seeds
parameter cleanup, lowers prologue/construction values, lowers statements,
synthesizes a legal terminal edge, rebuilds reachability and value uses, then
verifies structure.

Default expressions arrive as ordinary caller-side HIR values. MIR schedules
their `CallInput` checkpoints after written arguments, preserves the default
provenance bit, and verifies the trailing-suffix invariant against the complete
selected parameter list. Constructor initializers preserve their explicit
argument count alongside all expanded inputs. The production C++ emitter
therefore emits only verified MIR values and never emits a C++ default
parameter expression.

A valid HIR layout query lowers to an ordinary unsigned-64
`MirOperation::Literal` containing the retained frontend value. MIR has no
target-layout operation and therefore cannot reinterpret `sizeof` or
`alignof`; the source HIR identity remains available as provenance.

Each HIR program-constant substitution lowers into an exact per-body
`MirProgramConstantSubstitution` inventory row and a marked constant-producing
instruction, never a `Load` from not-yet-initialized program storage. Supported
positive scalar and enum values use exact literal/enum operands. A negative
integer uses one private magnitude literal consumed exactly once by the marked
`Negate` result. Verification ties the row, HIR value, constant, instruction,
type, uses, and source provenance together and rejects any competing unmarked
compute or storage load carrying that HIR identity. This preserves the semantic
later-storage proof inside the merged executable program-initialization body;
the initializer consumes the materialized constant rather than loading
not-yet-initialized program storage.

`MirProgramInitializationPlan` is the pointer-free authority for the merged
`Module/0` schedule. Ordered unit rows retain even empty source units. Dense
step rows copy the exact storage kind, role, symbol, concrete static owner,
binding, storage place, entry block, storage `Initialize`, and either
`ImplicitZero`/`Constant` data provenance or the dynamic HIR statement,
initializer, and MIR full-expression identity. Every Module block is tagged
with its owning step. Verification requires exact plan/body coverage,
canonical storage places and publication, step-local CFG, strict program-
storage access order, substitution materialization, and dense transitions to
the next step or final `Exit`. Non-generic class static-initializer bodies are
distinct canonical empty shells after their storage moves into Module. Checked
dynamic initialization retains ordinary verified failure records and edges;
production containment and generic body emission remain later backend work.

`MirClassInstance` carries the selected `[[c_abi]]` record size, alignment, and
ordered field offsets. Program verification checks that this metadata is
structurally complete and within the record bounds; deterministic printing
exposes it for audits. MIR does not contain a native-layout instruction.

Floating literals lower as ordinary `MirOperation::Literal` values tagged by
their semantic `float` or `double` type. The verifier requires the retained
`BinaryFloat` format to match that type, and deterministic printing uses
`f32:0x........` or `f64:0x................`. MIR contains no LLVM floating
representation.

Payload-enum construction and pattern extraction lower as distinct typed HIR
values and `MirOperation::PayloadConstruct`/`PayloadExtract` instructions with
the semantic enum, variant, field index, and ordered operands retained. An
exhaustive payload switch carries that semantic fact in HIR; MIR terminates its
otherwise-required unmatched CFG target with `Unreachable` instead of
inventing a path to the switch exit. MIR additionally carries the exact
pointer-free payload lifecycle table. Construction transfers each active class
operand obligation into the selected alternative and arms one enum obligation;
normal and failure cleanup therefore drop only the active alternative exactly
once. Whole-value move construction and the adjacent Move -> Replace -> Drop
assignment schedule transfer that obligation without copying or duplicating
active state. A switch or extraction read-borrows an owning enum place. Extracted
fields remain immutable copies, so move-only fields admit tag-only matching but
not a payload binding; this is not yet a consuming/borrowed projection model.
The bounded direct callable schedule uses those same obligations for argument
preparation, parameter entry, direct result placement, success activation, and
defined-failure cleanup. No AST or HIR expression is re-emitted to manufacture
a payload-enum call boundary.

For an admitted ordinary direct field, MIR retains the existing class
initializer and lifecycle schedule. A payload construction's value-rooted enum
obligation is reparented into the field's `ConstructionRollback` obligation at
the exact `This.field` initialization. Normal constructor completion transfers
that rollback obligation into the class field schedule; a failure preserves
the reverse-order rollback edge. Generated containing-value movement uses the
ordinary class schedule, while direct mutable-field replacement requires the
verified Move -> Replace -> Drop chain and leaves the class field schedule as
the sole eventual cleanup authority. The verifier rejects a missing,
duplicated, reordered, or retargeted publication; no backend field spelling can
create this permission.

Valid passive unions retain their checked size, ABI alignment, and ordered
field layout on semantic, HIR, and MIR class records. MIR does not infer an
active field or add a union operation: member access remains an ordinary place
operation carrying the frontend's unsafe classification.

Each defined integer operation lowers as an ordinary call with one of nine
exact intrinsic identities. Its effect-table entry is speculatable, removable
when unused, reorderable, non-trapping, and free of memory, ownership, and
user-code effects. Checked-result overflow is an ordinary error-state value,
not a MIR failure edge. These facts apply only to the explicit standard-library
operations; checked built-in arithmetic keeps its failure effect. MIR records
and verifies the selection but does not derive arithmetic mode from the public
function name.

`verifyMirProgram` is the fail-closed composition facade. Its explicit
component mask must contain body-local verification, cross-body/program
contracts, and effect summaries; an incomplete or unknown mask is rejected.
Body-local verification checks identity ranges, definitions and uses,
terminators, call/constructor and synchronization metadata, native-linkage
invariants, value availability, reachable loan state, and lifecycle state.
Program verification owns program-entry adapters, cross-body callable and
lifecycle contracts, generated inventories, initialization, and hosted
startup. The separate effect service derives and checks the closed failure
summaries. It
also requires each lambda instance's exact type to reproduce its declaration,
result, parameters, and captures, and requires callable targets to match that
full type. Same-shaped closures from distinct enclosing generic instances are
therefore not interchangeable. Function-item instructions receive the same
cross-body exactness check against their concrete namespace function row, so
matching native representation cannot make distinct `FunctionId` values
interchangeable. It rejects an adapter
identity on an ordinary or no-argument function, a malformed owned-argument
entry shape, and multiple MIR entry points. A value use in its defining block
must follow its defining instruction. A reachable cross-block use must be
dominated by the definition; this includes values referenced through place
roots and index projections. Current MIR has no block-parameter values: every
`MirValue` has one instruction definition, while constants, parameter
bindings, and entry loans use their own representations.

The program verifier additionally rejects synchronization operations when
`MirProgram` selects the default single-threaded profile, except for exact
compiler-trusted private thread, atomic, or mutex synchronization intrinsics in
a body carrying the lowering-owned dormant-wrapper marker. Such a body exists
because MIR retains the complete source-defined standard-library declaration
inventory; it has no task adapter or executable use and public use remains
rejected by semantics. A thread-spawn operation still requires the concurrent
profile and one exact task adapter. Target `threads` capability and public
wrapper availability remain semantic/source-library authority. Atomic and
mutex records are now consumed by the shipped `<std/atomic>` and `<std/mutex>`
slices; unrecognized or forged records still fail verification rather than
selecting native behavior.

The owned-entry append target is a module-root call edge for reachability and
dead-code decisions even though it is not represented by an instruction in the
user function body. A pass may rewrite that edge only while preserving the
verified owner, return, parameter, linkage, and entry-kind contract.

`computeMirDominance` exposes an immutable GTI-owned result in block IDs. Its
compiled implementation validates the CFG, copies it into a private snapshot
with stable node addresses, runs LLVM's generic dominator calculation, and
copies only reachability and immediate-dominator IDs into the result. It is a
fresh full computation: neither the result nor the verifier retains pointers
into `MirBody` or the private snapshot.

The snapshot adapter has a private `printAsOperand(llvm::raw_ostream&)` hook
because LLVM's generic dominator implementation requires it for diagnostic
printing. This does not make `raw_ostream` GTI's MIR-printing abstraction;
public headers remain LLVM-free and `MirPrinter` remains GTI-owned.

M-OWN-02 maps each carried semantic/HIR key to body-local `MirPlaceId` metadata
and preserves constant index values independently of the SSA value that
computed an ordinary index. Its verifier runs a sparse
available/moved/uninitialized state-set fixed point over reachable CFG edges.
Local binding storage begins uninitialized, parameters begin available,
initialization activates a local lifetime, and drop returns the root to
uninitialized for roots that participate in an explicit move. Because a
lexically dropped binding spells every scope end as a drop, its
re-initialization requires definitely uninitialized storage; a trivially
dropped binding leaves scope without any MIR instruction, so a moved or
conditionally moved loop-local may still carry moved state across the back
edge and its re-initialization at the declaration is accepted from any prior
state as the implicit scope end. The sparse
fixed point does not impose lifecycle state on unrelated legacy MIR places.
Moves require an exact available key, element assignment restores that key,
ancestor uses observe moved descendants, and different constant elements are
disjoint while dynamic selections remain may-alias. Branch and loop joins use
state-set union. A mismatched domain, forged move key, missing restoration, or
unavailable operand makes the MIR candidate invalid rather than producing a
late source diagnostic or relying on generated C++ behavior.

Member stores retain their semantic `Reinitialize` event. A checked-arrow
store lowers its selected access call separately from the field assignment,
preserving the assignment operation and the exact call-result loan. Reading an
owner handle does not read its pointee's moved fields; accessing those fields
or moving the whole owner still requires their availability.

Same-block lifecycle events can prove a drop obligation definitely moved.
Retiring that shell cannot invoke the source destructor, so lowering omits its
otherwise conservative destructor-failure edge. Program verification replays
that proof before accepting the non-failing drop. Cross-block or ambiguous
state stays conservative; this does not eliminate the shell's explicit drop.

The private unique-owner borrow intrinsics are local failure detectors, just
like checked storage reads. Both read-only and mutable access require one
`Invoke` edge before reference publication. The operation-specific verifier
checks the staged owner, pointee type, result loan/access, and one
`empty_owner_access` outcome with the semantic dereference/member-access
detail. Failure follows ordinary loan ending and reverse owner cleanup;
callers propagate the original record without re-siting it.

The current `MirPrinter` schema includes each body domain, carried place key,
constant or dynamic index metadata, ownership event, complete cleanup-relevant
class lifecycle shape,
exact class/lambda cleanup descriptor, typed drop obligation, call parameter
roles, full-expression and lexical cleanup-boundary tables, and lifecycle
event, so deterministic snapshots observe the same facts the verifier consumes.
It normalizes every nonzero
process-local snapshot generation to `1`; body/revision and all structural key
data remain exact.

`MirCanonicalPlace` remains for the older loan/carrier normalization and
checked-dereference verifier paths. Its index comparison now retains constant
and dynamic-selection metadata, but replacing that normalization with a
single key-producing loan transform is outside the bounded fixed-array slice.
Partial-construction rollback is represented for ordinary constructors: a
class field completed from one owning constructed temporary is an explicit
`Initialize` stage that reparents the temporary's obligation into a
`ConstructionRollback` obligation on the exact This-rooted field place; every
defined-failure edge of the constructor body drains the armed set in reverse
stage order behind one failure cleanup boundary, and normal completion
retires it by transfer to the caller. A constructor that
silently transfers any subobject into `this` without arming rollback (the
owned-parameter and other unstaged initializer forms) routes no
defined-failure edges at all — the lowering decides body-wide before any
edge is placed and the verifier holds the matching body-wide rule — so lowering
does not claim recoverable propagation for checked operations in that body.
Their terminal operation remains part of the verified MIR body rather than an
alternate source-tree authority. Owned-parameter class fields stage the same way: the
moved value's obligation reparents into the armed rollback obligation, so
those constructors keep routing failure edges, and every constructor failure
edge drains temporaries, scope bindings, and armed rollback obligations as
one globally reverse construction-ordered sequence behind a single failure
boundary. Unique-owner fields stage the same way as class
fields, and per-instance field-initializer bodies participate fully: their
owning declaration initializers arm rollback on the field's binding place
(their place domain roots fields in bindings rather than `this`), they route
defined-failure edges under the same body-wide unarmed-transfer rule, and
the emission analysis treats a fully armed body as carrying its complete
construction schedule. A source constructor receives every unoverridden,
active-cleanup field completed by that schedule as an initially active
`ConstructionRollback` on the corresponding This-rooted field place. Program
verification requires a unique matching field-initializer obligation with the
same owner, field identity, type, and cleanup descriptor; an explicit
constructor initializer suppresses the default entry obligation. The
constructor then drains those fields on failure or transfers them to the
completed object on success without a backend reconstructing source
initialization. Base subobjects, static-field initializer bodies, and the
destructor double-failure envelope remain M-FAIL-01 work.

M-LIFE-01 maps each HIR obligation to an exact MIR place and runs a separate
available/moved/uninitialized fixed point over lifecycle events. Parameters
begin active; value and lexical initialization activate obligations; move,
reparent, replacement, and transfer-out preserve exactly one owner; and normal
return/exit edges must retain no active obligation. Full-expression cleanup is
LIFO, lexical cleanup is reverse construction order, and a temporary created
only by a logical or conditional branch retains a path-conditional obligation
through the merge and is dropped at the enclosing full-expression boundary.
Its cleanup place is body-local rather than a branch-local SSA root. MIR
publishes only reached HIR boundaries, retains their source identities and
ordered obligation membership, and emits one primary standalone boundary
marker on the ordinary continuation plus an explicitly marked boundary on
each mutually exclusive early-return endpoint. Every marker follows that
path's reverse-order cleanup sequence. Verification rejects an absent or
duplicate primary marker, an unmarked extra endpoint, a misplaced or forged
marker, cleanup before a completed ordinary-path root, active state at a
boundary, a swapped cleanup sequence, double or missing drops, forged cleanup
metadata, use of a non-consuming operand as an ownership transfer, and
predecessor states that cannot justify an event's conditionality. Lifecycle
instructions and boundary markers are observable effects and cannot be erased
merely because they produce no SSA result.

Each drop obligation retains a stable HIR construction ordinal. Reached
full-expression drops are a contiguous suffix immediately before their path's
boundary marker in reverse ordinal order. Each emitted lexical cleanup sequence
has its own table entry and marker, and every binding-kind `Drop` instruction is
covered by exactly one such sequence. Resolved `Call` and `Construct`
instructions retain their exact parameter types; only a non-reference input
role can justify an ownership-consuming lifecycle event, and program
verification cross-checks those roles against the concrete target signature.
Ownership-consuming ordinary calls and construction retain that exact target
identity. A confined callable forwarding edge retains the exact source
parameter binding and target contract. When that contract requires `Once`
directly or through another forwarding edge, every matching concrete edge must
be rooted in an ordinary MIR ownership move; a copied source value cannot
satisfy the cardinality proof. Closure construction names one concrete lambda
instance and retains one ordered operand, capture mode, exact type, and
environment symbol for every capture. A copy operand must name an exact
readable place; an owned-move operand must be the single use of an ordinary MIR
`Move` with matching ownership state. A cleanup-bearing moved value may retain
one non-executable value-root place only when that place is the exact value-drop
obligation; an alias place or executable use through it invalidates the move
proof. Lambda-body symbol places identify their exact capture index, and
program verification ties that projection to the concrete instance. Closure
movement and drop use the ordinary lambda lifecycle obligation, including
recursively cleanup-owning captured values. Class
lifecycle metadata contains every lexically dropped field in
declaration order; its independently retained drop projection must be the exact
reverse order. Trivial fields remain structural HIR/layout facts rather than
lifecycle-verifier authority.

A concrete fixed-array obligation retains one aggregate schedule rather than
inventing a backend-visible drop for every element. Its extent sequence and
leaf cleanup identity must exactly match the array type. The aggregate `Drop`
still occupies one lifecycle position, while execution visits every leaf in
reverse index order and routes a failure-capable leaf destructor through the
ordinary MIR failure edge. This representation scales to nested fixed arrays
without making the backend rediscover element order or destructor identity.

## Controlled Optimization Edits

`mir_editor.h` exposes a deliberately narrow `MirEditor`.
Core MIR identifies bodies by `MirBodyAddress`, a GTI body kind plus owning
instance ID; instructions use stable `MirInstructionId`, and blocks use
`MirBlockId`. The editor never exposes mutable program vectors or retains
instruction pointers across an edit.

Current client-backed edits replace a computation with a local-fold or SCCP
literal, replace a branch terminator, promote one scalar place, attach one
safety discharge, tombstone one dead computation, or remove one unreachable
block. All queued claims are checked before mutation and applied to a copy in
deterministic identity order. Touched bodies have required value-use/CFG facts
repaired, then the complete candidate is verified before commit. The editor
owns analysis invalidation: semantic edits invalidate the body cache, while a
metadata-only safety discharge advances the body revision and preserves the
explicitly valid analyses. Invalid, stale, duplicate, or malformed batches
leave the input byte-identical.

`MirPass` and the body-scoped `MirBodyAnalysisManager` provide the current
small pass framework. The manager revisions bodies and caches CFG/reachability,
dominance, value uses, effects, scalar-promotion inputs, safety proofs,
place-alias/escape facts, natural-loop/range facts, and sparse constants.
Explicit preservation retains an analysis; unknown preservation invalidates
conservatively. There is no arbitrary insertion API, ID compaction,
incremental dominance update, memory SSA, or loop/memory transformation.

Semantic analysis chooses proven borrow endpoints; HIR carries them; MIR emits
and verifies them. Verification is an integrity gate, not an alias or last-use
analysis that invents missing semantics.

A global-origin function summary and every matching call instruction retain
one `BorrowOriginPlace`. Lowering materializes a symbol-rooted MIR place with
the summarized projections and creates the call-result loan from it. The
verifier rejects missing, forged, mismatched, receiver/argument-attached, or
wrong-return places. Unretained loans must still reach their selected
full-expression `EndBorrow`; retained mutable loans follow the semantic lexical
endpoint. The MIR profile remains metadata: mutable process-wide storage is
rejected by semantics in the concurrent profile rather than reinterpreted by
lowering.

A transient call-result loan used only as the receiver of a non-borrowing
member call ends immediately after that call. Lowering carries the source HIR
receiver identity through call preparation and emits the same selected
call-result chain retirement used for consumed arguments. Reference and
borrowed-state results are excluded because they may retain the receiver's
provenance. This makes two independent scalar reads from a mutable container
within one larger expression sequential rather than leaving the first
receiver loan active until the expression boundary.

One MIR loan may name multiple carrier bindings for a shared read-only
semantic loan. It still has exactly one producer and one path-sensitive loan
state; a shared loan uses its active and inactive states. Ending it invalidates
every carrier simultaneously, and verification rejects any subsequent carrier
use or inconsistent state at a join. Each local reference carrier's
`Initialize` separately retains its physical address-source place, so sharing
one provenance identity does not erase the runtime alias chain.

Copy construction of a read-only stored-reference carrier adds a binding to
that same loan while copying ordinary shell fields into independent storage.
Moving transfers a binding, and destroying one alias does not end owner
protection while another carrier remains live. Assignment is absent because it
would reseat an established dependency. A mutable stored-reference carrier has
an exclusive loan and is noncopyable rather than gaining this shared state;
moving it transfers the exact carrier edge, and ending the final carrier
reactivates its suspended mutable parent.

A bounded exclusive reborrow instead lowers as a distinct mutable or read-only
child loan linked to one mutable parent. Producing the child suspends the
parent; `EndBorrow` for a child reactivates it only when no other active child
remains. Nested chains apply the same transition recursively. Known-disjoint
sibling-field children may coexist, and a suspended parent may still be used
through a known-disjoint projection. Verification rejects direct or
overlapping use of a suspended parent, a read-only-to-mutable transition, or
inconsistent active/suspended state at a CFG join. The precise place model for
this slice is limited to stable roots with named-field and checked-dereference
projections; it does not claim indexed, raw, or opaque provenance.

When lowering re-roots a projected place at a loan whose source is a
reference carrier itself — a reference parameter's entry loan rather than a
body-local referent place — the composed loan-source place spells the carrier
dereference explicitly before its remaining field or index projections.
Disjoint sibling-field receiver borrows reached through one mutable reference
parameter therefore each name their exact `[dereference, field…]` place, which
keeps them field-disjoint for verification and lets the backend key every
field projection to one concrete class instance.

A reference binding that retains a borrowed call result keeps that call
result's own loan and adds itself as a carrier, whether the loan is a root or
already hangs from the parent its retained semantic loan names. Lowering does
not end the call result and recreate a child loan from the receiver or
argument origin. Such a recreated loan cannot name the returned address for
backend publication, and its source place would record the returned referent's
type over a projection chain that derives the receiver subobject's — a place
no later stage can key coherently. A derived call-result chain whose links do
not already end at the retained loan's semantic parent still normalizes
through that parent, so parent suspension and reactivation are preserved.

When the call's own argument or
receiver was already a borrowed call result, the retained loan is the outermost
link of a call-result reborrow chain: lowering keeps that link rather than
ending the chain and recreating a loan from the receiver or argument origin,
because the recreated loan cannot name the returned address and its source
place would record the returned referent's type over a projection chain that
derives the origin's. The links the retained loan reborrows carry nothing
themselves, but a parent must outlive its child on every edge, so they stay
live past the full expression that produced them and end together with the last
loan that reborrows them, child-first. Loan release by expression extent alone
would end such a link while the retained child is still active, which
verification rejects.

A reference binding that retains a call-result reborrow chain keeps that
chain's outermost link and adds itself as a carrier, including when the chain
already bottoms out at the loan the retained semantic loan names as its parent.
That is the shape a reference carrier produces: a reference parameter's entry
loan, or a local reference binding's loan, is what gives a retained loan a
semantic parent at all. Lowering does not end such a chain and recreate a child
loan from the semantic place, and it decides before materializing that place.
The place records the returned referent's type over a projection chain that
stops at the parent loan plus the receiver subobject, so it can name neither
the returned address for backend publication nor one coherent referent for any
later stage — and a body that records it fails the emitter's place-coherence
scan even when nothing uses it. The links the retained loan reborrows stay
live for its extent, keeping every parent outliving its child on each edge, and
the owning scope ends the whole chain child-first.

Returning a read-only reference parameter directly uses the same transfer as
the by-value carrier and summarized call-result returns above. The parameter's
seeded `Parameter` entry loan is the body's whole-extent capability and has no
producing instruction, so it cannot become the escaping `Return` loan by
reclassification: nothing in the body would ever name the address the return
publishes, and a backend can only default-initialize that pointer. Lowering
publishes a fresh parentless escaping `Return` loan from the entry loan's own
source through one `Borrow` whose single `Loan` operand names the entry loan,
and retires the entry loan at that same instruction boundary. Local read-only
aliases of the formal still share the one entry loan and extend its carrier
set; only the escaping dependency is published separately. The verifier admits
exactly this forward — a `Parameter` entry loan carried by its own
projection-free reference-typed formal place, matching source place, non-
widened access, and an immediately following `EndBorrow` of the forwarded loan
— and continues to reject every other parentless `Borrow` from a `Loan`
operand. No body may record a loan that is at once `entry` and `Return`.

`MirRangeIteration` owns the bounded range exceptions. It names the exact
owner/range/iterator/sentinel bindings, strategy, access mode, parent range
loan, child element loan, condition/body/increment/cleanup blocks, and
owned-temporary drop. A fixed-array plan ties its dynamic index place to that
inventory rather than claiming general indexed-place disjointness. A
structural mutable plan carries the exclusive parent through the
compiler-generated iterator returned by `begin_mut()`; mutable dereference
creates only the named child. Lowering ends descendants child-first before
increment and on `continue`, `break`, `return`, normal cleanup, and failure
cleanup. An escaping owned temporary is impossible because its drop follows
the final parent loan on every path.

The M-OWN-04 read-only plan additionally names one
`expectedPayloadExtraction`. Verification requires an exact `ExpectedValue`
call with a read-borrowed receiver, one local wrong-state failure origin, an
`Invoke` paired to that call, a non-escaping `CallResult` loan rooted in the
complete named Expected place, one exact payload loan-root place, and dominance
over range entry. A by-value Expected parameter remains native parameter
storage; the backend may transfer it into its MIR binding and defer its final
drop to native parameter scope only when that exact verified range relation is
present. No other Expected call-result loan receives this representation
permission.

Verification requires semantic, HIR, and MIR access modes to agree; a mutable
element loan must be a non-escaping child of the exact mutable range parent.
It rejects a detached or widened child, overlapping active owner use, missing
or premature `EndBorrow`, inconsistent loop edges, and a mismatched or missing
temporary drop. For an Expected payload range it also rejects a missing or
forged extraction, failure edge, source root, loan result, or dominance proof.
The backend consumes this verified plan. It does not infer range lookup,
invalidation, extraction lifetime, or cleanup from emitted C++.

`MirPrinter` must remain deterministic and address-free so tests and future
tooling can compare snapshots.

## Current Completeness Boundary

MIR is the sole executable-body authority for the production C++ backend. It
represents CFG, scalar and aggregate operations, places, calls, moves, loans,
raw-memory operations, drops, construction metadata, exclusive-reborrow
parent/child transitions, full-expression/lexical obligations, normal and
defined-failure edges, and use-def relationships. It does not own object or
vtable layout, calling conventions, native ABI spelling, or declaration and
generated-representation inventory. The `LoweredProgram` frontier combines
optimized MIR with those complete backend-neutral facts before any backend
runs. Some operation families also retain bounded verifier contracts rather
than one uniform materialization model; that is verification debt, not an
executable AST/HIR fallback.

It also does not yet completely implement
[Execution Section 4.2](../language/execution.md#42-evaluation-order). For the
bounded ordinary-invocation and concrete `operator()` slices, the verifier
requires a function receiver when applicable, argument inputs in exact
source/parameter order, and the final `Call` or `Construct` to form a strict
dominance/order chain. A read or mutable callable receiver uses its exact
borrow capability; an explicitly moved receiver or exact once-callable target
requires a `MoveValue` receiver checkpoint and ownership transfer. This
preserves an enclosing once-callable contract when exact selection falls back
to a read or mutable call operator. A scheduled constructor must have
no receiver and must retain its exact ordinary constructor target.
Verification rejects a direct unprepared operand, wrong
call-site, duplicate or abandoned checkpoint, swapped argument index,
type/role drift, and invocation before setup. Class-copy inputs additionally
require the exact copyable, non-borrowed source place. Class-move inputs require
the exact movable, non-borrowed materialized value. For ordinary calls, both
forms require a distinct caller-owned prepared-parameter obligation: copy
initializes it, move reparents the exact active source obligation when present,
and the final call transfers every stage exactly once. Constructors retain the
older direct checkpoint transfer until their partial-construction model lands.
Borrow-source, callable-source, ownership, and loan-flow checks trace through
the checkpoint's one underlying operand.

A `ReadTemporaryBorrow` or `MutableTemporaryBorrow` receiver input names one
mutable materialization place produced by the exact ordinary construction or
GTI call. A linear value uses its value-rooted place; a branch-only value uses
the one hidden temporary slot already required to carry its conditional drop
through a CFG merge. The shared MIR materialization query accepts only those
two exact forms and resolves one producer/source identity for verification and
backend consumption. The producer strictly precedes the input; the input
strictly precedes all arguments and invocation. Exactly one matching value-drop
obligation is activated by successful production and remains attached to the
enclosing full expression. Verification also requires the selected non-static,
non-virtual GTI target, concrete owner type, receiver qualifier, result
non-escape traits, producer type, place type/access/source identity, and drop
descriptor to agree. This is an explicit MIR borrow schedule, not a native
rvalue convention.

An ordinary-call argument may use `ReadTemporaryBorrow` only when its HIR
input names an exact fresh class/struct producer and its selected parameter is
read-only `T&`. Lowering materializes the producer's exact mutable value-rooted
place, emits `BorrowRead`, and attaches one `CallArgument` loan to the
`CallInput`. The loan is active while later arguments are prepared and ends in
reverse temporary-argument order immediately after the call, or at the start
of both normal and failure successors for an `Invoke`. The producer's one
full-expression drop remains independent and follows the loan end. Verification
rejects a mutable/projected/wrong-type place, a native or declaration-only
producer/target, borrowed state, an escaping result, producer/input reordering,
premature or missing loan ending, and a missing or duplicate drop. The backend
therefore consumes a complete schedule rather than relying on C++ reference
lifetime rules.

An ordinary-constructor argument may use `MoveBorrowedTemporary` only for the
corresponding HIR input: one exact fresh GTI call result of the same concrete
class type as the selected by-value parameter. Lowering assigns the successful
producer result its one value-drop obligation, emits the move checkpoint, and
reparents that obligation to the constructor parameter before construction.
Verification requires the producer, parameter, concrete constructor instance,
input order, full-expression, and success/failure cleanup edges to agree. It
also rejects a copy, duplicate transfer, abandoned producer drop, or second
destruction. The role is intentionally not available to general borrowed-state
class arguments.

Other calls and expression families still rely on ordered MIR produced by
recursive lowering rather than a dedicated verifier-visible materialization
schedule. Their emitted execution still comes from MIR, but the verifier has
less structural evidence with which to reject a malformed producer. The
remaining schedule work includes broader borrowed-state class parameters,
generated special construction, target/result placement, general pack
operations beyond `PackFold`, unresolved callables, and more uniform partial-
construction and rollback contracts. Module MIR owns the verified source-
graph-derived initialization schedule, and the hosted adapter executes that
schedule through the planned `Module/0` body.

`PackFold` has its own bounded verifier contract rather than pretending its
element calls are ordinary scheduled call syntax. Verification requires the
recorded element count, source-pack order, concrete element types, parameter
positions, and per-element function instances to agree with HIR and with the
one semantic pattern declaration. It rejects a substituted target from a
different overload/declaration, a missing or duplicated element, reordered
types, a mutable/consuming element parameter, or fixed-operand drift. The
operation executes its element calls left to right and keeps ordinary unknown-
call conservative effects: it is not speculatable, removable, or reorderable.
This metadata does not introduce general MIR pack iteration or ask a backend to
perform semantic instantiation.

Native callback conversion has one program-level representation contract.
Each `MirNativeCallbackAdapter` copies the exact HIR adapter identity and adds
the concrete target's defined-failure effect, the
`TerminateInvocation` containment policy, and the requirement to catch native
exceptions. The producing MIR value references that row. Verification rejects
a missing, stale, reordered, owner/member, external, signature-mismatched, or
non-single-threaded GTI adapter before a backend sees it. Callback thunk text
and calling-convention spelling remain backend representation choices; MIR
owns the executable target and containment decision.

M-LIFE-01 supplies body-local temporary identity, lifetime start, transfer or
reparenting, active drop, and LIFO full-expression obligations. MIR v18 added
caller-owned ordinary-call parameter stages and an edge-initialized cleanup-
owning call-result shape. Later M-EXEC-01 slices should make that ordered input
representation uniform across borrowed-state class values, remaining call
forms, result and target places, operators, and branches. Program
initialization already has a dedicated plan, per-block
`ProgramInitializationStepId`, production body route, and hosted containment
adapter.
The verifier must reject use before materialization, duplicate target
evaluation, invocation before parameter setup, cleanup-state mismatch at an
edge, and a boundary with a live untransferred obligation. A structural edit
that changes those regions invalidates and rebuilds their schedule and cleanup
facts.

MIR now carries exact local possible-outcome sets, snapshot-local origin
anchors, their verified artifact-local site IDs, the immutable artifact
descriptor, and direct/virtual/constructor/callable propagation identity. Any
such operation projects conservatively to `mayTrap`, making it
non-speculatable, non-removable, and non-reorderable, but the structured
identity remains the authority. Eligible checked scalar computations, loads, and ordinary calls now carry
verified fixed record, normal/failure successor, LIFO cleanup, and propagation
edges in every value position of a function, lambda, module, destructor, or
hosted-startup body: eligibility is position-independent, a nested detector's
failure edge drops the live temporaries and prepared owner stages recorded at
that point, and a full-expression-root ordinary call may still initialize one
cleanup-owning result on its success edge. A lexical result that is the direct
initializer of one binding may instead name that exact destination; its sole
`Initialize` consumer is required on the normal successor. Assignment
destinations and compound place schedules, other nested cleanup-owning and
borrowed results (an owning result without an exact destination still
initializes through its re-homed temporary and stays unrouted until the
remaining owning-result materialization lands),
constructors, field/module initialization stages, failure-capable destructors
and double failure, and partial construction remain outside that general
bounded verifier family. Constructor bodies now participate: each
completed subobject transferred into `this` arms a `ConstructionRollback`
obligation, so a defined-failure edge drains exactly the subobjects already
built instead of propagating through an empty cleanup block. Field and
static initializer bodies remain excluded until their construction schedules
are staged the same way. An operation that itself produces a loan is
excluded for the same class of reason: the loan record and its reborrow parent
link are created before the instruction, so a failure edge would end a loan
the operation never produced, and the loan-flow verifier rejects that as
ending a parent before its child. Admitting those operations requires a
success-edge loan on the invoke terminator, analogous to the cleanup-owning
result obligation, plus loan-flow liveness that starts at the normal
successor. The general body emitter validates each complete caller-to-callee
record route and hosted containment before emitting failure-form text. Broader
forms are still required by
[Execution Section 4.10](../language/execution.md#410-defined-runtime-failure).

The checked-scalar verifier no longer accepts an arbitrary valid failure
code/detail pair on the first integer production family. Signed and unsigned
addition, subtraction, and multiplication name their exact overflow detail;
division always names `division_by_zero:integer_division` and adds
`integer_overflow:division` only for a signed result domain; remainder names
only `modulo_by_zero:integer_modulo`. Each shift always names its direction's
`shift_count_out_of_range` outcome and adds `negative_shift_count` only when
the count domain is signed. Negation requires a signed result domain and names
`integer_overflow:negation`. An integer conversion names
`numeric_conversion_out_of_range:numeric_cast` exactly when the complete
source domain is not contained in the destination; a widening/safe-domain
conversion must carry no stale origin, site, or record. Float conversions and the
narrowing compound place schedule are deliberately outside this bounded
verifier family. A same-domain compound assignment or increment/decrement is
no longer a closed operation: it lowers to an ordered `Load`/`Compute`/`Assign`
schedule whose arithmetic satisfies this contract directly. A narrowing form
keeps its closed `Assign`/`Modify` instruction because semantics folds the
arithmetic and the checked conversion into one HIR-authored origin, and MIR may
not split that origin across stages; per-stage origins are the prerequisite for
completing the convert stage. The general C++ MIR emitter consumes every
operation in this fixed-integer allowlist only after both MIR verification and
representation preflight.

The bounded `Invoke` family accepts either a trivial scalar result or the exact
cleanup-owning result obligation of an eligible ordinary call. Scalar values
remain usable only from the normal successor. An owning result stays
uninitialized on the failure edge and is initialized by an explicit lifecycle
event only on the normal edge; the verifier rejects failure cleanup or use that
assumes otherwise. For a class result, production C++ emission additionally
proves that the result has one exact consuming `Initialize` into matching
uninitialized lifetime storage, or uses a dedicated discard slot. The callee
constructs directly into that address and the caller engages the slot only on
the normal edge; default construction followed by assignment is not a valid
publication implementation. A plain class-returning call uses the same exact
destination proof and direct construction. Eligible class-value parameters are independently staged in
caller-owned storage before the call and transferred only when the callee
begins. If a later exact scalar argument root detects a local failure or
propagates through an otherwise ownership-free static direct call, its `Invoke`
failure edge destroys every earlier prepared stage exactly once. Direct-call
propagation has no local site and forwards its existing fixed record unchanged.
The normal path continues argument setup and only the final outer call transfers
those earlier stages. The verifier derives this relationship from the operation
result, exact indexed `CallInput`, dominance, failure identity, and prepared
obligations rather than trusting new serialized metadata. Nested calls with
their own owning parameter or result state, borrowed results, constructors,
assignments, and class-valued normal-result merging remain separate work. The
implemented scalar block-parameter vocabulary does not change owning result
publication.

Cleanup blocks forward the same record through supported initialized and
partially initialized shapes to the selected hosted, callback, or managed-task
boundary. A second origin during primary-record cleanup routes the fixed
emergency envelope through `TerminateCleanupFailure`. The generated managed-
task adapter hands ordinary or emergency records to worker-owned runtime state
only after that verified cleanup. Destruction of the transferred callable and
its captures is the final task-entry cleanup phase: one failure becomes the
worker result, a failure after an existing worker record completes the
`GTI-R0014` envelope, and a third failure is an integrity termination. Explicit
or automatic join re-raises the resulting record on the joining thread. The
same representation supplies reusable boundary
primitives consumed by the same-thread native callback adapter. Foreign-thread
callback and E-EMBED-01 rows can integrate without changing the failure effect. The
verifier must reject missing/forged sites, origin-incompatible
categories, a propagating edge that re-sites or rewrites the record, a normal
result used on the failure edge, and cleanup/control-flow joins with mismatched
record state. Optimizers preserve the first observable origin, site, cleanup,
and prior effects.

Every source executable body now reaches the general verified-MIR C++ emitter.
Functions, constructors, destructors, lambdas, class initializer bodies,
module initialization, and hosted startup all retain their exact MIR identity.
The emitter preflights the complete program before text generation, so an
unsupported body shape rejects the compilation instead of selecting an older
AST/HIR implementation.

The historical labels `scalar-leaf-v1`, `scalar-cfg-v1`,
`scalar-direct-call-v1`, `class-default-cleanup-v1`,
`owned-lifecycle-call-v1`, and `scalar-failure-callgraph-v1` describe the
incremental migration only. Current generated comments retain a small subset
of labels to make ABI forms and body identities observable in tests; they are
not separate executable authorities.

MIR retains explicit source or identity-fold provenance for every rewritten
`Compute`/`Literal`. The verifier requires a rewritten literal to retain an
exact dominating, same-typed MIR identity chain to its original literal.
`LoweredProgramBuilder` therefore consumes the MIR proof rather than a HIR
replacement table and exact-checks source-to-optimized coherence before
publishing the backend contract.

The remaining MIR architecture work is to strengthen bounded verifier
contracts and operation materialization. It must not reintroduce semantic
inference or executable source-tree walking in a backend. See
[`backend.md`](backend.md) and
[`docs/plans/implementation-sequence.md`](../plans/implementation-sequence.md).
