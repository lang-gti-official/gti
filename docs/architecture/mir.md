# MIR

Status: implemented structural CFG, ownership/effect, synchronization,
lifecycle, defined-failure control flow, and sole production executable-body
authority.

MIR lowers each concrete HIR body into body-local control flow, values, places,
resolved calls, ownership operations, and cleanup. Verified optimized MIR is
the executable component of the backend-neutral `LoweredProgram`; `MirProgram`
alone is not the complete backend API.

## Representation

`MirProgram` and `MirLowerer` live in `include/gti/mir.h`. Compiled repair,
verification, and deterministic printing live in `src/compiler/mir.cpp` and
`src/compiler/mir_printer.cpp`.

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
non-payload enum spellings drawn only from the extracted
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
global/static validity. MIR verification also rejects represented
synchronization operations in the single-threaded profile, so a backend or
transform cannot introduce concurrent behavior after semantic checks.

The deterministic serialization is currently `mir-v38`/`mir-body-v38`
(v38 added caller-expanded default-argument provenance to ordered call inputs
and explicit-argument counts to constructor initializers; v31 added the
prefix-initialized storage type kind and its intrinsic
family alongside the logical-size bounds check; v32 added the
`ComputeFold` literal provenance; v33 added the `BranchFold` terminator
provenance; v34 added lambda parameter bindings).

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
Version 20 introduced function-definition provenance and the first MIR-owned
defined-failure effect summary. Each concrete function records
`DefinitionKind::Source`, `RuntimeBinding`, or `Declaration` and a
`mayRaiseDefinedFailure` bit. Version 21 extends definition provenance and that
bit to concrete constructors and destructors and makes
`MirDefinedFailureEffects` the canonical three-kind result. Source constructors
and destructors are distinguished from declarations; a runtime definition kind
is not valid for either lifecycle body. Version 22 adds the pointer-free merged
program-initialization unit/step plan and serializes each block's exact
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
destination's own unwinding rule. Version 35 records the exact binding place
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

