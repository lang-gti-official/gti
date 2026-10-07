# MIR

Status: implemented structural CFG, scalar block parameters, verified
per-instance effects, ownership, synchronization, lifecycle, defined-failure
control flow, and sole production executable-body authority.

MIR lowers each concrete HIR body into body-local control flow, values, places,
resolved calls, ownership operations, and cleanup. Verified optimized MIR is
the executable component of the backend-neutral `LoweredProgram`; `MirProgram`
alone is not the complete backend API.

## Representation

Raw address conversion uses `Compute/Convert` with one evaluated pointer value.
The independent `rawPointerConversion` query is shared by body verification and
production backend admission: only typed/void or same-pointee conversion is
permitted, pointee constness cannot drop, and no loan, raw-memory access,
ownership transition or failure metadata can be attached. Semantic pointer
conversion intrinsics lower away before MIR. Existing numeric `Convert`
contracts are not relaxed. The backend converts between integer-backed address
carriers without reconstructing a native pointer; conversion of an expired
address stays inert and does not establish permission to access it.

### Borrowed expected extraction

Prefix propagation and `expect` on `expected<T&, E>` lower to a state branch
and a checked read-only `ExpectedValue` loan, not an owned value temporary.
The success result forwards that loan and its exact runtime place; only the
error is copied/moved or translated into terminal panic. A conditional
reference result currently requires exactly one normally completing arm.
The extraction shares the external owner's provenance independently of the
carrier shell, so carrier cleanup cannot destroy the referent or terminate
its retained dependency. Reference-return publication preserves the actual
extraction producer while retiring body-local loans. MIR verification and
the existing production reference representation remain authoritative; no
AST/HIR-only backend path is introduced.

### Terminal calls and caller locations

Concrete functions and calls retain semantic `neverReturns` and `tracksCaller`
contracts. A terminal call has an explicit `Unreachable` normal continuation;
its defined-failure successor uses the ordinary failure record and reverse-order
cleanup. Enclosing expressions do not manufacture an SSA result or evaluate
later arguments after a terminal operand. The impossible normal continuation
may retain lifetime bookkeeping and the primary full-expression boundary, but
not ordinary computation or a return. Independent verification checks both
the selected declaration contract and that continuation. A terminal source
function cannot contain a reachable `Return`.

Caller transport is a separate fact from failure effects. A tracked MIR call
contains the exact semantic `CallerLocationOrigin`, its canonical artifact-local
site ID, and whether the enclosing tracked function forwards its incoming token.
`MirBody::hasCallerLocation` must agree with its concrete function declaration.
`FailureMetadata` binds caller anchors separately from actual detector origins;
the artifact descriptor admits the union of genuine detector outcomes and the
explicit-panic outcome at caller anchors. A caller anchor does not invent a
local failure or make a failure-free wrapper fallible.

The trusted `Panic` intrinsic reads one text view and is terminal, caller-tracked,
and a conservative optimizer barrier. At detection it selects the caller token
and snapshots the message into the version-two runtime record before cleanup.
The local detector origin remains independently verified. Calls, joins, and
cleanup propagate that record unchanged. No public `std::panic` spelling,
ambient thread-local caller state, native unwinding, or backend-inferred control
flow supplies authority. Deterministic MIR serialization records the declaration,
instruction, body, and caller-assignment facts (`mir-v94`, `mir-body-v79`).

The first caller-token consumer is panic. This does not change
`source_location::current()` to inspect an incoming dynamic token, extend
tracking through native function pointers, or implement a call stack.

### Constant-execution foundation

`mir_constant_evaluator` provides a bounded execution service over
canonical unoptimized MIR. It first runs ordinary whole-program verification,
then applies a bounded constant-execution profile at the selected function and
each executed operation. The initial profile covers by-value scalar functions,
local storage, checked arithmetic/conversions, branches, integer/character
switches, loops, recursive
constexpr calls, fixed arrays, and read-only text inspection. Fixed-array
`Aggregate` construction, default elements, by-value passage, size observation,
and nested `Index` places use exact typed constants. Binding and temporary
projections share one canonical root cell; writes replace the immutable array
along the selected path, preserving independent copies. Array materialization,
projection traversal, and backing-vector copies consume the shared step budget
before allocation. Index bounds fail at the executing MIR instruction.
Passive structural records also admit by-value parameters, local copies,
field/index projection composition, field mutation, and returned values. Their
exact class instance supplies field-symbol/type identity; external constant
arguments are checked recursively for complete, exactly typed fields. Passive
drop bookkeeping retires local storage without invoking hidden user code;
active cleanup remains ineligible. Source constructors with explicit semantic
constexpr eligibility execute the same MIR body and produce one fully initialized
receiver. Each field must have one explicit, non-reference initialization stage;
trivial fixed-array fields now retain their exact `Initialize` write as well as
the constructor-stage boundary. Failure-effect proofs require that exact write
for both passive scalar and fixed-array fields; computing an aggregate value
without publishing it into the field is not proof of a completed constructor.
Declaration defaults and nested initializer
constructor targets remain outside this executor profile. Before execution,
the profile checks that every explicit field has its exact MIR initialization
write; a declaration-only initializer must not become a spurious
uninitialized-constant failure. Read-only constexpr
methods inspect an exactly typed receiver through ordinary `This` projections,
including nested fields, arrays, and further method calls. Only constructor
frames may initialize receiver fields; method frames cannot mutate them.
Materialized result receivers read the exact MIR `Value` root; their passive
storage is retired at the represented move/drop, not reconstructed as a binding
or kept alive independently of full-expression cleanup.
Entry string-view dependencies are backed by immutable constant bytes.
Read-only reference parameters, automatic bindings, and returned projections
use verified MIR loans and immutable observations of the exact referent value.
The source loan prevents intervening mutation or movement; no host address or
reference identity is materialized. Borrow/call instructions publish loan
observations, reference binding and dereference read them, and `EndBorrow`
retires the loan. Reacquiring a loan observes the owner's current value, including
across loop iterations. Source reference-return validation still prevents local
escape. Reference results are read values at the external constant-service
boundary, not persistent reference constants. Only a leading checked dereference
of a read-only reference binding is transparent; arbitrary pointer projections,
mutable loans, stored-reference carriers, allocation, and runtime calls remain
ineligible. Untaken runtime-only
calls do not execute. Rewritten/tombstoned instructions and optimizer-produced
control flow cannot determine constant eligibility.

The service returns `ConstantValue` or a failure carrying the concrete body
address, block, and instruction identity, including constructor failures.
One shared step budget covers instructions and
terminators across calls, with an independent call-depth bound. Integer,
floating-point, conversion, and text operations reuse `constant_evaluator`
kernels rather than host arithmetic or native C++ evaluation.

Semantic constant requests now use this service for completed function, method,
and constructor roots through staged HIR lowering. Direct calls do not need a
free-function wrapper. Tests assert execution
metrics with the frontend stopped at semantics: array extents, static
assertions, and compile-time branch selection do not wait for whole-program
HIR. Located MIR failures map back through the staged HIR source expression.
The source language still has a broader existing subset than this initial MIR
profile. Unsupported staging/profile cases retain the legacy evaluator;
executed work is charged to the same budget, and arithmetic/resource failures
never retry there. Concrete type/value generic bodies use ordinary isolated
semantic reanalysis. New checked-reference calls never retry through the legacy
driver. Remaining construction schedules, mutable references,
and returned-view loans remain migration work rather than new exclusions from
the source language.

### Runtime representation

A source `return expression;` in a void body evaluates the exact void-typed
HIR expression through ordinary MIR instructions and failure edges, followed
by full-expression and lexical cleanup. Its `Return` terminator has no value
or return loan. Verification rejects either payload in any void body; no
runtime void value or result-publication obligation is introduced.

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

The deterministic serialization is currently `mir-v94`/`mir-body-v79`, with
`lowered-program-v14` declaration metadata. Version 94 additionally records
generated-default constructor identity explicitly on each constructor instance
and the independent generated-schedule obligation on concrete class rows;
backends must not infer that identity from its parameter count. Version 14
retains that obligation in detached class declarations, so removing both a
constructor row and its selected target cannot authorize a native default call.
The preceding
revision retired sparse capability/intrinsic and initialized-slot proof
identities and preserved deterministic per-function NUL-result policy. Older
version descriptions below record their original additions, not an alternative
current schema.

Concrete class rows retain transfer/share policy and any substituted unsafe
capability condition. The verifier checks that a conditional policy retains
its condition and that its result agrees with the payload's concrete MIR
capabilities; nominal facts remain frontend-owned, while real builtin aggregate
components compose those facts. Removing the condition or changing the derived
capability is invalid MIR. The declaration catalog preserves the generic
condition in `lowered-program-v13`, and optimization replay includes these facts.

MIR also copies the artifact program kind and a pointer-free
`MirEmbeddingPlan`. Each canonical row names the exact source
`MirFunctionInstance`, semantic declaration, signature, public symbol, and
request span selected by HIR. Program verification requires a non-empty ABI-v1
plan for a library, no plan for an executable, strict symbol order, unique
function roots, source definitions with GTI linkage, and no hosted startup or
process-lifetime callback adapter. Optimization coherence treats the entire
plan as immutable non-body policy. `LoweredProgramBuilder` exact-compares the
semantic/HIR/MIR chain and copies the rows into `LoweredEmbeddingPlan`, where
future wrapper and header backends can consume one sealed source of truth.

The combined schema preserves embedding rows for dormant library snapshots
and uses the same current version for executable and library programs.
The program header retains the selected target's identity, pointer width and
execution profile. Every serialized type retains its native C primitive
discriminator separately from its numeric kind; equal-width `long` and
`long long`, and plain `char` versus signed/unsigned char, cannot disappear
from the snapshot. Program verification checks these facts against the selected
target across declaration, aggregate, callable, cleanup and body inventories.
Field-aware borrowed-return and construction contracts serialize each result
field path, selected receiver or parameter, source field path, and permission.
Calls preserve the semantic contract; program verification also checks the
callee's actual return publication or constructor initializers independently.
Matching forged caller/callee summaries are therefore not sufficient proof.
An abstract entry retains caller-owned field identities without treating the
carrier's physical shell as the external referent. Its complete value transfer
is checked against the returned SSA producer. Compact sole-origin carriers
remain representable when nested in an explicitly mapped aggregate; their
field names must not be projected onto the external owner's storage.
Whole noncopyable field-mapped call transfers retire the consumed caller loan
on success, while the returned carrier retains its independently verified
dependencies. Failure cleanup retains its existing explicit MIR edges.
An immediate temporary carrier receiver retains the same complete dependency
set. Its materialized shell has a distinct call-only loan, while every stored
dependency survives through full-expression destruction of owned fields.
Independent verification rejects missing dependencies, changed permissions,
forged shell authority and owner-loan retirement before cleanup. Shared-only
and mixed carriers use one schedule rather than a separate passive exception.
Borrowed-result operators keep their ordinary value receiver stage. When their
result protects the receiver's own storage, lowering and independent verification
select the exact producer-owned temporary and its full-expression drop, retaining
the external stored dependency separately. An exact stored-field return instead
selects that external referent; the two sources are not interchangeable.
The prefix-construction capacity check and its exact append identity are
serialized for deterministic evidence and independent replay.
The new loan contract separates a physical carrier reference from its inherited
external-owner authority. `storedDependency` names one exact `Stored` loan;
unlike `parent`, it does not denote containment or suspend that loan. Dependency
identity, liveness, permission capping and child-before-dependency retirement
are verified independently. Dependencies may form an acyclic chain when an
adapter borrows another adapter. Protected-source and call-conflict queries
traverse the complete chain, and access authority is capped at each edge;
retaining an outer shell cannot lose protection for an intermediate shell or
the ultimate owner. Reference formals and receivers seed an abstract
`Stored` entry naming their concrete reference-field paths, separate from the
physical reference loan. Program verification independently binds function,
lambda and constructor-reference-formal entries to the exact formal (or the
function/lambda receiver) and checks field access against
the concrete class layout. Each local value copy attached to an entry requires
an independently proven, dominating same-type initialization; merely adding a
binding to the carrier list grants no authority.
An abstract entry describes declared field access, not a capability-produced
loan or the caller's cleanup ownership. In particular, borrowing a guard does
not acquire its lock-release obligation. Local bindings of stored referents
retain their concrete reference-field dereference; semantic overlap identities
are not substituted for executable projection chains.
Cleanup-bound nominal carriers are classified from concrete borrowed-state and
active-cleanup facts, independently of the trusted mutex capability. Exact
by-value entries own a binding drop; owning call/constructor publications own
their result drop and subsequent typed transfers. Verification rejects missing
lifecycle markers as well as invented ones. Ending such a loan requires the
exact carrier drop, failed producer, consuming call, constructor-field
publication or owned return transfer. Field publication must reparent the same
SSA value into its exact construction-rollback obligation. Ordinary receiver
and reference entries do not acquire this ownership.
In particular, publishing a reference field to a cleanup-bound object retains
its checked owner dependencies but does not transfer the object's destructor or
create a construction-rollback obligation for the referent. Marking such a
reference publication as lifecycle-bound is rejected independently.
An explicit nested constructor may instead supply that completed SSA value.
Its result loan retains every child field dependency through the successful
value-to-field reparent, then publishes the field's escaping dependency and
retires the consumed child loan. Constructor verification derives each outer
field-to-formal origin recursively from the selected child `Construct` and its
actual prepared operands; matching caller/callee field maps alone are not
proof. Reordered and multi-owner inputs preserve their individual paths and
permissions. The native backend erases this proven publication without
changing the existing field construction or rollback schedule.
An owned return may publish a distinct dependency before intervening fallible
local cleanup. Its result must belong to the original carrier's exact typed
drop-transfer chain. If that cleanup fails, the exported dependency remains
live through destruction of the unpublished pending result; it cannot end
merely because the return was abandoned. Independent verification checks the
exact pending-result drop and rejects an early endpoint.
Concrete HIR class instances also specialize generated copy/move member
availability from semantic traits. MIR rejects a generated member whose
concrete trait forbids it, including generic owners of reference-bearing fields;
the backend does not decide whether a GTI assignment is legal.
Semantic last-use analysis includes cleanup uses and retains ancestor loans.
MIR normal and failure drains interleave carrier destruction with
child-before-parent dependency retirement; a failed producer retires its
provisional result dependency without inventing a successfully constructed
object. Cleanup boundary verification accepts those dependency-end markers
without changing the exact reverse-order drop sequence. Loan and lifecycle
dataflow independently reject premature retirement or forged transfers.
An owning field provably disjoint from all stored-reference paths is not an
access through that external dependency; an ordinary physical receiver loan
still protects its shell. An adapter may consequently own its buffer while
borrowing another carrier. Publishing a carrier-reference constructor field
retains both its physical shell and exact abstract dependency through successful
construction. Failed construction rolls back initialized fields before ending
published loans child-before-dependency. Missing or forged dependencies are
rejected by program verification.
This applies to read-only as well as exclusive carrier-reference fields.
A reference obtained through an owning accessor also retains its exact
transient call-result ancestry until the adapter's endpoint; a coarse owner
identity is not a substitute for the selected constructor input's loan chain.
Conversely, a completed call with no reference or borrowed-state result can
retire an unretained transient accessor loan even when its receiver is an
owning member projected from that accessor. Lowering uses the already
materialized place's exact loan root, not a guessed owner identity. Escaping
loans, attached carriers and live children still prevent early retirement;
borrow-returning calls retain their normal full-expression or carrier endpoint.
A reference return preserves both its physical source and stored dependency.
A copied by-value return instead publishes a fresh external-owner loan from
the exact copied SSA value. Its original entry remains live until physical
references retire child-first, and must end on both value-return and failure
exits. These rules are verified before C++ representation admission. Optimizer
protected-source queries include dependency edges; unsupported profiles such as
constant evaluation remain conservative. Full campaign validation is tracked
in the implementation sequence, independently of this represented contract.
Program v82 reconciles SIMD intrinsic identities with structural relocation
traits in one program contract; it supersedes the separate v81 development
snapshots without changing the body-v69 format.
The contract includes six identity-bound SIMD arithmetic/comparison intrinsic
kinds. Their operands retain three exact immediate array borrows (read, read,
write), with concrete matching float/double lane shape and a bool output only
for comparisons. The verifier rejects stale extent/access/result facts and
normal place/loan verification prevents overlapping read/write operands.
Program v82/body v69 serialize structural relocation eligibility in type
traits and preserve pointer-only native class identity. Class-table
verification compares the semantic fact against exact generated movement,
concrete bases, fields, borrowed state, and cleanup; interface bases contribute
dispatch requirements, not stored value state. Passive primitive kinds are
explicitly admitted; an unknown or newly added type kind is unavailable until
classified, including when nested inside a fixed array. General typed and
prefix-storage relocation select their bulk candidate only from this verified
proof. Prefix intrinsics retain their checked liveness/capacity protocol;
general raw relocation instead requires explicit unsafe validity. Both preserve
source/destination lifetime behavior. There is no backend-only relocation
operation. C++ representation traits may
decline the bytewise candidate, never grant language eligibility.

Restoring exchange and swap are separate sealed call intrinsics, bound to
private prelude declarations. Semantics requires exact matching relocatable
types and initialized writable places. HIR/MIR retain the ordinary exclusive
loans, prepared replacement, owned result and lifecycle tokens. Program
verification independently proves failure-free relocation and the exact
reference/value/result shape; an owning replacement must transfer its prepared
drop obligation. The result activates the old value's distinct obligation.
Neither instruction drops a resource, invokes user code or has an internal
failure edge. Replacement construction and later cleanup retain their ordinary
failure paths outside the indivisible transfer. Optimization treats the
operations as explicit mutable-memory/ownership barriers. The C++ consumer
uses verified result slots and generated relocation; it does not grant
reference consumption or use native move assignment as semantic authority.

Checked indexed swap is a third sealed intrinsic over one mutable fixed-array
or initialized-prefix storage reference and two staged `uint64_t` indices.
Its one local `IndexOutOfBounds/PrivateStorage` outcome preflights both indices
before mutation; valid equal indices are a no-op. MIR verifies exact storage,
access, relocation, result and failure facts. Effects explicitly include
mutable/unknown memory, ownership transfer and trapping. It never calls an
element destructor, user comparator or custom movement operation internally.
Comparators and their failure edges remain ordinary source-defined calls in
the algorithm's MIR.

Owning fixed-array aggregates used as exact by-value constructor arguments
retain their value-rooted lifetime slots until the consuming call. Production
lowering checks the aggregate's element transfers and result initialization,
unique use, exact parameter type, and consuming `TransferOut` before taking
the slot. This also applies to unrelated source-defined constructors. It does
not default-construct or copy an array to transport it, and a missing ownership
publication proof rejects the backend representation instead of falling back
to passive scalar emission.

Restoring call results use those same explicit object lifetimes. Owning
Expected and payload-enum results activate the existing result-slot identity;
fixed-array results construct the caller's uninitialized output storage and
publish only after success. A cleanup-free move-only element still has a
lexical lifetime and cannot be transported through invented default
construction or assignment. Its exact binding initialization replaces a
temporary cleanup-token reparent, since no active temporary cleanup exists.
Pending array returns retain their verified return-result cleanup recipe if
another local's destructor fails before return completion. Independent
mutation tests reject removed result publication records.

Raw-array call results also reuse the existing placement-result forwarding
and value-rooted call-result proofs when returned directly, explicitly
discarded, or consumed by another call. The success edge activates the exact
result obligation, and a consuming call transfers it only after later
arguments finish. A failure in those arguments cleans up the already-created
array result. Cleanup-free record arrays instead reuse the representation-only
SSA lifetime slot: no semantic cleanup obligation is invented, and no default
construction or copying is introduced. The recursive MIR cleanup facts, not
native type traits, distinguish these two cases.

Cleanup-free fixed-array fields, including arrays of passive records, retain
the same explicit constructor field-write stage as cleanup-free class fields.
Their lexical type alone does not imply active cleanup. Program verification
requires that exact stage and field identity; staged constant evaluation can
therefore execute the ordinary MIR move and initialization without falling
back to a separate AST interpretation of movement. Active-cleanup arrays still
use their rollback-armed owning stages.