`deriveMirDefinedFailureEffects` starts every function, constructor, and
destructor conservatively at `true`. Its function component retains version
20's source-defined, closed, acyclic scalar-CFG/static-call proof and also
recognizes the exact `class-default-cleanup-v1` construction and normal-cleanup
schedule. The proof additionally admits passive string-view parameters,
places, values, literals, and call operands (a trivially droppable value view cannot
raise), admits reference parameters together with scalar field loads through
their dereference carriers (reading through a compile-proven borrow cannot
raise, exactly like a read-only receiver's field load), accepts a C-linkage or runtime-binding call target by its
`FailurePropagationKind::None` language contract instead of recursing into
a body that does not exist, and accepts the six wrapping/saturating
integer-arithmetic intrinsic calls, which carry no failure channel. Its destructor component can prove `false` for bounded source bodies
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
patched independently of `MirProgramEditor`; the derived value-use index is
rebuilt before an exact whole-program comparison. There is no authorized CFG,
branch-target, call-target, operation, operand, lifecycle, layout, or header
rewrite today. This comparison is structural rather than based on
`MirPrinter`, whose deterministic diagnostic format intentionally does not
encode every snapshot-local identity or declaration field.

Version 18 added caller-owned `PreparedParameter` obligations for eligible
class-value inputs to ordinary calls. Copy inputs initialize their stage, move
inputs reparent the exact source obligation when one exists, and the final
`Call` transfers each stage exactly once when the callee begins. It also added
normal-success lifecycle events for one bounded cleanup-owning ordinary-call
result shape, leaving that result uninitialized on the failure edge. The same
schema now supports an exact local scalar argument detector or un-sited static
direct-call propagation after one or more prepared stages without adding
duplicate failure or cleanup metadata. Version 17 added bounded
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
- explicit initialize, assign, modify, move, borrow, call, construct, drop, and
  end-borrow instructions, plus carried read/move/reinitialize ownership events;
- for the bounded ordinary-invocation slices, one non-removable,
  non-reorderable `CallInput` checkpoint per receiver and argument. Each
  checkpoint retains the source HIR value, call-site identity, receiver or
  exact argument-index role, selected parameter type, and value/class-copy/
  class-move/read-borrow/mutable-borrow mode. Argument checkpoints additionally
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
  constructor, callable, and future task-join propagation are separate
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
- one bounded failure-control-flow family for a failure-capable scalar
  `Compute`, `Load`, or `Call` that is itself a selected full-expression root,
  plus an exact scalar selected as an indexed ordinary-call argument after at
  least one earlier owning parameter has been prepared. The nested operation is
  either a local `Compute`/`Load` detector or a static direct `Call` propagation
  with no local site, nested owning-parameter transfer, or owning result. Every
  eligible operation has no destination, loan, ownership event, reference
  result, or active-drop result and occurs in a function or lambda body. The
  operation is the final instruction in an `Invoke` block. Its normal
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
- exact confined invocation and confined/owned argument-boundary records. The
  verifier permits this metadata only on calls; requires descriptors to be
  ordered, unique, within the call's operand list, and identical to the
  concrete target contract; and requires a confined invocation boundary if
  and only if the receiver traces to the matching enclosing callable-parameter
  binding. An owned call argument must be the exact moved parameter operand.
  An owned callee must either return that exact moved binding as a single-use
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
inventing a path to the switch exit. The current extracted fields are passive
immutable copies, so this is not yet a move/borrow projection model.

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

`verifyMirProgram` checks identity ranges, definitions and uses, terminators,
call/constructor and synchronization metadata, native-linkage invariants,
program-entry adapter
metadata, value availability, reachable loan state, and lifecycle state. It
also requires each lambda instance's exact type to reproduce its declaration,
result, parameters, and captures, and requires callable targets to match that
full type. Same-shaped closures from distinct enclosing generic instances are
therefore not interchangeable. It rejects an adapter
identity on an ordinary or no-argument function, a malformed owned-argument
entry shape, and multiple MIR entry points. A value use in its defining block
must follow its defining instruction. A reachable cross-block use must be
dominated by the definition; this includes values referenced through place
roots and index projections. Current MIR has no block-parameter values: every
`MirValue` has one instruction definition, while constants, parameter
bindings, and entry loans use their own representations.

The program verifier additionally rejects every synchronization operation
when `MirProgram` selects the default single-threaded profile. The concurrent
profile permits the records but does not by itself make a public concurrency
API available. Public wrapper semantics, trusted capability selection,
task-transfer proof, runtime support, and backend lowering remain separate
prerequisites.

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
uninitialized for roots that participate in an explicit move. The sparse
fixed point does not impose lifecycle state on unrelated legacy MIR places.
Moves require an exact available key, element assignment restores that key,
ancestor uses observe moved descendants, and different constant elements are
disjoint while dynamic selections remain may-alias. Branch and loop joins use
state-set union. A mismatched domain, forged move key, missing restoration, or
unavailable operand makes the MIR candidate invalid rather than producing a
late source diagnostic or relying on generated C++ behavior.

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
construction schedule. Base subobjects, static-field initializer bodies,
and the destructor double-failure envelope remain M-FAIL-01 work.

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
ordered obligation membership, and emits one standalone boundary marker after
the reverse-order cleanup sequence. Verification rejects an absent, duplicate,
misplaced, or forged marker; cleanup before a completed root; active state at a
boundary; a swapped cleanup sequence; double or missing drops; forged cleanup
metadata; use of a non-consuming operand as an ownership transfer; and
predecessor states that cannot justify an event's conditionality. Lifecycle
instructions and boundary markers are observable effects and cannot be erased
merely because they produce no SSA result.

Each drop obligation retains a stable HIR construction ordinal. Reached
full-expression drops are a contiguous suffix immediately before their sole
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

## Controlled Optimization Edits

`optimization/rewrite.h` exposes a deliberately narrow `MirProgramEditor`.
Core MIR identifies bodies by `MirBodyAddress`, a GTI body kind plus owning
instance ID; editor instructions add a block ID and zero-based instruction
index with an expected instruction-ID and operation guard. The editor never
exposes mutable program vectors or retains instruction pointers across an
edit.

The first supported edit replaces a verified computation with a literal while
preserving instruction/result IDs, type information, unsafe classification,
and HIR/source provenance. All queued patches are checked before mutation and
applied to a copy in deterministic body/block/index order. Touched bodies have
their value-use indexes rebuilt, then the complete candidate program is
verified before the copy is committed. Invalid, stale, duplicate, or malformed
patch sets therefore leave the input byte-identical.

This replacement cannot change CFG, reachability, place/loan topology, or
dominance. Its edit result records those facts as preserved while marking
instruction facts and value uses invalidated and the latter repaired. There is
no general pass manager, analysis cache, incremental dominance update,
insertion/erasure API, or ID compaction yet; each is added only with a concrete
transform that needs it.

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

One MIR loan may name multiple carrier bindings for a shared read-only
semantic loan. It still has exactly one producer and one path-sensitive loan
state; a shared loan uses its active and inactive states. Ending it invalidates
every carrier simultaneously, and verification rejects any subsequent carrier
use or inconsistent state at a join. Each local reference carrier's
`Initialize` separately retains its physical address-source place, so sharing
one provenance identity does not erase the runtime alias chain.

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
assignments, and general normal-result block parameters remain separate work.

Cleanup blocks forward the same record through supported initialized and
partially initialized shapes to the hosted-program boundary. The future
double-failure extension must construct the fixed emergency envelope when a
second origin occurs during primary-record cleanup; current MIR does not yet
represent that path.
The same representation supplies reusable boundary primitives consumed by the
same-thread native callback adapter. Later task, foreign-thread callback, and
E-EMBED-01 rows can integrate without changing the failure effect. The
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