Program v80/body v68 also serialize the resolved caller literal on each private
source-location intrinsic. MIR verification requires the exact zero-argument,
effect-free call shape, matching scalar/text result, and attached literal;
optimizers and backends therefore cannot substitute their own native build
location. This combines source-location metadata with the constructor const
eligibility and completed-body staging metadata described above.
Program v79 retains semantic constexpr eligibility on constructor instances;
lowered snapshot verification rejects disagreement with the HIR declaration.
Program v77/body v67 preserve complete shared-carrier dependency sets and
the strong-owner intrinsic/cleanup contracts, including exact recursive
array cleanup recipes.
Program v76/body v66 replace a stored carrier's single designated field symbol
with its ordered `designated-fields` path. The complete concrete nominal path
retains one external dependency through nested carrier construction and return.
The combined format also adds exact `payload-variant`/`payload-field` selectors to
aggregate cleanup recipes. Enum roots retain finite active-alternative recipes,
including concrete-class recursive back-edges. Program verification independently
derives and compares the complete recipe. Borrowed `PayloadExtract` values are
places, not owning copies: the verifier checks the exact selected switch edge,
payload type/access, and source loan; backends spell an address into that original
variant. Loan conflicts conservatively protect the whole matched enum.
Program v74/body v64 serialize signature receiver capability in semantic type
metadata. Read-only, mutable, and consuming signatures cannot share a serialized
identity. The lowered declaration seal includes the same field in
`lowered-program-v9`, rejecting a changed class-pattern qualifier.
This pending format also includes `retained-receiver` call inputs and the
distinct `ConsumeReceiver` ownership event: consumed availability does not
imply a physical field move or lifecycle transfer. Function rows also serialize
`owned-pointee` accessor summaries with exact field identity and private borrow
operation, independently checked against their source MIR bodies. Declared
class-field rows retain their read/mutable access independently of executable
places so the proof can reject a mutable borrow of immutable storage.
Program v73/body v63 add `polymorphicCleanup` to unique-owner cleanup recipes.
The root retains the static pointee class identity but selects cleanup of the
complete allocated object dynamically; it must not substitute that base's
static destructor or field recipe. Aggregate recipes can contain these leaves.
Lowering and effects conservatively retain a defined-failure boundary for this
dispatch, including element cleanup and replacement. Program verification
reconstructs the canonical recipe and rejects erased dispatch flags or forged
pointee identities. Static subobject cleanup remains statically selected.
Lambda invocations with an expanded argument pack retain an explicit HIR
receiver and lower to an ordered MIR `CallInput` receiver followed by concrete
argument stages. The call keeps its exact lambda identity, capability and
flattened signature; ordinary program verification rejects a missing receiver
or forged parameter type. No native pack matching selects the GTI call.
Program v72/body v62 add the generated `ReturnResult` drop obligation. A
completed owning return operand is reparented out of its full-expression into
this pending lifetime when callee cleanup can fail. It retains the exact
operand place, HIR value identity, and cleanup recipe, with no independent HIR
obligation or full-expression ID. The recipe may describe the value payload
of an `expected` return or its exact `unexpected<Error>` carrier. Only the final instruction before the matching Return
may transfer it to the caller; failure continuations clean it before older
temporaries and locals. Verification rejects early transfer across an Invoke,
missing reparent/publication, wrong failure-cleanup phase, and a fabricated
full-expression lifetime. Failure-free continuations need no extra obligation.
Exact owned-callable return/field transport proofs recognize this same pending
return lifetime. A failure-only drop of that exact result is not a second
successful use; reparenting alone is insufficient without its final matching
publication. Body lifecycle verification still owns activation and failure
paths, including rejection of missing publication and incorrect cleanup phase.
An owning conditional or propagated success can retain its pending return in
the existing Temporary-rooted join rather than inventing a second value slot.
Verification requires one exact same-typed, source-identified Move transport
from that temporary, and binds final publication to that SSA result. Prefix
`?` error returns use the same pending-return protocol as explicit returns.
Failure cleanup without lifecycle-bound loans drains temporary, lexical, and
pending-return obligations together in reverse construction order; those
lifetimes can interleave when an initializer publishes a binding before its
older full-expression temporaries are retired.
Program v71/body v61 retain zero-based capture projections in aggregate
cleanup recipes. A closure recipe contains each active capture exactly once
in reverse capture order; its source-destructor targets contribute to normal
and secondary-failure edges just like class fields. Program verification
reconstructs the recipe from the concrete capture inventory and rejects missing,
reordered, or forged projections and erased cleanup effects.
An `owned-value` callable parameter contract describes exact owned entry rather
than an exact-return/field transport shape. Its verifier checks the concrete
type, parameter binding/place, availability, and lexical entry-drop obligation.
The ordinary body lifecycle and loan verifiers govern the remaining control
flow; invocation inputs must still prove ownership transfer. Confined callable
contracts retain their separate invocation/forwarding restrictions.
Concrete pack contracts index the flattened signature while bindings retain one
source pack root. Verification resolves each contract to its exact element and
replays closure traits from the concrete captures. Lexical elements require
entry-active projected drops. A forwarded closure has a distinct prepared
parameter obligation: copies initialize it without consuming the pack, while
moves transfer its entry obligation. Invocation transfers that prepared lifetime
exactly once, including failure-capable calls.
Program v69/body v60 add exact C-thread condition initialization, destruction,
signal, broadcast, wait, and timed-wait declarations. The wait calls retain
`ConditionWait` (atomic enrollment/release followed by reacquisition), and
signal/broadcast retain `ConditionNotify`. Notification is not an independent
shared-data publication edge. Both are non-removable/non-reorderable runtime
barriers; their verifier rejects erased, replaced, or atomic-order-bearing
records and forged native signatures. Ordinary native-call emission consumes
the contracts without a new backend body path.

Program v68 extends the exact private C-thread declaration vocabulary with
manual mutex initialization, destruction, lock, try-lock, timed-lock, and
unlock. The three acquisition calls retain `MutexLock`, and release retains
`MutexUnlock`; their language edges are conditional on successful acquisition
or final recursive release. The verifier binds records to exact native
signatures and rejects erased/reversed operations and forged native targets.
The ordinary native-call body representation is unchanged.

Program v67/body v59 make checked fixed-array replacement preparation own its
exact local bounds origin as well as its complete old-value cleanup. It checks
the destination before beginning cleanup, consumes no RHS ownership on either
failure path, and routes through `Invoke` even when the destructor cannot fail.
The paired `Assign` has no local detector or propagation: only successful
validation and cleanup reach its non-failing commit. Each dimension of a
chained array place is checked before accessing the next dimension. The
verifier rejects missing or displaced bounds origins, early ownership
publication, unpaired replacement, and altered success routing.

Program v66/body v58 split represented plain owning assignment into
`CleanupBeforeAssign` and its paired `Assign`. The preparation carries the
independently derived destination cleanup recipe, but consumes no RHS ownership
and ends no representation lifetime. Fallible cleanup uses `Invoke`; only its
unique success successor may commit the replacement. The destination remains
live but disarmed/empty if cleanup fails, so subsequent scope cleanup is safe.
The staged RHS stays active until `Assign` publishes it. Pair identities,
adjacency/success routing, complete destructor effects, and generated non-failing
assignment after cleanup are verifier-owned. The v67 extension adds the
built-in checked array-element destination to that protocol; unrepresented
cleanup and destination families still fail closed.

Program v65/body v57 make recursive ownership recipes finite. On re-entering
an enclosing class instance, lowering records an exact `recursiveCleanup`
back-edge instead of expanding that class again. The back-edge has the same
class/type identity and no body or children; it is never a root recipe. The
independent MIR derivation exact-compares the complete tree, including these
edges. Every reachable source destructor still occurs in the expanded root,
so effect composition remains complete without following a cyclic graph.
The backend calls the already selected class cleanup helper through the
owning pointer or collection representation; it does not rediscover the
schedule from native fields. Empty owners stop traversal. This is a cyclic
*type* graph, not permission to construct cyclic exclusive ownership at runtime.

Program v64/body v56 compose a class's guarded source destructor with its
ordered active-subobject recipe. The body runs first, then reverse fields and
the admitted base recipe. Its failure summary is separate from the complete
recipe: a failure-free body can still have fallible fields. Lowering and the
independent MIR derivation retain both the exact destructor and every child;
the verifier rejects missing/retargeted bodies, missing guards, and incomplete
or reordered children. The backend uses the shared cleanup sequence, preserving
the first failure while draining children and stopping immediately on a second.
Direct values, unique-owned values, nested source-destructor fields, and
storage/vector scope destruction and removal use this same recipe.

For acyclic non-polymorphic classes with generated move assignment and recursively
empty-representable fields, the private native empty-state constructor explicitly
disarms the source destructor. This is not a source-language default constructor.
MIR publishes each completed field and rolls it back before constructor
parameters. Body-ordered parameter publication may be suppressed only when the
same complete native initializer binding also suppresses its move. Plain
constructor emission must prove the ordinary callee actually exists; a
status-only nested constructor cannot be called through an invented overload.
No-failure analysis is not itself proof that a plain constructor body exists:
the representation snapshot retains a contained status wrapper when the exact
field-publication schedule requires in-place construction.

An owning Expected field can use the same private construction/rollback path
when its error alternative has a recursively proved observation-free empty
representation and defined-failure-free structural transport. The backend
constructs that error alternative in place, never default-constructing the
success payload. This is unpublished representation, not a new GTI default
value or permission to assign either alternative. Generated guarded native
moves preserve the source lifecycle guard, including a disarmed private shell;
they must not reactivate an empty error or its nested fields. MIR still owns
field publication, rollback order and the active alternative's cleanup.

Cleanup-free class construction still carries its selected constructor's
failure edge. Absence of a result-drop obligation does not suppress Invoke,
including in a constructor field initializer. The exact success continuation
publishes the field; failure propagates the original record and drains earlier
completed fields. The C++ field-placement proof also admits a failure-free
owning constructor followed immediately by publication of its initialized
value obligation, retaining the same field rollback for later failures. Neither
case invents a cleanup obligation for a cleanup-free result.

Prefix append retains an exact selected source constructor for final-slot
construction, including its reference/value parameter roles after concrete pack
expansion. All arguments are staged before `StorageConstructPreflight`, which
reads the single staged prefix-storage operand and owns its capacity failure.
The retired sparse variant's index and duplicate-slot preflight are not accepted.
Prepared owning arguments remain active on that failure edge. The unique success
continuation begins with the paired construction, transfers prepared parameters
once into the constructor, and
publishes the slot as live only after construction succeeds. This applies even
when the constructor itself is proved failure-free.

The preflight/append pair must name the same storage input, source call identity
and exact constructor. No intervening executable operation or alternative entry
may reach the handoff. Only that exact preflight may add a non-consuming use of
the append's mutable storage input; ordinary prepared inputs remain single-use.
MIR independently checks the selected target, parameter types, argument roles,
dominance, failure sites and cleanup transfer. The intrinsic's storage effects
compose with the selected constructor's effects. The constructor's original
failure record drives field rollback, previous-element and caller cleanup.
Reference, owner-dependent text and unsafe raw-pointer inputs retain their
existing semantic access/escape contracts; native copyability grants no new
borrowing permission or borrowed-payload storage admission.
Concrete pack flattening uses the same bounded `CallInput` type-compatibility
predicate as independent MIR verification. It retains the source operand type
and exact selected parameter type for existing null-pointer and pointee-const
conversions, rather than rejecting a valid conversion or changing the selected
constructor signature. Removing pointee constness and changing pointee types
remain invalid.

Generated same-type copy/move append has no source-constructor target. Direct
arguments and concrete pack expansion retain the same ordered `CallInput`
preparation. The backend must not introduce a native block-local owning
temporary outside that schedule: on capacity failure its destructor could
otherwise hide the original failure instead of using MIR's secondary-failure
cleanup path. Capacity success/failure and failing destructors are checked for
both spellings, without introducing a public container-name rule.

The optional-storage publication capability consumes exactly one completed
element of its exact payload type, with no source-constructor target. Public
emplacement resets the old value and constructs its replacement in ordinary
GTI/MIR before publishing it. Constructor arguments, partial construction and
first failure therefore use the existing construction protocol rather than an
unchecked native constructor hidden inside a storage intrinsic. Whole-program
verification checks the exact element and transfer contract. The inline slot
does not encode external-owner loans: semantic admission rejects borrowed-state
payloads even without an accessor call, and MIR independently checks the
concrete payload's movability and borrowed-state facts, including actual fields,
bases, aggregate components and captures. Neither assignment capability nor a
public container spelling grants admission. Sparse and prefix storage likewise
reject borrowed payloads in their typed places, values, signatures and fields;
an empty storage value is not an exemption.
Publication also rejects an owner-dependent text expression, matching sparse
and prefix storage. Text parameters conservatively retain that dependency even
when a caller passes a literal; the generic optional value constructor cannot
store them until its contract can distinguish static text from an owner loan.
Empty optional text storage remains permitted.

Reset of the same inline capability uses the shared element-removal cleanup
recipe also used by general raw destruction and prefix storage. MIR independently
derives the payload recipe and its destructor failure effect. Fallible reset has an Invoke
edge, with no new local failure site: the payload destructor's original record
propagates through caller cleanup. The representation retires engagement
before running that recipe and ends the disarmed native shell afterward;
failure cannot make the containing owner destroy the payload twice.

Program v63/body v55 extend the explicit removal recipe to `PrefixStorageErase`.
MIR independently proves generated relocation into an empty element slot,
including guarded source-destructor state, every field and base, and recursively
movable capture/aggregate state. This is not an assignment proof: overwriting a
live value still requires its cleanup. Declared/deleted moves and missing guards
cannot satisfy the contract.

Erase detaches the suffix from the prefix before running the selected element's
cleanup. On success, proved-failure-free moves close the hole and restore the
contiguous prefix. On failure, the operation drains the detached suffix in
reverse order, then returns the original record through `Invoke`; ordinary
caller cleanup owns the untouched prefix. A second failure while draining the
suffix terminates before native teardown with both source records. Local bounds
failure changes no live state and retains the erase site's ordinary diagnostic.
The collection matrix covers first/middle/last removal, append after success,
direct leaves, owning aggregates, nested collections, and inherited elements.

Historically, program v62/body v54 added an exact `elementCleanup` recipe on private
`StorageDestroy` and `PrefixStoragePop` calls when the element belongs to the
represented cleanup family. Lowering derives it from the concrete operand;
the MIR verifier independently rederives and compares the entire recipe and
its destructor propagation. These calls retain both their local bounds/state
outcomes and source-destructor effects. Proving a local check safe does not
prove element cleanup safe. Effect composition includes every source destructor
in the recipe, and unrelated instructions cannot carry a recipe. The sparse
`StorageDestroy` operation is now retired: source sparse policy invokes general
`ObjectDestroy` below, while prefix removal retains this recipe contract.

`ObjectConstruct` records unsafe final-address construction with the exact
selected constructor, concrete input types, prepared-parameter handoffs and
constructor-failure edge. It reuses the ordinary constructor body and rollback
schedule. Unlike prefix construction it has no container preflight:
alignment, extent and vacancy belong to the unsafe caller. The verifier rejects
forged storage preflights, borrowed destination authority and mismatched
constructor/failure metadata. Scalar construction and generated transfer retain
their exact typed selection. The backend supplies the address to the selected
verified constructor; it does not infer lifetime permission from C++ traits.

The declaration-bound `UninitializedStorage` class capability supplies only
layout and inert representation. Concrete semantic traits derive copy/move
eligibility from the payload's bitwise relocation contract and propagate its
concurrency capabilities. MIR independently verifies those facts, empty field
shape, generated special members and absence of payload cleanup. The backend
implements a byte representation with no engagement state; source wrappers
use ordinary address formation and existing typed/erased pointer conversions.

`ObjectRelocate` is an unsafe, failure-free `Call` over two identical writable
complete-object pointer types. Verification checks the exact metadata shape
and rederives failure-free relocation eligibility from concrete MIR type facts.
It permits no constructor override, cleanup recipe, storage preflight, loan or
failure edge. Its effects conservatively read/write unknown memory, move and
initialize a value, forming an optimization barrier. The backend performs the
verified generated transfer, then ends the inactive source representation;
it must not invoke source GTI cleanup or acquire/release a byte region.

`ObjectDestroy` generalizes this exact `elementCleanup` contract to one writable
raw pointer. Semantics requires explicit `ObjectLifetime` unsafe permission and
an inferred complete pointee type. HIR seeds the concrete lifecycle declaration;
MIR independently checks the raw operation shape, rederives its entire cleanup
recipe and preserves destructor propagation. It has no local bounds/vacancy
detector or storage owner: raw validity is the caller's unsafe obligation.
The backend invokes only the verified recipe, retires the disarmed native
representation, and returns its boolean cleanup result through `Invoke` without
replacing the original failure record. No byte release occurs. Effect analysis
includes the same exact destructor targets and synchronization effects; the
optimizer classifies raw lifetime end as a conservative memory/cleanup barrier.

Source-defined sparse policy uses private boolean checks for vacancy, liveness,
relocation capacity, source initialization and destination vacancy. Source GTI
computes each condition; the intrinsic only raises its exact existing
`InvalidStorageState` detail when false. The shared intrinsic failure mapping
feeds semantics, MIR verification and lowering, just as the key/container
argument checks do. These calls have no storage inspection or lifecycle side
effects. They remain trapping optimization barriers without a supported
discharge proof, and forged outcome families are rejected independently.

Explicit removal retires the sparse live byte in source, or prefix length in
the retained prefix capability, before cleanup,
ends the disarmed element representation once, and returns to the same MIR
`Invoke` failure edge. The backend-private checked status distinguishes local
errors, which create a record at the removal site, from propagated destructor
errors, whose record is already complete and must not be overwritten. Caller
cleanup drains the remaining live elements. Source-defined vector/storage
`clear()` loops use these same operations. The v63 extension supplies the
arbitrary-index prefix erase hole/shift protocol for the represented family.

The former `StorageTryAdopt`/`StorageAdoption` capability and its private
sparse destination type are retired. Descriptor validation, two-region lease
publication and metadata initialization are ordinary source operations.
The unsafe binding contract establishes provider obligations; general
`ObjectLifetime` calls establish typed construction, relocation and destruction.
No MIR instruction adopts a sparse bitmap or implicitly allocates bookkeeping.

Historically, program v61/body v53 extended the cleanup tree with one unprojected
element recipe for owning storage and prefix storage. The sparse recipe and
native live-slot walker are now replaced by ordinary source reverse traversal
and general `ObjectDestroy`; the remaining paragraph records the original
protocol and its retained prefix implementation. A collection drop walks live
elements in reverse index order (skipping uninitialized sparse slots), runs
the exact child recipe, ends the element representation lifetime, and finally
releases the allocation. The live bit/prefix length is retired before entering
element cleanup. Nested collections share the explicit primary/secondary
failure protocol; no ambient failure state or native exception unwinding is
used. Direct lifecycle-guarded source-destructor leaves, unique owners, and
generated field/base aggregates compose as elements. The v64 extension adds
source-destructor classes with represented active fields; v65 adds recursive
ownership-type graphs using finite back-edges.
The HIR producer and independent MIR derivation must agree on the entire
element recipe; missing, duplicate, mistyped, projected, or inactive child
descriptors are rejected. `collection_cleanup_failure_runtime` covers ordinary
and failure-path drops, sparse slots, nested/moved/empty collections, aggregate
elements, and inherited-base rollback. The v62 extension also tests explicit
pop, slot destruction, clear, and unchanged local invalid-slot reporting.

Program v60/body v52 extend v59's single-owner descriptor into an ordered
aggregate cleanup tree. Generated classes retain every active field in reverse
declaration order, then their active base; unique-owned generated aggregates
retain the pointee schedule before allocation release. Terminal leaves include
lifecycle-guarded source destructors and cleanup-free scalar ownership/storage,
so ordinary `std::string` can coexist with fallible resource owners. The HIR
lowerer constructs the schedule and the MIR verifier independently derives and
exact-compares it. Effect composition retains every source-destructor target,
not one representative leaf. Releasing owned allocation remains a runtime
effect even when those source destructor bodies are empty. Raw allocation
obligations do not gain pointee cleanup.

The outer `Drop` uses the existing `Invoke` and primary/secondary record
protocol. Within the ordered schedule, a first failure is retained while the
remaining children drain. A second failure terminates immediately with both
records. A schedule entered during failure returns on its first new failure,
without native teardown running any remaining children before the terminal
MIR edge. Generated C++ helpers consume the selected steps and eventually tear
down empty representations; they do not consult AST/HIR or use ambient failure
state. `aggregate_cleanup_failure_runtime` and
`unique_pointee_cleanup_failure_runtime` own the focused runtime evidence.
Program v58/body v50 add `InitializeBase` and a constructor-only `Base` place
projection, selected by exact concrete base identity. The operation invokes
its selected constructor in place and arms one `ConstructionRollback` only on
the success edge; it never publishes a standalone class value.
Program v57 adds exact C-thread runtime declarations, registered callback-entry
policy, and hosted startup/shutdown calls. Program v56/body v49 introduced the
resolved `requiresUnsafe` function contract and
`DeclaredUnsafeCall` instruction marker. HIR checks the declaration flag
against semantics, MIR checks direct calls against their exact targets and
rejects unsafe targets in native/managed adapters and function items, and
lowered construction preserves declaration/instance agreement. The detached
lowered serialization advanced to `lowered-program-v6` for the additional
declaration flag. This is caller-permission metadata, not a new execution or
failure mechanism: ordinary calls and existing cleanup still execute from MIR.
Program v55/body v48 add `AllocateObject`, the generated `ObjectAllocation`
drop obligation, and the instruction's exact `object-allocation` identity.
These separate raw allocation from selected ordinary class construction; raw
storage is not a live source value or a source-address capability.
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
Program v44/body v42 added exact proof-carrying safety discharges to instructions
and transformation records. At that stage a discharge named bounds, storage-state, or
arithmetic-overflow family plus the exact local defined-failure origin and
outcome; records retain all related MIR block, instruction, value, and place
identities used by independent replay. The storage-state family and its
initialized-slot proof are retired with the sparse engine; current supported
families are documented in [optimization](optimization.md). Program v42 adds complete verified
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
semantics, and the loan retains the exact ordered designated field path down
to the reference leaf. Each step is checked against the concrete class fields.
The current field-aware contract permits any number of shared and exclusive
paths. Its entry loan retains each designated path and permission; at a call
boundary its result loan retains every independent caller source in `source`
plus typed `additionalDependencies`. Constructor and function summaries retain
the complete field-to-formal-origin map. These are checked dependencies, not fabricated
storage under a single merged owner. Loan conflicts, scalar-promotion admission,
and alias/escape analysis inspect the entire set. Shared reference/subview
projections conservatively preserve that complete set rather than independently
shortening one owner's endpoint.
When a result composes several single-path shared entry carriers, compact
source identity is decided per selected input, not by the result's field count.
Each mapping preserves its own opaque entry source and destination field path;
the verifier independently rejects substitution or collapse of those sources.
Two distinct result fields may intentionally name the same concrete owner.
Their field paths remain distinct in `additionalDependencies`; equality of the
source place is not permission to discard either path. Typed field projection
preserves its semantic overlap key so local publication can reconcile every
dependency. A cleanup-bound construction from one shared input retains that
input's ancestry while recording the full result map. Multi-input composition
does not invent a common parent. Independent mutations reject missing or
collapsed result paths and premature release of the guarded pair.
Returning the carrier or its exact stored referent publishes a fresh escaping
`Return` loan through one `Borrow` operand that names that entry loan. A carrier
return also names the exact SSA value loaded or moved from one of that loan's
carrier bindings. A reference return carries a binding-rooted runtime place
whose only projections are that designated field path and its dereference. The entry
loan ends at that transfer. The verifier independently rejects a projected or
wrong-field entry, a shell or unrelated same-typed carrier, an ordinary call-
result place, a reused or multiply produced return loan, a wrong payload or
referent type, or a terminator that does not carry the same loan and exact
runtime value/place.

Construction of a nested carrier consumes the parameter's entry dependency
through an explicit `Borrow` publication to the exact initialized field,
followed by `EndBorrow` for the incoming dependency. The field initializer's
ordered copy/move value remains the physical shell transfer. Verification
checks the constructor's parameter, initialized field, full reference path,
access mode, and value transfer together; neither the shell's address nor a
same-typed unrelated field substitutes for the external owner. A projected
receiver can inherit this dependency only along the verified reference-bearing
path. Backend projection spelling follows those typed fields and represents
the final reference dereference without reconstructing ownership.
For a cleanup-free, generated-copy read-only carrier parameter, native member
initialization may copy that shell only when the copy binding also proves its
exact entry-to-field `Stored` publication. The same proof admits the parameter's
representation slot; it does not turn the by-value shell into the referent.
After exact field-mapped publication, cleanup of a copied formal shell is also
distinct from destruction of its external referent. The verifier recognizes
this only through the entry's checked reference-field/dereference path; an
ordinary owned field does not receive the exemption, even when its type matches.

A nested shared-view value return binds the actual `Load` operand's field
prefix to the source entry dependency and publishes result-relative stored
paths. The verifier checks the runtime load independently of the summary;
changing only the summary, load prefix or result path cannot redirect a view.
At a call on a nested member, lowering composes the receiver's exact field
prefix with the callee's relative path before selecting a concrete stored
dependency. Different same-typed referents therefore remain distinct.

References to a carrier's owned scalar or owned allocation instead protect
its physical shell as well as any external stored dependency. That dependency
does not make an owned element survive moving, replacing or destroying its
carrier. Conversely, copying an external shared view does not freeze ordinary
disjoint shell fields. A cleanup-bound carrier may have received its dependency
through an exact whole-value initialization or move chain; the shared verifier
query proves that historical identity while the flow verifier still requires
the currently active owner and valid loan state. This is not an exemption for
use after movement or for ending a resource loan before cleanup.
A by-value cleanup-bound entry loan retains its formal parameter as an immutable
provenance anchor even after that parameter moves into locals. Current binding
availability is tracked separately. Every additional carrier must be reachable
from the formal's initially active drop obligation through exact lifecycle
transfers; an unrelated same-typed local or parameter cannot supply this proof.
The program verifier checks this history independently of the loan's carrier
summary, and the flow verifier still checks active ownership at each operation.
A newly constructed cleanup-bound owner can retain a copied passive view's
shared dependency as a distinct child loan. Its cleanup obligation does not
merge with that view's ordinary last-use endpoint. Mapped reference returns
through carrier aliases select the exact active stored dependency as parent;
the physical alias and the external referent are not interchangeable. Body
verification checks the selected operand/dependency ancestry and access, while
program verification independently substitutes its typed field path, including
projections beneath the stored reference. Lowering preserves the typed path
prefixes and publishes returned call-result loans before child-first retirement.
Finishing a derived view does not retire an external-owner loan still retained
by another live physical-shell alias. Child-first endpoint selection accounts
for both parent reborrows and stored dependencies; independent verification
rejects ending either kind of authority before its dependents.
Read-only structural range elements retain the exact iterator dereference
`CallResult` chain when it terminates at the semantic range loan, including
a range reached through a borrowed-success carrier and reference accessor.
They do not retire that chain and replace it with a `Local` loan against the
coarser shared range. The selected returned address and child-first lifetime
therefore survive through each iteration's last use, break, continue, return
and defined-failure cleanup. Mutable iteration retains its existing direct
exclusive parent/element contract; no shared access is widened.

Targetless generated move construction preserves the source carrier's complete
dependency set: origin places, permissions, ancestry and destination field
paths. Its reference-shaped special-member formal does not describe an ordinary
borrowing call. MIR explicitly transfers the already-moved SSA obligation into
the new result. Retiring the original lifecycle-bound loan requires that exact
source/result handoff and unchanged dependency set; a same-typed construction
alone is insufficient. Native reference members preserve their bindings rather
than moving the referents.

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

A by-value call that consumes a complete borrowed carrier retires the input
loan on the call-success continuation before initializing the returned carrier.
If HIR also records that loan's last-use endpoint on the enclosing statement,
lowering acknowledges the already-emitted endpoint exactly once for that
statement. It must not emit a second `EndBorrow` or ignore arbitrary inactive
loans. For a conditional statement, only HIR's exact declared enclosing endpoint
can be acknowledged by nested transfers. Each arm has independent endpoint
state; the join retains an acknowledgment only when every normally reaching
arm supplies the same identity. A terminating arm supplies no fallthrough
evidence. Loop and switch bodies cannot export an unproved path-only transfer
acknowledgment. The result still retains every source dependency; live child
reborrows and unrelated overlapping loans remain conflicts.

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

The checked-borrow Expected extension reuses that authority without adding a
native reference-valued sum. A successful `expected<T&, E>` return publishes a
fresh escaping `Return` loan from the exact referent loan, then retires the
body-local source at the same boundary. An error Return carries only the exact
`Unexpected<E>` value. Forwarding a complete carrier instead publishes its
ordinary Expected runtime value and uses the Return loan solely for owner
provenance; reconstructing success from the loan would corrupt an active error
alternative. A direct local success initialization consumes one exact Loan
operand whose place names the runtime `T` and whose loan names the protected
owner. Verification admits only the read-only specialization, exact types,
one carrier, non-widened access, and immediate source-loan retirement.
The success loan always uses read-only access, including when the enclosing
method has a mutable receiver. A by-value nominal carrier parameter can provide
that success through its one exact designated reference path; the Return
operand retains both the external owner loan and the separately verified
runtime field/dereference place, never the carrier shell's address.

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
masking an executable MIR schedule. A final concrete source pack retains one
grouped parameter place even when its function signature has several (or zero)
concrete elements. Bodyless verification checks the exact ordered element roles
against that signature and rejects symbolic, nested, or non-final pack places.
General body admission and failure-form
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
success edge activates. When an ordinary constructor consumes that result as
an enum field initializer, the same exact slot and success activation precede
the field's `Initialize`/`Reparent` into its construction-rollback obligation.
The callee failure edge leaves the field unpublished; a later initializer
failure drops the successfully published field. The backend uses this existing
schedule for constructed variants, moved parameters, and helper results,
requiring failure-free generated relocation rather than enum assignment.
Version 18 also added
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
additional place or loan use. Earlier prepared payloads survive later argument
evaluation, with their exact cleanup recorded on each later failure edge;
preparation does not imply immediate invocation. Fresh closure arguments likewise
reparent their value-root lifetime into prepared storage. Their unprojected root
may be lifetime metadata, but cannot create an additional executable alias or
replace the source-to-prepared ownership transition. Version 17 added bounded
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
  cleanup. The prepared owner remains active until the final invocation transfers
  it exactly once. Scheduled ordinary constructors use this same path:
  written, defaulted, and pack-expanded inputs retain prepared-parameter
  places across later argument evaluation and transfer at `Construct`, not at
  the input checkpoint. Failure while preparing a later argument drains earlier
  prepared owners in reverse order; constructor-body failure owns the handed-off
  parameters and partially initialized fields. Copy/move special-member
  construction retains its separate exact source-place path. Every checkpoint's
  single result has exactly one executable use by the matching `Call` or
  `Construct`, so the invocation consumes only prepared inputs rather than
  reevaluating a source operand. The sole zero-use exception is an interrupted
  invocation: every remaining normal CFG path must reach an explicit terminal
  call before the full-expression boundary. A bare unreachable edge, a cycle,
  or a later statement's terminal call is not evidence. Failure-edge loan and
  drop checks still apply independently; an abandoned prepared owner is never
  transferred to a nonexistent callee;
- expanded constructor arguments retain the concrete selected parameter type.
  An exact read-only reference parameter borrows the projected pack element
  instead of inheriting its source pack's by-value copy/move role. This is a
  call-input representation rule, not proof of complete constructor-pack
  or C++ constructor-field support; the remaining gaps are tracked in the
  active standard-library campaign;
- each mutable-reference element of a concrete formal pack has one semantic
  `Parameter` entry loan over its exact `PackElement` place. The carrier binding
  remains immutable, while the loan grants mutable access to the caller's
  referent. Forwarding stages the exact loan, not a borrow of the whole pack.
  Verification checks per-element coverage, access, non-escape and source
  identity; normal and failure exits retire the entry capabilities;
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
- one `MirOperation::FunctionItem` for each selected GTI function item. The
  result type carries the nominal `FunctionId` and exact signature, and
  `functionTarget` carries the concrete MIR function instance. It has no
  operands, place, ownership event, capture environment, or failure behavior.
  Verification requires the target to exist, have GTI linkage and a body, be
  namespace-owned rather than class-owned, and exactly match the nominal ID,
  result, and parameter rows. A missing target or forged identity/signature/
  owner is invalid before optimization or backend entry;
  passive value stages preserve the exact item type and need no prepared drop.
  A fresh item crossing an owned boundary retains its single-use producer and
  consumer checks. A namespace call cannot carry a method receiver. Any effects
  of producing the item are evaluated before the call's arguments in MIR;
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
  and optional identity chain has exactly one executable use, ending at the
  verified call receiver or forwarding operand. An owning move may additionally
  have one exact Value-rooted full-expression cleanup place with one matching
  obligation; that root is not another invocation. Pack-expanded calls use a
  `MoveValue` receiver stage for an owned moved lambda and retain that same
  obligation; the stage does not create a prepared parameter. A cleanup-free
  closure instead retains a plain value stage after its source-consuming Move.
  One move proof cannot authorize two consuming calls. Program verification
  checks the required capability against
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

Program verification also requires each function, constructor, destructor and
lambda body to retain its enclosing instance's exact body kind and return
type. Constructors and destructors return `void`. A well-formed body-local
return operand cannot authorize changing the callable's published result
signature, including either alternative of an Expected result. Backend
admission independently retains this identity check.

An implicit named return arrives as an ordinary HIR `Move`, so MIR emits the
same exact-place `Move` instruction and availability transition used for
explicit transfer. Return lowering then uses the shared pending-result
preparation, reverse cleanup, and publication path. MIR verification therefore
does not trust source spelling or add a second return-only ownership mechanism:
it requires the available-to-moved event, source obligation transfer, and one
final `TransferOut` publication already required for an explicit move.

Default expressions arrive as ordinary caller-side HIR values. MIR schedules
their `CallInput` checkpoints after written arguments, preserves the default
provenance bit, and verifies the trailing-suffix invariant against the complete
selected parameter list. Constructor initializers preserve their explicit
argument count alongside all expanded inputs. The production C++ emitter
therefore emits only verified MIR values and never emits a C++ default
parameter expression.

A valid HIR constant `sizeof`/`alignof` query lowers to an ordinary unsigned-64
`MirOperation::Literal` containing the retained frontend value; its source HIR
identity remains available as provenance. Runtime object representation queries
are a separate contract: trusted declaration-bound `ObjectSize` and
`ObjectAlignment` calls preserve one concrete `objectLayoutType` in HIR and MIR,
with no runtime operands, parameter types, borrow origins or lifecycle events.
Lowering retains otherwise-unused queried class instances. Program verification
and backend admission share the exact complete-type/zero-input proof, rejecting
query metadata on other instructions. These operations are pure, removable when
unused, and target-dependent; scalar constant interpretation remains overdefined.
The C++ representation snapshot retains the queried type, and emission uses that
verified representation's native size/alignment. It does not reinterpret the
constant-query rule, source AST, or public library names. The public descriptor
and generic forwarding function remain ordinary source-defined GTI.
The eligibility predicate is a trusted private structural-constraint identity;
the ordinary public concept selects it through normal concept composition.

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

Integral enums retain a program-level table of
their declaration identity, source unit, and exact underlying integer type.
The `EnumUnderlyingValue` call consumes one already-staged enum value and
produces exactly that integer type, with no failure, loan, or cleanup effect.
Verification and backend admission reject payload enums, missing identities,
inexact result types, and altered parameter shapes. Formatting uses this
general operation through source-defined library policy.

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
Their verified generated movement is failure-free, including when composed
inside another passive record. A fixed-array aggregate of staged copyable
passive records constructs its final storage directly in MIR operand order.
Its lexical result/binding tokens and exact publication remain mandatory, but
no active per-element cleanup is invented. Named inputs are copied when their
MIR Loads execute; native default construction is not used as SSA staging.

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

Scalar unique allocation likewise requires a local `allocation_failure` /
`unique_owner` detector and an `Invoke` before owner activation. Independent
verification checks its scalar pointee, zero/one exact initializer, result,
absence of constructor dispatch, and failure metadata. Named native callback
values are passive ordered by-value inputs, including expanded packs; an
ordinary callback parameter may also receive the already-admitted null value.
Native invocation uses the separate exact-signature call contract below; null
transport does not make invocation safe or admit foreign-thread entry.

Class unique allocation with a selected ordinary constructor evaluates and
prepares arguments first, then issues `AllocateObject`. Its local allocation
failure edge owns argument cleanup. Success arms one `ObjectAllocation`
obligation for unconstructed storage. The following allocation intrinsic call
retains the exact constructor target and normalized constructor-failure effect,
transferring prepared arguments only at constructor entry. Constructor failure
drains the raw allocation without invoking a pointee destructor; successful
construction activates the ordinary owner result and immediately transfers out
the raw-storage obligation. The independent protocol verifier rejects foreign
uses, loans, aliases, detectors, lifecycle events, and reordered publication;
ordinary lifecycle dataflow additionally verifies path-wise cleanup.

The C++ backend implements raw storage with matched aligned allocation and
deallocation and publishes a native owner only on success. Copied and moved
class inputs are materialized in MIR lifetime slots before allocation, not
deferred until constructor entry. Fallible construction retains the bounded
constructor rollback proof. Each field rollback executes at its MIR `Drop`,
then recreates only the proved observation-free native empty state. This keeps
field cleanup ahead of earlier-created constructor parameters; deferring field
cleanup to caller-side object destruction would reverse that order. The caller
then destroys only the unpublished representation. A source destructor remains
disarmed until constructor success. The proof of empty fields for body-ordered
parameter publication is distinct from permission to create an empty value of
the whole class: the latter additionally requires a complete cleanup recipe,
generated move assignment, recursively empty-representable fields, and explicit
guard disarming in the private tag constructor. Broader
rollback, targetless generated default/copy/move allocation, and other
terminal allocation capabilities remain separate migration work.

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
Explicit numeric, bool, char, plain-enum, and non-borrowing cleanup-free class
constructor-field initializers
carry an `Initialize` into the exact This-rooted field. The initializer value is an
ordinary instruction operand, so optimization sees its publication use.
Program verification requires exactly one matching stage and full-expression
boundary; constructor metadata cannot stand in for a missing write.
An owned synthetic temporary returned by a Move publishes at that Move, then
retires its disarmed native shell at the adjacent exact MIR `TransferOut`.
It does not acquire a second semantic drop; named locals retain their existing
moved-from cleanup schedule. This includes a propagated owning success payload
returned into an expected result after its source envelope has been consumed.

An explicit stage also suppresses metadata-derived publication from every
sibling MIR record of that source initializer. Increment and assignment
expressions may retain a discarded assignment result alongside the scalar
value actually consumed by `Initialize`; that bookkeeping record must not
introduce a second field write or an invented native temporary. Checked
arithmetic still precedes the source-place write and field publication, so
overflow leaves the source unchanged and rolls back only completed fields.

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
alternate source-tree authority. A prepared argument handed to an ordinary
call is not an unarmed subobject transfer. Its `PreparedParameter` obligation
remains caller-owned until invocation; the ordered-call verifier binds its
single `TransferOut` to that exact call. Constructors and field-initializer
bodies retain failure routing across these calls. The status-constructor
representation also admits the existing class-result argument staging slots:
if a later argument fails, their ordinary MIR drops run before completed-field
rollback, with the same secondary-failure edges as in function bodies.
Owned-parameter class fields stage the same way: the
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
initialization. The C++ spelling consumes these field drops in place and restores
empty native fields, rather than deferring their observable cleanup beyond the
MIR schedule.
This rollback needs empty-state construction, not assignment into a live
field. A source destructor therefore does not exclude an otherwise eligible
owner containing a construction-only field. Fresh owning arguments use the
same verified prepared-parameter slot and exact transfer into a selected
field constructor as named moved arguments. A constructor result can also
publish into an owned Expected binding: its Invoke success activates the
payload obligation, then the binding Initialize transfers it into the
wrapper. Generated lifecycle-guarded movement reuses MIR's failure-free
relocation proof; it does not execute the disarmed source destructor.
Constructor fields use that same typed publication protocol when an already
permitted success/error value initializes an Expected field. The source
payload is transferred out and the completed target wrapper receives its own
rollback obligation; the field must not inherit the payload's narrower drop
type. Verification checks the selected alternative, target cleanup recipe and
publication events independently. This does not admit new borrowed-success
conversions or defer conversion policy to native C++.
An owning Expected call result passed into a constructor retains its exact
value-rooted slot across subsequent argument evaluation. Its producer's Invoke
success dominates the consumer, and one shared owned-argument CFG proof checks
both prepared class arguments and these aggregate results: every normal path
reaches the sole consumer without touching the slot, while each intervening
failure path drops it exactly once or terminates on a second cleanup failure.
The exact storage root is not another value consumer merely because failure
cleanup references it. Parameter position and type, unconditional transfer,
and the constructor result's own publication remain independently checked.
Emission preserves the MIR argument order and only retires the moved-from
native shell at the consuming operation; it does not defer argument evaluation
into a C++ call expression.

The first base-subobject family now uses an entry `InitializeBase` followed by
an `Invoke`. It covers a selected zero-argument base constructor,
parameterless derived construction, cleanup-free derived fields, and generated
base/derived destruction. Both trivial-cleanup and owning bases carry the exact
base cleanup bit in their rollback descriptor. The bounded source-free
generated-derived HIR constructor lowers through the same body schedule; it is
not a native-only default constructor or a terminal containment adapter.
A base that fails is not armed in the derived body;
later derived failure drains the completed base last, while successful
construction transfers its obligation to the enclosing object. Independent
verification binds the exact base relation, constructor, place, rollback type,
entry position, activation and retirement, and forbids aliases or ordinary
value/borrow uses of the base place. Interprocedural effect analysis follows
the selected base constructor, so its allocation, memory, and other effects
remain visible in the derived constructor's summary and to the optimizer.

The C++ backend fuses this operation into the exact native base status overload
and branches on the retained result before executing derived MIR. Passive
field defaults include plain read-only enum values. For the selected
aggregate schedule, a fallible base cleanup runs at the explicit MIR `Drop`
and returns its secondary record before the caller tears down the empty native
representation. Other admitted base rollback finishes when the allocation
caller destroys the unpublished native object. That deferral is bounded to
constructors without owning parameters or derived fields; it cannot cross
their cleanup order. No base is destroyed and placement-reconstructed as an
ordinary field.

`tests/fixtures/inherited_constructor_cleanup_failure.gti` covers the original
loss of the derived constructor's primary failure when its base's unique-owned
leaf destructor also fails. The selected chain now retains both records for
`GTI-R0014`. The corresponding multiple-owner regression is
`tests/fixtures/inherited_multi_owner_cleanup_failure.gti`; it now uses the
complete aggregate schedule rather than deferred native cleanup. Generated
pointees may themselves contain several supported active fields. Collection
destruction now composes through the v61 element recipe, including the
vector-owning base in `inherited_collection_cleanup_failure.gti`. This still
does not establish cyclic ownership-type graphs, base arguments, owning derived
fields, declared base/derived destructors, or
static-field initializer bodies. Those remain M-FAIL-01 work.

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

Boundary mutual-exclusion reachability is scoped to one dynamic evaluation.
It does not cross a natural backedge whose header dominates the originating
marker: the next iteration may propagate an argument error before the outer
call/root executes. An inner loop that does not enclose that marker is not
ignored. Duplicate markers reachable within the same iteration still reject.

Concrete owning pack parameters use one entry-active Binding obligation per
lexical-drop element, each at its exact `PackElement` place. Those lifetime
places are canonical across expansions and folds. Program verification checks
exact element coverage, type/traits, entry activation, and source-order
construction ordinals; alternate owning projections without the matching
entry lifetime are invalid. A copied element initializes a separate prepared
parameter without consuming its source. Because the entry place is shared
across expressions, it has no individual HIR expression origin. Its exact
entry obligation supplies copy-source identity instead. A forwarded move uses
an adjacent `Move`/`CallInput` pair whose lifecycle event transfers the exact
element obligation into the prepared parameter. Normal and failure cleanup
drain only remaining active elements in reverse order, retaining the ordinary
primary/secondary defined-failure protocol. Direct constructor-pack owning
inputs use the ordinary prepared-parameter invocation contract when semantic
selection names an ordinary constructor body.

An exact one-element concrete pack selecting a generated copy/move constructor
uses the existing `Load(Copy)` or `Move` operation on its canonical
`PackElement`, rather than pretending the special member is an ordinary call.
The selected reference parameter and concrete element/result type must agree;
the element's retained HIR copy/move role remains authoritative. An owning move
transfers the entry element obligation into the result's value obligation;
copying independently initializes the result and leaves the pack available.
Cleanup-free results retain the normal no-value-drop convention, while the
moved-from cleanup-free parameter still has its harmless lexical lifetime.
The original construction expression supplies result provenance and the
full-expression boundary. No native constructor lookup or new escape rule is
introduced; borrowed-state pack elements remain outside the existing by-value
pack contract.

An active-cleanup nominal receiver's `Move` initializes its existing Value
obligation from the source binding. Its `CallInput(Receiver, MoveValue)` aliases
that value without a prepared parameter, destination, or lifecycle transfer.
The invocation cannot transfer out this obligation: the caller's full-expression
stack retains it across argument evaluation, the call, and later siblings.
Verification rejects missing lifetime provenance, duplicate executable uses,
missing activation, and a transfer at invocation. Ordinary argument inputs keep
their existing prepared-parameter transfer contract. The C++ backend constructs
one lifetime slot at the Move, invokes through that slot, and consumes the MIR
normal/failure drops; it must not delegate the lifetime to a native argument.

Ordinary consuming methods and value-producing operators use this same
invocation capability and exact selected function identity. Fresh owned
receivers stage `MutableTemporaryBorrow` from their full-expression slot with
`Once` capability. Verification binds the stage to its unique producer,
mutable owned storage, cleanup obligation, and single invocation use. A
conditional receiver instead names its initialized join temporary through a
transport-only Load/Move identity; initialization and lifecycle dataflow prove
the slot live at the call. The backend must not reconstruct that slot into
itself at the join. Fallible arm producers activate only on their Invoke
success edges. Program verification independently rejects whole-This moves
outside consuming instance methods and consuming calls that publish tracked
borrowed state.

`CallInput(Receiver, RetainedReceiver)` is the in-place forwarding counterpart.
It carries a writable, field-only This place and a semantic `ConsumeReceiver`
event (available to moved), but no Move instruction, prepared parameter,
destination, or lifecycle transfer. Availability is consumed independently of
physical cleanup: the enclosing receiver keeps its fields. Body verification
checks exact place/event identity and subsequent availability; program
verification requires an enclosing consuming method and a non-borrowing
nominal or exact stored-lambda callable target. A retained lambda's complete
closure type must match its selected lambda instance, not just its signature.
The staged capability has one exact invocation use.
The backend emits the selected receiver-qualified call against that retained
place, leaving outer full-expression cleanup unchanged.

`MirFunctionInstance::ownedPointeeProjection` describes a transparent source
accessor, not a consuming call. The bounded proof requires one direct declared
`UniqueOwner<T>` receiver field and the matching checked read/mutable borrow,
followed by its exact returned loan. The only permitted control-flow split is
the intrinsic's empty-owner failure edge. Calls to other functions, writes,
alternate return origins, extra effects, and virtual dispatch do not satisfy
this proof. The verifier checks the concrete owner field/type, access, intrinsic
operand lineage, returned loan, failure path, and full-expression boundary;
ordinary receiver-derived borrow metadata is insufficient. This is the
accessor-side proof for retained pointee consumption.

For that caller, a `RetainedReceiver` stage has `retainedOwnerPlace` naming the
mutable field-only This owner anchor, while its executable operand is the
accessor's actual unprojected mutable CallResult loan. The semantic ownership
event consumes the anchor, not the borrowed referent. Body verification checks
anchor/loan identity and preserves every other live-loan conflict. Program
verification traces the loan to the exact independently verified accessor,
checks its receiver against the anchor, and requires a single consuming
invocation use with no escape, reborrow, or additional executable loan use.
There is no synthetic caller access to the wrapper's private owner storage,
and no cleanup transfer: the enclosing receiver still owns destruction at
the caller's full-expression boundary.

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
recursively cleanup-owning captured values. For captures with complete cleanup
recipes, the obligation retains their exact reverse-order aggregate schedule,
including nested class/owner/collection cleanup. Destruction failure therefore
belongs to the enclosing MIR cleanup edge, not native closure teardown. Class
lifecycle metadata contains every lexically dropped field in
declaration order; its independently retained drop projection must be the exact
reverse order. Trivial fields remain structural HIR/layout facts rather than
lifecycle-verifier authority.

Expected and Unexpected obligations retain positional alternative cleanup
recipes: `[success, error]` and `[error]`, respectively. Passive alternatives,
including `void`, retain typed inactive entries; they are not omitted or
confused with the other alternative. Lowering and whole-program verification
derive these recipes independently from the concrete types. Failure effects
include potentially active alternative destructors, while execution cleans
only the actual active alternative. Secondary cleanup failure preserves the
original failure record. A `void` success never requires payload access.

Error mapping uses the existing branch, state-proven extraction, Move,
ordinary function Call/Invoke, Unexpected, and Initialize vocabulary. Its
Expected-valued join has one temporary storage identity, not a default-active
payload followed by assignment. The C++ backend uses uninitialized lifetime
storage for that join and for non-default-constructible Expected call results.
Passive class payloads still require representation storage, even without a
semantic destructor; these slots retire only the native representation.
Owning slots follow MIR's transfer and cleanup events, including mapper failure
and pending return rollback. No AST/HIR expression walker executes the mapping.
An owning call result may also publish through `Unexpected` into an ordinary
named Expected binding. The payload, wrapper, and binding retain distinct drop
identities: the checked call activates only on success, wrapping transfers its
payload once, and the exact Initialize transfers the wrapper into the binding.
Later failure cleans the binding; it does not replay or delay the producing
call. Direct Expected moves use the same return-publication proof whether the
owned class appears in the success or error alternative.
Storage-free error values use that same binding-publication model without an
invented wrapper lifetime: `Unexpected` is spelled at its sole `Initialize`
consumer. The binding has uninitialized native storage even when the error is
scalar, so an inactive owning success alternative is never default-constructed.
Trivial bindings retire only their native representation and rearm at the next
loop iteration; owning bindings retain their exact MIR activation and drop.
When an owning aggregate call result is consumed directly by a constructor,
its exact success-activated value obligation is transferred into that
construction before the constructed object is prepared for a later call.
The staging proof accepts this direct value route as well as an explicit
prepared-parameter shell, requiring the same unique consumer, type/place
identity, success activation, and single transfer. It does not defer the
argument call or skip either constructor's failure cleanup.

A concrete fixed-array obligation retains one aggregate schedule rather than
inventing a backend-visible drop for every element. Its extent sequence and
leaf cleanup identity must exactly match the array type. The aggregate `Drop`
still occupies one lifecycle position, while execution visits every leaf in
reverse index order and routes a failure-capable leaf destructor through the
ordinary MIR failure edge. This representation scales to nested fixed arrays
without making the backend rediscover element order or destructor identity.
When elements have an exact recursive cleanup recipe, the array also retains
one immediate-element recipe, recursively for each dimension. This includes
non-failing active cleanup, such as shared-owner release: the backend executes
the verified reverse-order recipe before retiring the inert native array
shell. Arrays embedded in allocated objects use the same recipe. Native array
destruction is not a substitute for releasing checked ownership resources.

## Controlled Optimization Edits

The compiler-owned optimizer vocabulary gate is described in
[Optimization](optimization.md#language-growth-conformance-gate). Body
verification runs its structural admission check before CFG, ownership, and
lifecycle reasoning, including for hosted startup. Unknown instruction,
operation, intrinsic, terminator, projection, operand, or failure vocabulary
cannot become a harmless fall-through. The same gate is used by optimizer
analysis entry points and independent source replay; effects remain owned by
`mir_effects`, not by the coverage catalog.

`mir_editor.h` exposes a deliberately narrow `MirEditor`.
Core MIR identifies bodies by `MirBodyAddress`, a GTI body kind plus owning
instance ID; instructions use stable `MirInstructionId`, and blocks use
`MirBlockId`. The editor never exposes mutable program vectors or retains
instruction pointers across an edit.

Current client-backed edits replace a computation with a local-fold or SCCP
literal, replace a branch terminator, promote one scalar place, attach one
safety discharge, refine one independently proved exact receiver call,
tombstone one dead computation, or remove one unreachable block. Exact
receiver edits preserve IDs, operand evaluation, invoke edges, failure origins,
and cleanup while refining the receiver place/type and selected static target;
canonical replay checks the complete evidence record. All queued claims are
checked before mutation and applied to a copy in
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

Borrow-source canonicalization preserves a projected call-result referent.
The dependency origin of a call is not necessarily the address it returns:
`unique_ptr<Left>::operator->`, for example, depends on the owner but returns a
`Left` reference. A second call through `root->next` therefore retains the
loan-rooted `Left::next` place, with the first call-result loan as its parent;
it must not compose `Left::next` onto the `unique_ptr<Left>` binding. Program
verification independently requires this exact projected source rather than
accepting equality of flattened dependency keys. This repairs nested checked
arrow chains without backend inference or a public-wrapper name rule.

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

A local read-only alias can retain that same parameter authority while its
runtime address denotes an accessor's subobject rather than the parameter
itself. Such a return also carries the exact dereferenced alias place in the
`Loan` operand. Verification follows unique, dominating reference
initializations back to the same parameter dereference or a verified
call-result address, preserving type, access and stored dependencies throughout.
Without an explicit runtime place, only the exact parameter referent type is
valid. Carrier membership or a same-typed unrelated alias cannot substitute
for this executable address evidence.

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
borrow capability; an explicitly moved receiver uses a `MoveValue` receiver
checkpoint and ownership transfer, while a fresh consuming receiver uses
`MutableTemporaryBorrow` with the exact once-callable capability. This
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
mutable materialization place produced by exact construction, a GTI function
or lambda call, or owned Expected extraction. A linear value uses its
value-rooted place; a branch-only value uses
the one hidden temporary slot already required to carry its conditional drop
through a CFG merge. The shared MIR materialization query accepts only those
two exact forms and resolves one producer/source identity for verification and
backend consumption. The producer strictly precedes the input; the input
strictly precedes all arguments and invocation. Exactly one matching value-drop
obligation is activated by successful production and remains attached to the
enclosing full expression. Verification also requires the selected non-static
GTI target (non-virtual for reusable receivers), concrete owner type, receiver
qualifier, result
non-escape traits, producer type, place type/access/source identity, and drop
descriptor to agree. This is an explicit MIR borrow schedule, not a native
rvalue convention.

An admitted single-owner borrowed carrier reuses this materialization and drop
schedule. `findMirTemporaryCarrierDependency` binds the exact producer/value,
one nonescaping `Stored` loan, stable owner place, and full-expression value
obligation. A separate `CallArgument` loan over the temporary's physical shell
names that loan through `storedDependency`, not `parent`: the shell is not a
subplace of the external owner, and mutable shell access cannot upgrade the
external dependency. The receiver input records this physical loan and ends it
after invocation or before failure cleanup. The stored dependency and required
ancestors remain live until the carrier's drop. Lifecycle verification rejects
early dependency retirement or transfer of the ephemeral receiver obligation;
ordinary returned/stored carriers keep their independently verified transfer
rules. Program verification checks the exact ordinary producer and selected
non-borrowing method, and the backend consumes this same verified schedule.

Calls inside conditional and short-circuit arms retain these dependencies
through the arm's existing conditional value drop. Only dependencies proven
against that drop, and their required ancestors, may have joined loan-state
sets at the CFG merge. An executable loan use still requires definite active
authority, and conflicts treat possibly live dependencies conservatively.
Direct child-borrow creation also rejects joined conditional states instead
of turning uncertain parent authority into an active child. Two-phase
reservations remain distinct from active siblings: a reservation does not
prevent a completed read sibling from reactivating their parent.
Conditional retirement is permitted only after independent lifecycle flow has
proved the carrier obligation inactive. This does not admit arbitrary
conditionally initialized references or add runtime lifetime tracking.

If construction fails after publishing a reference field, its stored loan
remains live through the partial object's rollback. Lowering inventories those
published loans separately from ordinary scope loans and retires them only
after armed subobject obligations are inactive. Independent verification rejects
early retirement, missing failure retirement, and retirement on the successful
constructor path, which must export the dependency to its caller.

A read-only operator on an eligible single-owner borrowed-class prvalue uses
the same explicit `ReadTemporaryBorrow` schedule as an ordinary method. Other
already-supported read-only borrowed-class operator observations retain their
ordinary ordered `Value` receiver stage. Its source has an explicit full-expression
materialization/lifetime obligation and retains its separately verified stored
dependency loan. The stage aliases the source result without copying, moving,
or transferring that obligation. Native emission requires the exact source
value, stage site/role/type, unique result slot, activation, and scheduled drop;
it does not reclassify this carrier as a fresh owning temporary.

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

An ordinary-call or constructor argument may use `MoveBorrowedTemporary` only
for the corresponding HIR input: one exact fresh GTI call result of the same
concrete class type as the selected by-value parameter. Lowering assigns the
successful producer result its one value-drop obligation, emits the move checkpoint, and
reparents that obligation to the prepared parameter before invocation.
Verification requires the producer, parameter, concrete selected instance,
input order, full-expression, and success/failure cleanup edges to agree. It
also rejects a copy, duplicate transfer, abandoned producer drop, or second
destruction. The owner's stored loan survives the shell transfer until its
existing full-expression endpoint. Other supported constructed/copied borrowed
carriers use the ordinary exact copy/move input contracts; they do not acquire
permission to escape or shorten an owner loan.
Borrowed lambdas retain their separate confined-callable lowering; this input
rule covers nominal class/struct carriers, not arbitrary borrowed values.

This schedule admits a completed mutation before a later overlapping borrow:
the mutation and its success edge precede the borrow producer, that producer
precedes its indexed input, and all inputs precede invocation. Existing loans
still conflict with mutation. The reverse written order remains invalid, as do
simultaneous exclusive aliases and later reads under an earlier exclusive loan.

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
unsupported concurrent GTI adapter before a backend sees it. The exact
C-thread entry additionally retains `CThreadCallbackKind`: `ThreadEntry` for
`int32_t(void*)`, `OnceInitializer` for `void()`, and `None` outside the concurrent
profile. HIR/MIR reject missing, swapped, or forged roles. The
backend consumes the role to enter/leave the exact registered runtime callback around
the verified target invocation, retaining the existing terminating failure
firewall. Callback thunk text
and calling-convention spelling remain backend representation choices; MIR
owns the executable target and containment decision.

An indirect native `Call` instead carries `nativeCalleeType` and a staged
receiver value. It has no function, lambda, constructor, body, or adapter
target. `isMirNativeIndirectCall` checks the exact admitted signature, result,
argument roles/types, receiver `CallInput`, lexical `ForeignPointerCall`, and
absence of a GTI failure channel. The ordinary ordered-input verifier checks
callee-before-arguments scheduling and single invocation, while generic
callable verification retains the concrete parameter boundary. Empty concrete
semantic failure facts shadow deferred generic facts rather than falling back
to an inapplicable callable failure channel. Foreign effects remain
conservative: no GTI body-effect summary is inferred from an arbitrary address.
Production emission reads this verified signature and staged value, reusing
the ordinary native ABI operand bridge; it does not resolve a source name or
consult AST/HIR to discover a target.

MIR v57 retains `CThreadRuntimeOperation` on exact private native declarations.
Create/join stay ordinary native `Call` instructions with explicit
`ThreadSpawn`/`ThreadJoin` synchronization, not managed-task intrinsics. Verified
hosted plans include exact `BeginCThreadRuntime`/`EndCThreadRuntime` native calls
after module initialization and after normal entry cleanup, respectively.
Verification checks declaration signatures, synchronization identity, callback
policy, and the complete hosted ordering. The C++ backend admits these rows as
part of the hosted schedule; it does not discover lifecycle from public wrapper
names. The detached declaration/adapter contract is serialized as
`lowered-program-v9`.

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
conversion must carry no stale origin, site, or record. Float conversions remain
outside this bounded integer verifier family. Physical-place integer compound
assignment and increment/decrement lower to an ordered
`Load`/`Compute`/`Compute(Convert)`/`Assign` schedule, omitting the conversion
when the arithmetic and destination domains agree. Semantics records the
promoted arithmetic domain in `ExpressionInfo::updateArithmeticType` and
authors separate arithmetic and narrowing failure origins at the source
operator. HIR retains those facts; MIR never splits or invents an origin.
The prior destination is read before the right operand, and the only write
occurs after both checks succeed. Postfix retains the original narrow value;
prefix and compound assignment retain the committed narrow result. Bare
instance-field destinations and explicit receiver projections use the same
primitive schedule. The general C++ MIR emitter consumes every
physical-place operation in this family. A non-array indexed class projection
still denotes a selected subscript call, not a physical element address; it
retains the checked closed compound boundary until one exact returned
reference can be staged. Splitting that projection into read/write would
duplicate the accessor or require an unselected overload. The emitter admits
each operation in this fixed-integer allowlist only after both MIR verification
and representation preflight.

The bounded `Invoke` family accepts a trivial scalar result, an eligible
cleanup-free nominal call result, or the exact cleanup-owning result obligation
of an eligible ordinary call. A nominal call needs its failure edge in nested
argument positions as well as at a full-expression root, even without a result
drop identity: the consumer's prepared slot is initialized only after the
producer succeeds. Calls returning `expected` with a nominal
success or error alternative also retain their required failure edge, even
when neither alternative has active cleanup. A fresh class constructor used as
a prepared by-value argument has the same requirement, including when the class has no active
cleanup. Fixed source expressions before a pack expansion use the same
evaluation context as ordinary arguments. The verifier independently derives
that context from the exact indexed `CallInput`; earlier prepared owners remain
live until invocation and are unwound if construction fails. Other constructor
destinations retain their own publication contracts. Scalar values
remain usable only from the normal successor. An owning result stays
uninitialized on the failure edge and is initialized by an explicit lifecycle
event only on the normal edge. A value-rooted producer slot cannot claim to
be initially available independently of its drop activation; the verifier
rejects that contradictory lifetime as well as failure cleanup or use that
assumes otherwise. For a class result, production C++ emission additionally
proves that the result has one exact consuming `Initialize` into matching
uninitialized lifetime storage, or uses a dedicated discard slot. A cleanup-free,
non-borrowing class result may also replace a bare mutable binding through the
existing uninitialized SSA lifetime slot. Its exact single-use assignment must
immediately follow the call or begin its `Invoke` success block; generated
move assignment publishes only after success and immediately ends the moved-from
representation shell so loop re-entry can reuse the stage. This is a structural
rule for ordinary classes, including fixed-array fields, not a container-specific
exception; owning or borrowed results retain their separate MIR lifecycle and
loan protocols. The callee
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
their own owning parameter or result state, borrowed results, other constructor destinations,
assignments, and class-valued normal-result merging remain separate work. The
implemented scalar block-parameter vocabulary does not change owning result
publication.

A reference return may form its outgoing loan before local cleanup finishes.
If that cleanup fails, no reference is published: its failure edge ends the
pending return loan before draining the remaining locals. Ordinary successful
cleanup leaves the loan available to the Return terminator. Loan dataflow
verification rejects an abandoned return loan that survives a failure exit.

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
