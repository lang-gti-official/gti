# MIR

Status: implemented structural CFG, scalar block parameters, verified
per-instance effects, ownership, synchronization, lifecycle, defined-failure
control flow, and sole production executable-body authority.

MIR lowers each concrete HIR body into body-local control flow, values, places,
resolved calls, ownership operations, and cleanup. Verified optimized MIR is
the executable component of the backend-neutral `LoweredProgram`; `MirProgram`
alone is not the complete backend API.

## Projected references and existing-value allocation

A local reference initialized from a projected address retains that exact
runtime address independently of its protected owner loan. In particular a
shared range element can protect its enclosing range while naming one selected
element; subsequent fields and selected operators project from the reference
carrier, not from the coarser range origin. The lowering query preserves this
distinction without changing access permissions, active-loan rules or endpoints.

Unique allocation of an already-built class value uses the selected generated
copy/move policy. HIR stages a generated move as a consuming same-type value,
not as a callable shared-reference formal. MIR reuses the separate checked
`AllocateObject`, construction and raw-storage retirement protocol: exhaustion
precedes transfer into allocated storage and aborts without cleanup; a later
constructor failure drains the unpublished allocation and the applicable
still-owned inputs and caller cleanup. The private generated-construction
shape query is shared by lowering, verification and backend admission; program verification also
checks the exact class's generated special-member policy. No public factory
name determines language meaning or bypasses deleted/private constructors.

## Representation

Bodyless generated default construction uses an explicit ordinary `Construct`
with zero inputs and no constructor target. Whole-program verification requires
the unique concrete owner's generated default policy and absence of an
initializer schedule; ordinary materialization, publication and cleanup remain
mandatory. Temporary receiver/argument proofs and effect analysis consume that
same construction operation. A targetless ordinary `Call` cannot stand in for
construction; native/deferred calls retain their separate typed targets or
invocation capabilities.

The existing one-input passive-union family is proved separately: the exact concrete union,
sealed overlapping layout and first declared field type must match the input.
That proof grants no ordinary call target, default-constructor policy, cleanup,
borrow, failure or precise effect authority.

A passive scalar value return preserves its source operand type and declared
body return type. The MIR-owned scalar-return query proves an exact type or
uses the existing shared context-free initialization-conversion rule; it does
not repeat assignability or decide literal-dependent narrowing. Semantic
contextual typing must already give a narrowed integer literal its exact type.
Scalar results use value/constant/copy/move carriers. A copy/move's claimed
source type must equal its exact selected place type before choosing either an
exact return or an initialization conversion; ordinary operand and availability
checks remain mandatory. Address or loan carriers cannot stand in
for a by-value scalar. The query applies to the destination type: a reference
return can intentionally carry a referent-typed address operand and retains its
independent return-loan authority. Aggregate, enum, text, callable, raw and
ownership return families receive no new authority from this scalar rule.

Raw address conversion uses `Compute/Convert` with one evaluated pointer value.
The independent `rawPointerConversion` query is shared by body verification and
production backend admission: only typed/void or same-pointee conversion is
permitted, pointee constness cannot drop, and no loan, raw-memory access,
ownership transition or failure metadata can be attached. Semantic pointer
conversion intrinsics lower away before MIR. Existing numeric `Convert`
contracts are not relaxed. The current backend keeps a native typed pointer
slot inside its private raw-address carrier and copies its representation for
inert transport/conversion; it does not make an integer round trip the storage
authority. Conversion of an expired address stays inert and does not establish
permission to access it.

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
bounded caller-tracked terminal outcomes at caller anchors: explicit panic,
inactive success access, and inactive error access. A caller anchor does not
invent a local detector or make a failure-free wrapper fallible. Inline and
cold record publication both select that same verified caller token; the
detected category and detail remain those of the exact intrinsic.

The trusted `Panic` intrinsic reads one text view and is terminal, caller-tracked,
and a conservative optimizer barrier. At detection it selects the caller token
and snapshots the message into the version-two runtime record before cleanup.
The local detector origin remains independently verified. Calls, joins, and
cleanup propagate that record unchanged. No public `std::panic` spelling,
ambient thread-local caller state, native unwinding, or backend-inferred control
flow supplies authority. Deterministic MIR serialization records the declaration,
instruction, body, and caller-assignment facts (`mir-v113`, `mir-body-v91`).

Caller-token consumers are panic and the identity-bound inactive-result
failure operations used by source result classes. This does not change
`source_location::current()` to inspect an incoming dynamic token, extend
tracking through native function pointers, or implement a call stack.

### Constant-execution foundation

Immediate invocation results extend the existing constant-substitution inventory
with passive aggregate constants. A `Compute/Literal` stage marked as a
constant substitution has no scalar `literal` payload for an aggregate: its
unique HIR value identity selects the exact inventory value. It has no operands
or call/constructor target. Verification checks the publication schedule and
validates aggregate fields, element types, active sum alternatives and passive
lifetime against the complete program catalog. Native emission materializes the
proved value using target layout; scalar arrays remain SSA values, while records
and record-containing arrays use their ordinary object storage representation.
The C++ backend fails closed for aggregate body-constant publication.

Statement-level immediate completion leaves no MIR runtime call: HIR retains
the source-bound proof for a successful standalone `consteval void` statement
and lowers only the completed statement. A bare return selecting an immediate
constructor enters the same constant-substitution inventory as other passive
aggregate results. Its literal is bound to the return full expression; MIR
verification checks the typed constant, and LLVM materializes the proved value.


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
active cleanup uses the transient lifetime profile below. Source constructors with explicit semantic
constexpr eligibility execute the same MIR body and produce one fully initialized
receiver. Each field must have one explicit, non-reference initialization stage;
trivial fixed-array fields now retain their exact `Initialize` write as well as
the constructor-stage boundary. Failure-effect proofs require that exact write
for both passive scalar and fixed-array fields; computing an aggregate value
without publishing it into the field is not proof of a completed constructor.
Declaration defaults and nested source construction expressions lower to this
same selected schedule; generated default constructors execute it too. Before execution,
the profile checks that every explicit field has its exact MIR initialization
write; a declaration-only initializer must not become a spurious
uninitialized-constant failure. Read-only and mutable constexpr
methods use an exactly typed receiver through ordinary `This` projections,
including nested fields, arrays, and further method calls. Only constructor
frames may initialize receiver fields. After those writes, a constructor body
or mutable method may edit a mutable field under the ordinary loan rules.
Materialized result receivers read the exact MIR `Value` root; their passive
storage is retired at the represented move/drop, not reconstructed as a binding
or kept alive independently of full-expression cleanup. A temporary produced
only in a conditional arm or a `&&`/`||` operand lives instead in its
CFG-promoted hidden `Temporary` slot. Its producer, as the shared
materialization query resolves it, publishes the result into that slot when it
completes, whether MIR retargeted the producer there or arms the slot's
obligation on its `Invoke` success edge. Argument and receiver borrows of the
slot therefore read the produced value, and the post-merge drop retires only a
slot that its operand produced.
Consuming method receiver preparation also designates that same materialized
root. A move-value receiver `CallInput` does not introduce a second owning
constant snapshot: field moves in the callee retire the caller's exact selected
paths, and subsequent receiver-shell cleanup skips those moved fields. Ordinary
by-value argument transport remains independent.
Entry string-view dependencies are backed by immutable constant bytes.
Checked read-only and mutable reference parameters, automatic bindings, and
returned projections use verified MIR loans. An executor-local location retains
the live frame/root cell, resolved field/index path, referent type, maximum
access, and storage epoch. Indices are captured at formation, not recomputed
after a call. Root, reference, receiver and field permissions cannot be elevated
by an expression's read-access annotation. Ordered borrowed call inputs preserve
the location through SSA staging; mutable callee writes update the caller's
cell rather than an argument snapshot. Internal reference results must retain
a live enclosing-frame origin. Borrow activation and `EndBorrow` retain their
ordinary reservation/endpoint roles; movement, initialization and drop retire
the relevant storage generation. No target address or persistent reference
identity enters `ConstantValue`. Source reference-return validation and ordinary
MIR verification still prevent local escape and conflicting loans.

Immutable aggregate storage is replaced on writes, so independent copies cannot
become aliases. Location path traversal/copying and backing aggregate updates
consume the shared budget. A `void` call completes as a unit result without
manufacturing a scalar constant. At the external service boundary a reference
result is its observed value and a mutable input is isolated executor storage.
Only a leading checked reference-binding dereference is transparent; arbitrary
pointer projections, stored-reference carriers and runtime calls remain
ineligible. Untaken runtime-only
calls do not execute. Rewritten/tombstoned instructions and optimizer-produced
control flow cannot determine constant eligibility.

Ordinary payload-free enums use the semantic enum catalogue's nominal identity
and exact underlying integer domain. `EnumConstant`, exact-type comparisons and
enumerator switch labels execute here, including enum locals, record fields and
array elements in mutable builders. Value reads of resolved enumerators lower
to constants; explicit place formation and source borrowing rules are unchanged.
A constructor whose text field publishes through the constructor record,
without an explicit `Initialize`, still executes in this profile: the field
takes its initializer's exact SSA value when the body completes, and the object
keeps declaration field order. The compatibility walker is required only when
an executed body holds a payload-enum place or value; a type that merely names
a payload enum, such as a reflected field metaobject, does not force the MIR
profile.
Payload enums retain their exact concrete type, active variant and ordered
active fields in immutable constant aggregates. Existing `PayloadConstruct`,
`PayloadExtract` and switch MIR execute their ordinary verified selection rules;
there is no payload-enum AST interpreter. Constant argument checking validates
the exact variant and every active field recursively. Checked observations use
executor-local locations including the variant and field index, so an inactive
or moved field cannot be read. Consuming receivers own their incoming snapshot;
active enum cleanup follows the canonical selected-variant recipe, including
transient exclusive owners. Neither stored references nor live allocations may
persist in constants. These operations share the existing work and lifetime
limits and cannot fall back to AST execution.

Constructor reference inputs use the same location transport as function
inputs, preserving source/access/epoch and writes through mutable arguments.
Generated defaults are structurally eligible, not implicitly source-marked
constexpr. The selected initializer inventory and each exact field write are
still verified; stored borrows and unrepresented in-place
constructor targets are not silently executed.

Transient exclusive allocation uses the ordinary `AllocateObject` followed by
selected construction, or the scalar allocation intrinsic. The request-local
heap assigns monotonic typed identities, not target addresses. Pointee loans
retain allocation identity, access, projection and generation; reset/replacement
cannot leave a live checked reference to freed storage. Projected movement
retires the exact path while preserving the surrounding object shell. Result
extraction retains its payload origin so propagation neither drops a moved
payload twice nor hides it in a copied SSA value.

Initialize/Move/Reparent/TransferOut/Drop lifecycle events drive active cleanup.
`MirDropType` supplies exact destructor identity and ordered subobject recipes;
execution does not rediscover cleanup from C++ representations or public names.
Source destructors execute their verified body, then the remaining subobjects.
An executed failure ends the constant request immediately and retains its body,
instruction and source identity; no resulting constant or heap is published.

Concrete pack roots remain grouped binding cells, while concrete call signatures
are expanded. The executor maps declaration bindings to those exact elements,
including empty packs, without changing MIR's call-input evaluation order.
Transient owner nodes are accepted only inside this executor. Inputs and final
results are recursively checked to reject imported or escaping owner identities;
active-cleanup result types and leaked live allocations are also rejected.
Allocation identities and live logical heap size have separate deterministic
bounds in addition to shared execution/depth limits. These profiles do not
extend the AST compatibility execution path.

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

Instance-field initializer inventory includes declaration-only `Initialize`
rows for fields without a resolved declaration initializer. The source/HIR
bridge binds each such row to the exact root variable statement, field binding,
SymbolId and concrete type, and proves both field and statement initializer
absence. The shared MIR shape predicate recognizes only this exact instance
body/field identity with no value, operands, constructor stage, failure or
lifecycle effect. It does not authorize erasing a real initializer. These rows
declare storage, not an implicit value or reference loan: the constructor's
separately selected `This`-field writes and their publication remain required.
No corresponding exemption applies to locals, module/static fields or actual
constructor stages. Conservative inventory retention need not imply execution
of a declaration-only row and must not be weakened to avoid representing it.

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
global/static validity. Complete-check lowering can include unused,
non-generic standard-library method bodies; artifact lowering skips unreached
compiler-trusted declarations. When an included compiler-trusted
body contains an exact private thread or atomic intrinsic, lowering marks
that body as a dormant synchronization wrapper. MIR verification exact-checks
the marker, rejects it on module/hosted bodies or unrelated synchronization,
and otherwise rejects represented synchronization in the single-threaded
profile. Thus unused `<std/thread>` and `<std/atomic>` imports remain valid
without granting a backend or transform executable concurrent authority.

The combined deterministic serialization is currently `mir-v119`/`mir-body-v93`,
with `lowered-program-v29` declaration metadata. These distinct combined
identities retain both macro result-region/local-initializer authority and
thread-local initialization, cleanup and reference-pack proof facts.
Compiled scalar linkage records also seal each callable's result type and
compilation-local source unit alongside its persistent declaration key. Imports
bind against their exact virtual facade unit; exported overloads bind against
the selected root module. Scalar wrappers retain the GTI failure channel and
forward a fallible implementation's original result, status and failure record.
When the existing MIR cleanup analysis proves a compiled producer body only
propagates failure, its LLVM code forwards the caller's failure destination and
defers the inherited-cleanup check to the importing GTI call site. It performs
no observable cleanup before that check. Bodies with observable cleanup or
managed-worker containment retain their runtime checks; the self-contained
object profile continues to reject unresolved runtime dependencies.
The producer's cold failure-origin helper uses guaranteed inline memory
operations to initialize the existing fixed-size record without a libc call;
exact failure-channel self-copies erase before native code generation.
Class records also preserve semantic-selected virtual root/target declarations.
HIR binds each selection to exact concrete function and owner identities plus
successive direct-base paths; MIR verifies complete root coverage, signatures,
final target coherence and paths independently. Abstract classes may retain pure
targets. Lowered declaration and instance records seal and serialize the same
selection. These are backend-neutral facts; this metadata alone does not admit
LLVM virtual execution or prescribe a table layout or physical call ABI.
Represented polymorphic constructor stages may also carry an exact active-class
plan anchored after successful `InitializeBase`, or at entry when no state-bearing
base exists. Verification checks the selected instruction and success dominance;
optimization preserves that identity. Absence remains valid core MIR for existing
base-call families whose staging is incomplete, and requires a native admission
gate rather than an invented entry transition.
Every source destructor of a polymorphic class records the lifecycle-dispatch
transition to its own class (`MirDestructorDispatchState`, `mir-v117`). From
the destructor's entry, through its body and its own fields' cleanup, the
object dispatches as that class. The owner decided this rule on 2026-10-27;
see [backend lifecycle dispatch](backend.md#lifecycle-dispatch). Verification
requires the record exactly for those destructors.
Declaration-bound computed
type projections retain logical callable results independently of native ABI
result channels. The combined schema retains
complete borrowed-dependency-set proofs alongside foreign-storage inventories
and place identity. Loan selection records distinguish an evaluated stored
reference address (exact extraction value and carrier) from the external places
that may own its referent. Consuming selections also identify the exact old
dependency group in `transferredFrom`; this is a handoff claim, not a live
reborrow parent. Body verification requires an existing, distinct group only
for a transfer-mode producer. Program verification must separately prove the
closed field-transfer prefix and preserve external ancestry after the old
carrier retires. The records alone grant no borrowing authority: independent
complete-value flow reconstructs every selected external origin, access mode and
active-alternative guard. A passive carrier with several reference fields keeps
that complete fact only across its exact sealed extraction prefix, until the
last field is transferred; ordinary whole-value moves still invalidate it.
The declaration format also retains exact external storage
names. Earlier versions record
checked borrowing capture modes, each concrete environment's invocation
capability, per-dependency loan parents, and exact stateless GTI function-pointer
targets. Reference captures retain ordinary checked-reference storage and
carrier origins; mutable captures make the environment non-copyable and require
exclusive invocation. Composed carriers retain every dependency and ancestor,
including dependencies transported through generic parameters and fields.
No native address or public library spelling replaces those checked facts.

A cleanup-owning borrowed result uses the same success-only call publication
protocol whether its source type is a class or payload enum. A failure-capable
producer initializes its result obligation only on the Invoke success edge;
the failed call publishes neither the result nor its dependent reference.
Borrow metadata does not turn an owning result into a reference-only call or
permit unconditional result initialization. MIR independently rejects missing
failure edges and malformed result activation.

Function rows retain optional declaration-relative complete result dependency
contracts. MIR independently validates
each declared alternative's exact concrete type, tag and dependency-free
payload layout, then reconstructs facts at every reachable normal return. The
actual alternatives must be covered by the declaration contract; additional
well-typed dependency-free cases are conservative, so optimizer narrowing does
not change caller lifetime obligations. Unknown return facts and a function
with no normal return cannot prove this contract. A program-local memoized
query treats recursive/in-progress callees as unknown; declarations cannot
justify one another cyclically. Exact direct calls use only independently
verified target contracts, and failure successors receive no result fact.
Value-annotation and return-contract checks share the same producer/storage
dataflow reconstruction. These facts neither establish nor replace ordinary
loan, access, escape, movement or cleanup proofs.

A by-value conditional carrier formal, such as a payload enum with a reference
alternative, names no single stored reference. When such a function's complete
result contract is independently proved, the per-function return summary
admits that formal as its argument origin, as it does for a complete reference
result. When the function returns the formal whole, its `Return` loan must be
the one `Borrow` of the formal's guarded entry group, taken from the formal's
typed entry contract, with the same source, access, guards and paths. The
group's designated fields are then the source paths that the returned-field
contract of a class result is compared against. Relaying another function's
result for such a formal, as in `return relay(std::move(slot));`, is not yet
admitted: call lowering publishes the referent place while the call-result
check expects the argument's own place.

The shared context-free result-domain utility combines alternative suppliers
for one exact result field/guard/access as an OR. It preserves every available
supplier and treats an unresolved supplier as unknown even when another is
available. Only independently impossible suppliers can eliminate a case; an
empty required nested domain also eliminates its containing case. Eliminating
every case is a distinct no-normal-result state, never a complete empty result.
The utility only handles typed guard/domain relationships. Semantic analysis
and MIR must independently establish the actual argument origins and supplier
states; neither shares an origin interpreter or trusts the other's attached
facts. Nonempty call substitution preserves the complete guarded external
dependency group rather than selecting one representative origin.

A complete mapping may name static storage: its origin is `Global`, with the
storage symbol in `globalRoot`, and its source path is built from that
storage's own type. A capture-free `Symbol` place identifies that storage. The
provisional failure-effect proof retains the real module and initialization
inventory, so persistent-storage effects and exact receiver proofs have the same
declaration authority as the final program; only hosted startup is omitted until
the final lowering rounds.
A carrier whose contract names a global borrows the global's storage-typed
place, not a place typed as the carrier. The per-input returned-field checks
skip `Global` mappings, which have no call input; the global-origin check and
the complete-set flow verify them. A mapping may also name caller storage that
contains the referent, such as an accessor receiver, an owned element buffer or
an owning handle's checked pointee, provided that storage is reached without a
stored reference and stores none itself. A dereference through a class ends
that path. Published dependencies keep a `CallResult` accessor as their parent
together with its exact source, as reference loan facts do; a parentless
referent names the exact input place, either the reference argument's place or
a by-value carrier's `Stored` sources. Dependency transport compares sources
structurally: distinct place ids with one root, binding, symbol, capture,
temporary, value, loan, projection path, type and access are one source, as
when two calls through one reference formal each create a dereference place.

Constructor rows also retain complete result contracts. For a conditional
borrowed field, verification requires the exact `Initialize` destination and
operand, its independently reconstructed complete value fact, and a distinct
escaping Stored field group published after initialization. Publication must
preserve every origin, access, path and guard before retiring the producer's
group. That group is either the fresh loan of the producer that constructs the
value, or, when the value is a whole-value `Move` or copy `Load` of a by-value
formal, the formal's guarded entry group. The verifier takes that entry group
and its complete contents from the formal's typed entry contract, never from
the value annotation, and requires the transferred value to still hold exactly
that set, so a formal written before its transfer is not accepted. The
published field group remains active at every normal constructor
exit. Known-empty alternatives require positive typed evidence and publish no
manufactured loan. The current source constructor slice uses an empty body
and one common nominal field prefix; unrelated sibling obligations cannot be
dropped during projection.

A consuming receiver transfers its complete stored group through the ordinary
ordered call-input stages. A reference result may outlive the consumed shell
only when the independently checked callee contract derives it from the
retained external owners. Exact receiver entry identity and access, selected
alternative proof, group transfer and retirement remain separate requirements;
a receiver-owned origin cannot escape its destroyed shell.
Initializing a distinct direct field of the same exact non-union record does
not erase the retained field's complete fact. This separation is proved from
the concrete record and its declared field identities, not merely unequal
projection labels. Whole-object writes, unknown roots, union overlap and
unproved indirect projections retain conservative invalidation.

Program schema 111/body 91 also carries a call-local excluded-normal-result
flag for direct reference-result calls. This is distinct from a declaration's
`noreturn` contract: the callee can return normally for other inputs. Semantics
must establish that every supplier in the callee's complete normal-return
contract is excluded by these exact inputs; HIR retains the source-bound
selection proof. MIR independently reconstructs that exclusion from the
verified callee summary and the actual incoming dependency groups before
allowing a call with no result loan. Missing or unknown input facts cannot
establish exclusion. The call retains its effects and failure continuation,
while its normal continuation is unreachable. It cannot publish a result,
invent an empty reference dependency, or form later argument/receiver stages
after that terminal evaluation.

Program schema 110 also preserves whether a function's resolved
`borrowed_from` constraint was explicit source intent. Concrete HIR and MIR
function rows retain that bit alongside the formal/receiver origin; an explicit
constraint for a reference-bearing result without a resolved origin is invalid.
A generic constraint can be vacuous for a concrete dependency-free result while
retaining its explicit source intent. Source-to-IR tests establish
preservation, while MIR checks the resolved contract against executable return
facts. MIR does not inspect the AST to rediscover a completely erased source
annotation. This distinction prepares conditional complete-result contracts to
retain explicit restrictions without inventing a dependency for an empty
alternative; nonempty conditional call transport is not yet enabled by this
metadata increment. Body serialization remains version 90.

Version 105/body 86 additionally distinguishes absent borrow-set evidence
from a complete producer-attached set, including a proven empty set. An empty
payload construction names its exact concrete enum alternative; MIR checks
every active field's dependency-free traits against the sealed type tables.
Typed nested alternative paths accompany the set, independently of the real
origin-bearing loans. Dependency-free complete sets also follow exact local
initialization, copy, explicit/implicit move, value stages and branch joins.
The existing forward-dataflow solver independently reconstructs that transport
from constructor instructions and physical carrier storage; it does not use
an attached claim as its premise. A join preserves every possible alternative;
an unknown predecessor, moved source or mutable call cannot manufacture empty
evidence. Existing availability/lifecycle/loan checks remain authoritative.
The semantic loop visitor conservatively forgets incoming exact-alternative
facts, rather than treating its single visit as a proof for every iteration;
a dominating construction inside the loop can establish fresh evidence.
Conditional nonempty loans, loop-carried conditional dependency contracts and
declaration-owned result-summary proofs remain implementation goals. This
infrastructure does not admit reference-bearing enum payloads yet.

Version 107/body 88 preserves typed active-alternative conditions on the primary
loan dependency as well as each additional dependency. Conditions retain exact
enum identities and nested subobject paths across semantic, HIR and MIR records;
they are not qualified as body-local origin places. Until the active-alternative
proof is implemented, the verifier rejects conditional primary and additional
loans symmetrically rather than silently treating the first field as
unconditional.

Checked erased-reference operations preserve the exact origin and access while
changing the referent type. Restoration is explicitly unsafe and retains its
checked lifetime witness. The GTI function-pointer operation records either one
safe function instance or one stateless lambda instance, with its exact signature;
invocation uses the same explicit failure and cleanup protocol as other GTI
calls. It is not a native C callback adapter. Reference-field initialization
through a checked helper verifies the actual returned reference and publication.
The native reference-initializer representation currently admits a no-failure,
single-reference-argument helper within a reference-initializer prefix; the
helper executes once, in order. Supporting fallible or later-field reference
initializers requires an explicit preparation schedule, not a substituted input
address or duplicated helper call.

Version 101/body 84 reconciles
the checked range endpoint parking/publication inventory and shared failure
cleanup with the scoped-task contract introduced on main in version 100/body 83.
That contract adds exact
scoped-task adapter/loan groups and each adapter's scoped operand identity.
The private synchronous `ThreadScope` call consumes each exact task once and
retains its checked dependency loan through the operation's normal and failure
continuations. Program verification replays the operand's actual origins,
field paths, access modes, structural scoped eligibility and exact callable
entry. Body verification requires all task loans live and rejects overlapping
exclusive sibling accesses. Neither an empty group nor borrowed transport
changes the ordinary owned-thread contract. Concrete generic and pack forwarding
preserve each task's exact entry origins, including multiple reference fields
and repeated types. The production backend emits the complete group through
the scoped runtime service; the source-defined `parallel_invoke` wrapper maps
its recoverable status after join-all and cleanup. Cleanup-bound borrowed pack
elements retain separate lifecycle loans through their exact element drops or
owned handoff. Whole-binding identity alone is insufficient to discharge a
different element's obligation.

Fresh-value transport distinguishes lexical loan lifetime from active object
cleanup. A freshly constructed borrowed carrier can have no destructor work
or cleanup slot. Its unique SSA consumption remains required, and the concrete
class's independently verified active-cleanup fact controls whether a slot is
necessary. Absence of a drop instruction alone never proves trivial cleanup.

Version 99 records each
failure record's kind, producer or shared-cleanup continuation, and adds the
`ContinueFailure` terminator; see
[shared failure cleanup](#shared-failure-cleanup). It retains exact concrete
integral and payload enum identities, including their type/value arguments,
alternative fields and lifecycle facts. Version 95 also records
generated-default constructor identity explicitly on each constructor instance
and the independent generated-schedule obligation on concrete class rows;
backends must not infer that identity from its parameter count. Version 15
retains both exact enum identity and that obligation in detached declarations,
so removing both a constructor row and its selected target cannot authorize a
native default call. The combined revision preserves the retirement of sparse
capability/intrinsic and initialized-slot proof identities and deterministic
per-function NUL-result policy. Older version descriptions below record their
original additions, not an alternative current schema.

Concrete class rows retain transfer/share policy and any substituted unsafe
capability condition. The verifier checks that a conditional policy retains
its condition and that its result agrees with the concrete MIR capabilities.
Conditions are bounded, topologically ordered graphs of typed transfer/share
queries and conjunction/disjunction nodes. Semantics resolves source concepts
and transparent queries into this graph; HIR substitutes every leaf's type, and
MIR replays the graph without consulting public names. The one-parameter unsafe
assertion is a singleton graph. Nominal facts remain frontend-owned, while real
builtin aggregate components compose those facts. Removing the condition or changing the derived
capability is invalid MIR. The declaration catalog preserves the generic
condition in `lowered-program-v17`, and optimization replay includes these facts.

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

Exact generic enum identity and dormant embedding rows share the current
combined schema, which preserves embedding rows for dormant library snapshots
and uses the same current version for executable and library programs.
Storage ownership and conditional capability verification query enum traits by
the full concrete `SemanticType`, including type/value arguments. A primary
enum declaration ID does not identify a generic instance. Payload catalogs
provide the authoritative ownership, borrowing and concurrency facts folded
from actual alternative fields; nominal generic metadata does not become a
stored component. Missing exact rows remain invalid rather than granting a
storage or capability exemption.

Concrete parameter-pack elements retain the exact enum catalogue traits, just
as nominal classes and lambdas retain their resolved traits. Body verification
checks the pack root, element index, type, access and availability; program
verification checks enum traits against the exact integral or payload enum row.
Payload enum relocation and lifecycle facts must not fall back to the generic
scalar-enum defaults. This also applies to enum pack places synthesized while
lowering constructor arguments or loans. The existing semantic boundary for
owning payload enums inside by-value packs remains unchanged.

The combined serialization is `mir-v113`/`lowered-program-v22`, preserving
embedding rows, exact generic enum metadata, and concrete class alias catalogs.
Each class alias row carries its primary semantic declaration identity and exact
concrete type. The independent detached verifier rejects omitted, duplicated,
foreign-owner, and wrongly substituted aliases; program verification rejects
open concrete types and duplicate identities. Declaration-bound value argument
expressions are serialized as owned typed metadata, never evaluated by a backend.
Those program/declaration additions did not change body operations. The
`mir-v97`/`mir-body-v81` cutover removes the private prefix semantic/intrinsic
family and construction-preflight instruction and field. Ordered call inputs
now have no private extra-use exception; source capacity checks use ordinary
control flow and general object-lifetime calls.
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
When a forwarding call's result is discarded, its temporary loan ends at the
full-expression boundary. The consumed input's semantic endpoint continues to
acknowledge the input loan; it must not be redirected onto that temporary and
attempt to retire it a second time. Retained results continue to transport
their checked identities and dependencies normally.
An immediate temporary carrier receiver retains the same complete dependency
set. Its materialized shell has a distinct call-only loan, while every stored
dependency survives through full-expression destruction of owned fields.
Independent verification rejects missing dependencies, changed permissions,
forged shell authority and owner-loan retirement before cleanup. Shared-only
and mixed carriers use one schedule rather than a separate passive exception.
A temporary carrier borrowed only by argument-role `CallArgument` loans may
be consumed once those loans end; loan flow still rejects an earlier move. A
temporary borrowed by a receiver stage or a call result keeps the drop-only
rule. A copied carrier argument relays the binding's retained `Stored` group as
the parent of a scalar reference result, and lowering maps a `Parameter` entry
loan's complete dependency to the key-matched place under its referent.
Borrowed-result operators keep their ordinary value receiver stage. When their
result protects the receiver's own storage, lowering and independent verification
select the exact producer-owned temporary and its full-expression drop, retaining
the external stored dependency separately. An exact stored-field return instead
selects that external referent; the two sources are not interchangeable.
Source prefix-capacity checks are ordinary control flow. Their selected calls,
ordered inputs and object-lifetime operations remain visible to deterministic
inspection and replay; the retired private construction-preflight identity is
not serialized.
The loan contract separates a physical carrier reference from its inherited
external-owner authority. Ordinarily `storedDependency` names one exact `Stored`
loan; unlike `parent`, it does not denote containment or suspend that loan. Dependency
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
the concrete class layout. Concrete parameter packs retain a separate entry for
each opaque checked reference, including read-only references: the verified
element index, referent type, access, source, and entry identity must all agree.
A constructor reference initializer may execute a checked reference-returning
helper, then publish its result while retaining the original caller-owned
`Stored` dependency. The helper's body-derived origin, exact call result, and
field publication must agree; matching forged call/field metadata cannot invent
or erase the original owner authority. Each local value copy attached to an entry requires
an independently proven, dominating same-type initialization; merely adding a
binding to the carrier list grants no authority.
A lifecycle-bound stored result may obtain exclusive owner dependency from
its verified callee while accepting a shared address. Its incoming carrier
parent supplies the selected address permission and lifetime; it does not
supply mutable referent permission. A mutable parent therefore qualifies as
well as a read-only one, and the result's own designated field is provenance,
not a second dependency. For this sole-origin publication, body
verification retains the exact staged input, live parent, source containment,
Mutable result exclusion and cleanup endpoint. Program verification reconstructs
the parent from independently checked stored entry paths and binds publication
to the exact callee/constructor contract. Clearing that parent, changing the
input/target/access, or using the readonly ancestor while the exclusive child
is live remains invalid. Ordinary reborrows retain their existing permission
rule. Every finite normal return and constructor publication is checked,
including the structural trusted capability construction base; legal recursive
relays require no termination proof or new certificate subsystem.
Some invalid ancestor uses through stored readonly receivers still reach the
MIR safety firewall instead of a semantic source diagnostic; that diagnostic
frontier remains pending and is not satisfactory user-facing rejection.

Checked references to primitive text views use the same two-dependency model.
Their abstract `Stored` entry has no nominal field path: it represents the
opaque caller-owned text and grants read-only access only. Semantic analysis
creates that entry separately from the physical reference loan; HIR preserves
both identities and MIR lowering seeds both before body execution. Verification
requires the exact text-reference formal, independently checks every local
publication, and rejects missing, substituted or prematurely retired owner
dependencies. An empty field list is not evidence for an arbitrary carrier or
permission to omit the external dependency.
Reference-returning helpers preserve the physical view address and its stored
owner separately, including when their result is immediately used to construct
a cursor or another source-defined adapter. A retained adapter keeps the exact
accessor-result chain alive; a read-only chain may terminate at the matching
live reference formal with the same verified external dependency. When an
adapter stores a named reference, semantics retains that reference as its parent
even for shared access; its endpoint must outlive the adapter and dependent
copies. MIR preserves this parent separately from the backing owner. Literal-backed
locals may omit an external owner only when verification proves their literal
initialization through write-free copies or read-only local reference aliases.
For an alias, both its supplied address and its loan origin must prove that
same static-lifetime property; clearing borrowed-state flags is not proof.
Checked restoration of an opaque reference has a separate explicit proof:
the concrete restored field paths and access must match a verified restore
operation or body-proven helper summary. Its hidden dependencies remain bounded
by the original checked witness rather than becoming invented `Stored` owners.
A named restored-reference return may retain that exact caller-parameter witness
only when its source and access agree, there are no additional, designated, or
cleanup-bound dependencies, and every local reference carrier has a unique
typed initialization from the original call result. Producer, initialization,
and return publication must occur in that order on every path. The pointer
carrier cannot be reseated, moved, addressed, or forwarded through a CFG edge.
The fresh `Return` loan preserves the witness and immediately retires the
body-local call result; ordinary origin, liveness, escape, and permission checks
remain mandatory. An alias identity proof alone grants no escape authority.
An abstract entry describes declared field access, not a capability-produced
loan or the caller's cleanup ownership. In particular, borrowing a guard does
not acquire its lock-release obligation. Local bindings of stored referents
retain their concrete reference-field dereference; semantic overlap identities
are not substituted for executable projection chains.
Constructing a cleanup-owning child from an incoming carrier's reference field
uses that same abstract entry as its parent authority. A concrete field borrow
need not be spelled as a `Loan` operand: verification requires an active entry,
its exact declared reference-field/dereference path, sufficient access, and an
argument place identical to the child's recorded origin. A different field,
carrier shell, read-only authority for a mutable child, or merely overlapping
place cannot satisfy the proof. This rule applies equally to ordinary methods
and nested scoped jobs; it does not grant the parent any cleanup ownership.
A call result borrowed from a receiver behind a stored reference field records
that exact selected receiver place as its source, as a `this` field receiver
does. The field may belong to a formal, a local carrier, a local reference
alias, or a holder that another call returned by reference
(`holder.box.slot()`, `teller.account.balance.lock()`,
`outer.inner().box.slot()`). The carrier's own loan source
would name the object that holds the reference, or another of its referents,
instead. A formal's alias loan parents such a result only while its access
reaches the receiver through every stored reference on the way; behind a
read-only stored reference, that reference's abstract entry is the parent, as
exclusive publication requires. The alias that selects the receiver through
the stored reference its parent entry describes is that input's path, not a
competing sibling; every other overlapping loan still conflicts. Semantics
records a carrier with one stored reference path as a compact sole-origin loan;
MIR lowering names that path in the loan's designated fields when the
reference's referent type is the loan source's type, so canonicalization
resolves a borrow through the field to its referent rather than the carrier
shell. The path is named only while every carrier is a value of that one
class. When a carrier of another type joins, such as a view of the referent
copied out of the holder, the loan returns to the compact form. A named path
is still a single origin. An input that is the inner object holding the
reference reaches the same referent: lowering selects through the input's own
path, and program verification accepts the input's path as the tail of the
carrier's. A result behind one field of a mapped carrier depends only on that
field's dependency: lowering does not copy the carrier's other owners onto it,
and program verification expects only that dependency. A derived loan is
contained when it lies in any place its parent protects, as a direct reborrow
already is.
For a composed result's additional dependencies, a projection through the exact
reference field of a borrowed carrier retains that carrier's abstract external
entry, not its temporary physical reference. Canonicalization keeps the formal's
leading reference dereference separate from the stored field's dereference.
Whole-carrier access and owned-field projections still retain the physical
loan, and a direct reborrow without the explicit stored-dependency edge keeps
its immediate parent. Verification rejects ending the external entry before
its forwarded fields, even when the physical reference has already ended.
An additional input can also borrow an independently owned local, without an
incoming loan. A non-escaping stored result may combine that direct binding or
owned-field source with a primary getter-result parent; the local is not required
to lie inside the getter's protected storage. This containment distinction
requires an unambiguous binding-rooted source without dereference or inherited
loan authority. Exact call/constructor field mappings still establish the input
identity and permission; ordinary ownership, conflicting-access and scoped-task
checks protect it until the carrier ends. Borrowed or pointer-derived inputs do
not acquire owned-local authority through this rule.
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
A fresh payload enum stored in a source-class field follows the same rule:
its exact `PayloadConstruct`, field `Initialize`, escaping `Stored` publication,
and immediate producer-loan retirement preserve the entire conditional
dependency group. The native enum value already carries the selected reference
payload. Publication emits no separate pointer and never reads an inactive
alternative to reconstruct provenance.
When another alternative owns cleanup, constructor staging may use the enum's
private inactive native state: it has no active payload and therefore no borrow
or cleanup to observe. Containing classes qualify only through recursive exact
field empty-state checks; direct reference fields do not qualify. This is a
construction-only representation facility, not a source default constructor,
assignment permission, or substitute for field publication and rollback proofs.
The child formal itself may receive a fresh ordinary constructor result.
Origin replay peels its exact prepared input and follows the selected field
through that result's executable constructor before reaching an enclosing
formal; a primary loan source is not a substitute for this field-wise proof.
The replay is bounded by the body value and concrete constructor inventories.
For a fresh owning argument, body verification also requires its active stored
loan's exact source, access and parent ancestry, plus the checked moving
parameter slot and that slot's transfer into the consuming constructor.
If this constructor fails, its callee has already drained the consumed
parameter. Lowering retires the caller-local child capability before draining
its external parent, without adding another object drop. Copied or borrowed
arguments keep their existing retirement schedule. These are compile-time
loan/publication facts, not runtime lifetime metadata or new allocations.
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
A by-value carrier that a call builds from a reference argument naming exactly
what an accessor returned (the call's own result place, as in
`b.get().begin()`, or a reference binding initialized from it) has a `Stored`
loan whose source is the place rooted at that accessor's `CallResult` loan and
whose parent is that loan. It holds exactly the accessor loan's stored
dependency, which is none for ownerless text. A temporary accessor result has
no semantic loan of its own, so its semantic carrier contract names the
accessor's owner; lowering refines that to the returned referent, and a binding
that retains the accessor's loan does not move the carrier's source back to the
owner. The program verifier admits ownerless text at that exact referent only
under its result loan.
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
classified, including when nested inside a fixed array. General typed
relocation selects its implementation only from this verified proof. The
former private prefix-relocation family is retired: source owners now prove
their capacity/live-prefix preconditions before general unsafe relocation.
The separate bulk-range operation additionally requires independently verified
bitwise-relocatable elements; a source concept chooses bulk or structural
per-element relocation. Both preserve source/destination lifetime behavior.
There is no backend-only relocation operation, and native representation traits
never grant language eligibility.

Borrow-carrier value moves follow an owner decision of 2026-10-28, taken as
the recommended option while the owner was away.
`mirTypeMoveRelocationKind` derives the relocation that a generated value move
performs. It equals `mirTypeRelocationKind` except for borrow carriers. A
carrier whose borrowed state is references and views into storage outside the
object relocates structurally, as the addresses it holds, and its loans follow
it as they follow any value move. No carrier holds a live borrow of its own
storage, for two reasons. MIR forbids moving a borrowed place, and semantics
never lets a carrier borrow storage of the object it is built into. The body
verifier rejects a moved carrier that carries a non-entry `Stored` loan of its
own binding. An opaque entry loan is exempt: it stands at a by-value
parameter carrier for its caller's external referents. Program verification
re-derives every class's move relocation. A class that is not a carrier must
match its relocation trait. A carrier may relocate only when it is a movable
ordinary record with a generated move. Source-visible relocation is unchanged:
`traits.relocation`, `std::relocatable` and relocating swaps and exchanges
still treat carriers as unavailable. The alternative, letting carriers satisfy
`std::relocatable`, would be a language change and was deliberately not taken.

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

Ownership and scalar-initialization dataflow follow only the executable edge
of a typed literal boolean branch, including its exact same-typed identity
chain. Structural CFG edges remain intact for dominance, loan endpoints and
cleanup validation. This prevents an impossible natural exit of `while (true)`
from changing a post-break moved-state assignment into a live replacement.
Unknown conditions still join both edges; this proof does not evaluate calls,
loads, arithmetic or constant contexts, and does not relax loop-backedge or
use-after-move checking. Ownership dataflow first solves the complete executable
CFG, then validates each instruction against stable block-entry states. This
retains exact `Available|Moved` reinitialization facts at a conditional break
without rejecting an intermediate worklist state before its other predecessor
arrives; transfer states remain derived from MIR operations, not asserted event
results.

Lifecycle cleanup publication still uses the complete structural CFG: guarded
temporary drops and their exact conditionality must remain valid even when a
literal makes one a runtime no-op. Only an ambiguous consuming source's
`Available` precondition can additionally use feasible lifecycle availability.
This lazily reuses the same literal-successor proof, lifecycle event transfers,
invoke-success initialization and join lattice. Unknown branches remain joined;
unclassified/dead blocks retain structural validation. Destination initialization,
drop conditionality, loan retirement, full-expression/lexical boundaries and
terminal obligations are never relaxed by that refinement. Ordinary bodies
need no second dataflow or allocated callback; a queried block's event prefix is
advanced incrementally rather than rescanned for every consuming source.

Checked indexed swap is a sealed intrinsic over one mutable fixed-array
reference and two staged `uint64_t` indices. Source prefix storage instead
uses general object-lifetime operations; it is not another intrinsic operand
kind.
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
payload-enum results activate the existing result-slot identity;
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

Not every field initializer has an explicit stage. Borrowed-state scalar
fields such as `std::string_view`, and other initializers outside the staged
families, are published by the constructor record alone: the record's
initializer names its single HIR argument, the body defines that argument's
MIR value, and the backend writes the field from it with no `Initialize`
instruction. `mirRecordPublishedConstructorInitializer` identifies these
record-published values, and an explicit `Initialize` carrying the same
initializer index suppresses the record so one source initializer never
publishes twice. Such a value is live for the record even though the body
records no use of it; the verified dead-instruction rewrite refuses to remove
it, and the C++ emitter publishes from the same query.

The initializer index on an instruction names only that initializer's
publication, its `Initialize` or `InitializeBase`. A temporary that ends with
the initializer's full expression, such as a method receiver materialized for
`Meters lap = Feet(30).to_meters();` as a field declaration default, is
dropped by an ordinary `Drop` with no index before the initializer's
full-expression boundary. Verification rejects the index on any other
instruction, because program verification counts the instructions carrying it
as that initializer's publications.

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
Projected payload reborrows name their immediate live parent loan as the
`Borrow` operand, rather than a read of the parent's value. Identity-only
canonical containment follows place-valued reference forwarding while still
checking the selected variant/field. A shared local child may reborrow a shared
parent without gaining mutable access; parent activity, CFG containment,
non-escape and child-before-parent endpoints remain mandatory and independently
verified. A payload borrowed through a reference parameter is such a child of
the parameter's entry loan. Returning it publishes a fresh parentless Return
loan from the child and retires the child at that boundary, as a returned
payload selection does. The child is never reclassified in place, because its
own `Borrow` still reads the entry loan.

Native emission tail-forwards a cleanup recipe only when its verified schedule
contains one possible action: a class with no source destructor and exactly one
aggregate child, or an enum whose every alternative has at most one cleanup
child. It passes the original failure record and unwinding mode and returns the
child's result; an empty alternative returns success. A cleanup sequence does
not establish a failure context before its first action. After the sole action
fails, the otherwise-created context would be activated and immediately
destroyed, with no later action able to observe it or raise a second failure.
Child sequencing, source-destructor and multi-child recipes, and the caller's
native retirement after either success or primary failure remain unchanged.

Canonical storage identity preserves the concrete executable place and its
field projections through address-preserving place-valued SSA operations.
This is distinct from a lifetime dependency origin, which may deliberately
refer to an enclosing owner. A call's receiver is not automatically its result
storage. Traversal is bounded by the place/value/loan graph it follows;
cyclic forged forwarding remains ambiguous and is rejected. Exact enum
relocation traversal similarly distinguishes concrete instances rather than
treating every occurrence of a generic primary as the same recursive type.

Canonical checked-place forwarding does not cross `RawDereference` or
`RawIndex`. Such projections designate pointee storage, not the storage holding
the pointer; a read-only carrier shell therefore does not make the raw pointee
read-only. Ordinary pointer type/access and lexical unsafe obligations remain
separate from checked loan ancestry.

Program verification independently resolves Value-rooted raw class field
projections through the exact MIR class catalog and checks their final type.
It reuses the ordinary field/base/array/reference projection query; a missing
root, invalid selected field, or contradictory final type rejects the owning
body/place before lowered sealing. Native admission retains its independent
supported-layout and final-type checks. This program-level check does not yet
extend through Lambda capture-field selectors, whose exact environment identity
requires a separate proof; it does not declare those paths universally valid.
Class projection prefixes are still checked before that boundary, and each
independently materialized prefix place retains its own final-type check. This
is the current verification coverage, not a permanent source-language exception.

Program v74/body v64 serialize signature receiver capability in semantic type
metadata. Read-only, mutable, and consuming signatures cannot share a serialized
identity. The lowered declaration seal includes the same field in
`lowered-program-v9`, rejecting a changed class-pattern qualifier.
This revision also includes `retained-receiver` call inputs and the
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
Fixed-arity lambda invocations lower their HIR call plan to an ordered MIR
`CallInput` receiver followed by argument stages, so a base-reference
parameter receives a prepared borrow of the exact base type rather than a
direct borrow of a derived place. Lambda invocations with an expanded argument
pack retain an explicit HIR receiver and lower to the same receiver stage
followed by concrete argument stages. The call keeps its exact lambda
identity, capability and flattened signature; ordinary program verification
rejects a missing receiver or forged parameter type. No native pack matching
selects the GTI call. A cleanup-free moved environment has no full-expression
obligation, so its receiver is a `Value` stage of the exact Move rather than a
`MoveValue`; the once-callable receiver proof follows that stage to the Move
and still requires its exact ownership event.
Scoped HIR result regions lower entirely to existing MIR operations: an exact
typed temporary, per-return initialization and lifecycle transfer/reparent,
local cleanup, and an ordinary join transport. Their returns cannot produce
the surrounding body's Return terminator. The result is active in failure
cleanup before local destruction begins, and keeps the enclosing expression's
lifetime after successful local cleanup. Class and closure results retain a
Value lifetime obligation even when cleanup is trivial: every return activates
the same typed slot, and independent lifetime flow proves availability before
movement or argument preparation. No backend reconstructs multi-return
initialization from control-flow patterns. Region joins reconcile outer loan
bindings, transferred endpoints and temporary states, rather than inheriting
the state of the last visited return. Borrowed region outputs remain rejected
until their provenance/loan-transfer join contract is implemented. No backend
interprets macro syntax or invents local-return cleanup.

Initializer expressions use these same lexical lifetimes. Each merged
program-initialization step separately records its persistent storage and the
exact binding/symbol/place identities of its lexical locals, plus their nested
full-expression identities. The HIR initializer-reachability query supplies
that inventory; MIR verifies disjoint storage/local identities, local use and
markers confined to their owning step, and unchanged dense storage ordering.
Full-expression canonicalization repairs the step metadata together with drop
obligations and markers, including cleanup-sharing fallback. These locals are
not persistent program storage or constructed field rollback obligations.

Program v72/body v62 add the generated `ReturnResult` drop obligation. A
completed owning return operand is reparented out of its full-expression into
this pending lifetime when callee cleanup can fail. It retains the exact
operand place, HIR value identity, and cleanup recipe, with no independent HIR
obligation or full-expression ID. Only the final instruction before the
matching Return may transfer it to the caller; failure continuations clean it
before older temporaries and locals. Verification rejects early transfer across
an Invoke, missing reparent/publication, wrong failure-cleanup phase, and a
fabricated full-expression lifetime. Failure-free continuations need no extra
obligation. Exact owned-callable return/field transport proofs recognize this
same pending return lifetime. A failure-only drop of that exact result is not a
second successful use; reparenting alone is insufficient without its final
matching publication. Body lifecycle verification still owns activation and
failure paths, including rejection of missing publication and incorrect cleanup
phase. An owning conditional or propagated success can retain its pending return
in the existing Temporary-rooted join rather than inventing a second value slot.
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

Cleanup-free class construction still carries its selected constructor's
failure edge. Absence of a result-drop obligation does not suppress Invoke,
including in a constructor field initializer. The exact success continuation
publishes the field; failure propagates the original record and drains earlier
completed fields. The C++ field-placement proof also admits a failure-free
owning constructor followed immediately by publication of its initialized
value obligation, retaining the same field rollback for later failures. Neither
case invents a cleanup obligation for a cleanup-free result.

Source-defined prefix owners invoke general `ObjectConstruct` for final-address
construction, retaining selected reference/value parameter roles after concrete
pack expansion. Capacity and logical-prefix checks are ordinary GTI control flow;
there is no private construction-preflight instruction or extra-use exception.
Every prepared `CallInput` is consumed exactly once by its ordered invocation,
or abandoned through its represented terminal failure cleanup. Owning arguments
remain active until the construction handoff. The source owner publishes its
initialized length only after construction succeeds.

An operation-free `CallInput(Value)` forwarding one already computed SSA value
does not inherit the source expression's unsafe-operation classification.
The executed `Load`, pointer `Compute` or selected unsafe `Call` keeps that
classification and its actual memory effects. This suppression requires exact
typed SSA forwarding, no place access, destination, receiver, loan, target,
intrinsic, lifecycle, synchronization or prepared-owner/drop metadata. Explicit
stage metadata and owning, borrowed and retained-receiver stages are unchanged.
Source spans and ownership events remain attached; preparation does not execute
the earlier expression a second time or relax its lexical unsafe obligations.

Copying or borrowing a place returned by an already-evaluated call likewise
does not repeat that call's local failure effects on `CallInput`. Place
dependency lowering executes the producer and its checked `Invoke` before the
prepared input is published. The producer retains its detector, source location
and failure cleanup; the later copy/borrow retains only its own staging and
ownership events. Unexecuted place selection checks remain on their owning
operation, including the existing checked array-address prefix schedule.

The backend's passive generated-class representation proof composes through
nested concrete record and fixed-array fields. Every visited record must retain
generated destruction and no active cleanup/drop state; copy publication also
requires generated, available copying at every nested record. User destruction,
borrowed fields, state-bearing bases and non-passive storage do not become
passive merely because an enclosing type has a generated special member.
The exact constructor parameter-to-field schedule remains unchanged. Recursive
proof state rejects cycles and memoizes shared nested records, while the
existing scalar/array-leaf path requires no proof-state allocation.

MIR independently checks the selected target, typed inputs, dominance, failure
sites and cleanup transfers. The object-lifetime operation's effects compose
with the selected constructor's effects, and the original failure record drives
field rollback and caller cleanup. Source drain guards continue remaining
element destruction after the first failure using ordinary destructor bodies.
Reference, owner-dependent text and raw-pointer inputs retain their existing
access/escape obligations; native copyability grants no borrowing permission.
Concrete pack flattening uses the same bounded `CallInput` type-compatibility
predicate as independent MIR verification. It retains the source operand type
and exact selected parameter type for existing null-pointer and pointee-const
conversions, rather than rejecting a valid conversion or changing the selected
constructor signature. Removing pointee constness and changing pointee types
remain invalid.

Generated same-type raw copy/move construction has no source-constructor target.
Direct
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
public container spelling grants admission. Source sparse and prefix wrappers
retain their own payload constraints and general lifetime/borrow obligations;
they no longer have private MIR storage types that recognize those public
container names. An empty optional storage value is not an exemption from its
independent payload check.
Publication also rejects an owner-dependent text expression, matching sparse
and prefix storage. Text parameters conservatively retain that dependency even
when a caller passes a literal; the generic optional value constructor cannot
store them until its contract can distinguish static text from an owner loan.
Empty optional text storage remains permitted.

Reset of the same inline capability uses the shared element-removal cleanup
recipe also used by general raw destruction. MIR independently
derives the payload recipe and its destructor failure effect. Fallible reset has an Invoke
edge, with no new local failure site: the payload destructor's original record
propagates through caller cleanup. The representation retires engagement
before running that recipe and ends the disarmed native shell afterward;
failure cannot make the containing owner destroy the payload twice.

Source prefix erase uses general destruction and relocation in ordinary GTI.
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
in the recipe, and unrelated instructions cannot carry a recipe. Both private
removal families are now retired. Source sparse and prefix policy invoke general
`ObjectDestroy` below with the same exact recipe contract.

`ObjectConstruct` records unsafe final-address construction with the exact
selected constructor, concrete input types, prepared-parameter handoffs and
constructor-failure edge. It reuses the ordinary constructor body and rollback
schedule. It has no container preflight:
alignment, extent and vacancy belong to the unsafe caller. The verifier rejects
unrelated allocation metadata, borrowed destination authority and mismatched
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

Explicit removal retires the sparse live byte or prefix length in source
before cleanup, ends the disarmed element representation once, and uses the
ordinary MIR `Invoke` failure edge. Source checks create local records at the
removal site; general object destruction propagates a destructor's already
complete record without overwriting it. Caller cleanup drains the remaining
live elements. Source-defined vector/storage `clear()` loops use these same
operations. Version 63 records the earlier private arbitrary-index prefix
erase protocol, not an additional current storage engine.

The former `StorageTryAdopt`/`StorageAdoption` capability and its private
sparse destination type are retired. Descriptor validation, two-region lease
publication and metadata initialization are ordinary source operations.
The unsafe binding contract establishes provider obligations; general
`ObjectLifetime` calls establish typed construction, relocation and destruction.
No MIR instruction adopts a sparse bitmap or implicitly allocates bookkeeping.

Historically, program v61/body v53 extended the cleanup tree with one unprojected
element recipe for owning storage and prefix storage. Both private storage
engines and their native live-slot walkers are now replaced by ordinary source
reverse traversal and general `ObjectDestroy`; the remaining paragraph records
the original protocol. A collection drop walks live
elements in reverse index order (skipping uninitialized sparse slots), runs
the exact child recipe, ends the element representation lifetime, and finally
releases the allocation. The live bit/prefix length is retired before entering
element cleanup. Nested collections share the explicit primary/secondary
failure protocol rather than native exception unwinding. Direct
lifecycle-guarded source-destructor leaves, unique owners, and
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
down empty representations; they do not consult AST/HIR to reconstruct cleanup.
A private runtime scope can carry an incoming cleanup-failure context through
calls, but the verified MIR record and schedule remain authoritative; it is
not a second source-level failure system. `aggregate_cleanup_failure_runtime` and
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
`PayloadConstruct` exact-checks that table, transfers every active owning
operand once, and initializes one enum value
obligation; `PayloadExtract` and switches observe an active enum place without
copying the enum, or, for a consuming switch, move fields out of its hidden
subject binding with one final `TransferOut`. Nested enums, fixed arrays and
result payloads use structural success/error drop recipes, not class-only transfer
rules. Program verification rejects recursive inline enum/class/array layouts
before invoking recursive trait or drop queries; owning indirection is not an
inline-layout edge. A prepared move identifies its source/generated drop in
the correct HIR/SSA identity domain and preserves the exact place and slot;
missing or substituted identities do not authorize publication.
Program v52 serializes ordered type and `uint64_t` value arguments on every
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
marker role used by `?` propagation, which through `std::tryable` branches on
its `has_value` call and returns the residual's construction. Each propagation
failure edge retains a path-exclusive boundary alongside the one ordinary
continuation boundary, so verification can prove exact temporary and lexical
cleanup without reopening HIR. Program v48 and body v44 serialize the exact
designated stored-reference field on a by-value carrier's `Stored` entry loan.
Verification binds that field to the unprojected formal-parameter source and
uses it to prove the exact runtime referent or returned carrier transfer without
backend reconstruction. Program v47 and body v43 exposed the lowering-owned
dormant trusted-wrapper marker on every body header. Program v46 added the
ordered managed-thread task-adapter inventory. Each row
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
local fold client checks its eligible literal/identity/compute rewrites under
shadow agreement with the HIR constant-analysis replacement for the same HIR value;
other MIR passes use their own independently replayed proof records. The
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
calls remain conservative. The one declaration exception is the declared
process-allocator runtime contract below: its exact classified declarations
carry a precise local summary that callers consume like a source summary. Verification independently derives the least
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
explicitly defaulted default constructor or destructor as `Generated`.
That policy does not create a user-authored body. A selected generated default
constructor whose fields or base need execution now has its own exact MIR
schedule; trivial targetless construction and structural drops retain their
verified generated operations. Neither policy authorizes a native body walk.

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
synchronization, or the other dimensions of the separately implemented
`MirEffectSummary`.

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
When an additional concrete dependency originates through an independent
reference-formal entry or an explicit loan-rooted place, it retains that
input's authority separately from the primary parent. Canonicalizing a source
place does not erase the formal's entry permission. Body verification checks
the secondary input's liveness, containment and access at publication, retains
it in the lifetime/ancestry graph, rejects cycles and child-before-input release,
and continues to check every protected place for conflicting access. It does
not require unrelated inputs to fit inside the first parent's storage. The
ordinary primary reborrow and its suspension/reactivation remain represented;
escaping publications keep their separate transfer contract.
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
A by-value `Stored` entry loan initially names only its actual formal parameter,
even when completed semantic flow already associates the dependency with a later
local. Lowering records that local only at its actual transfer. The formal remains
an immutable provenance anchor after movement, not a second live exclusive value;
current binding availability is tracked separately. Its exact reference-field
access is independently checked for cleanup-free as well as cleanup-bound entries.
For a cleanup-bound entry, every additional carrier must be reachable
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
Source-defined iterators can carry that exact owner through a separate
`storedDependency` rather than a parent reborrow; both relations retain their
independent identity and child-first endpoint checks. The program verifier
requires a borrowing call or constructor's result loan to remain `CallResult`
or `Stored`, matching the producer, rather than accepting a forged `Local` loan
with otherwise unchanged source metadata.
For `CallResult`, `producedBy` must equal the owning call/construct's source
value identity as well as retaining the exact origin and access mode. A
different producer is compiler corruption even when the selected source place
is unchanged; backend address publication cannot repair that provenance.
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
transfers the exact runtime carrier and origin. A reborrow remains a child of
its live carrier; a consuming by-value relay instead publishes an independent
result dependency on the same external origin after retiring its input, as
specified below. The mutable parent is suspended until all child reborrows
and the carrier child end. Structural and flow verification
require the selected constructor/call operand to resolve through the declared
parent, require child-first retirement on every path, and reject copy,
reseating, forged ancestry, parent use while suspended, or an escaping child
shape. The C++ backend merely represents the already-verified reference field
and schedule.

A by-value call that consumes a complete borrowed carrier retires the input
loan on the call-success continuation before initializing the returned carrier.
An exact same-type noncopyable argument relay transports the semantic loan
identity to its published `Stored` result only when the complete source,
permissions, ancestry and dependency set agree and semantics has not assigned
the result a distinct retained identity. A fresh result keeps its independent
endpoint; the input's last-use endpoint only acknowledges its retired input
loan, including carriers with mixed shared/exclusive or multiple dependencies.
It must not leave the result
parented to the consumed shell or resurrect the retired formal at a later local
initialization. Independent verification still rejects forged field paths,
weakened access, retained consumed ancestry and omitted retirement.
If HIR also records that loan's last-use endpoint on the enclosing statement,
lowering acknowledges the already-emitted endpoint exactly once for that
statement. It must not emit a second `EndBorrow` or ignore arbitrary inactive
loans. For a conditional statement, only HIR's exact declared enclosing endpoint
can be acknowledged by nested transfers. Each arm has independent endpoint
state; the join retains an acknowledgment only when every normally reaching
arm supplies the same identity. A terminating arm supplies no fallthrough
evidence. When a consuming call retires a carrier's group in one arm of an
`if`, `?:`, `&&` or `||`, the carrier is moved on that path, so semantics
rejects any later use of it. Lowering then retires the same group at the end of
every other normally reaching arm, records the same endpoint there, and only
then joins, so every edge agrees. This applies only to a cleanup-free group: a
lifecycle-bound group is still needed by its carrier's drop, and a carrier with
a destructor consumed on only some paths is not yet lowered. Loop and switch bodies cannot export an unproved path-only transfer
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

A reference bound to a field or element of a call's returned reference, such
as `std::vector<int32_t>& v = holder.get().items`, `root->items` or
`(*root).items` through an overloaded `operator->` or `operator*`, or the hidden
range of `for (auto& x : root->items)`, retains that call's result loan, as a
reference bound to the returned reference itself does. Its address is the
projected place rooted at that loan. Lowering does this when the place's only
projections are fields and elements and the call's loan has the binding's
semantic parent: none for a local owner, the entry loan of a reference
parameter such as `std::unique_ptr<Dir>&`. A Local loan on the projected
place would leave the call's owner unborrowed, and one on the semantic origin
could not name the address; `referenceAddressRetainsLoan` rejects both.

A carrier construction that stores such a returned reference also retains
the exact call-result loan as its parent, whether its input is the direct
accessor result or a reference binding initialized from it. Its Stored loan
keeps the accessor result's external dependency separately from the physical
referent address. This applies to `Construct` as well as function calls;
verification checks the exact constructor input and dependency before either
backend can use the carrier.

A read-only binding through a mutable owner's smart pointer retains a
read-only call-result loan: semantics selects the read-only `operator->` or
`operator*` step from the binding's use (static semantics, accessor pairs), so
two such bindings, or a binding and a later read of the owner, coexist.

A reference to a borrowed carrier, of the carrier's own type or a base or
interface type, is initialized from a loan on the carrier object, as a `Borrow`
of a same-class reference publishes it. `verifyMirBorrowProducers` rejects one
backed only by the carrier's `Stored` loan (`reference to a borrow carrier is
not backed by a loan on the carrier`), because that loan designates the
external referent and leaves the carrier movable while the view lives. An
upcast call argument borrows the carrier's place, a call relaying it borrows
the carrier's shell, and an upcast reference return publishes the parameter's
exact referent.

A call returning text by value under a loan must carry borrowed state in its
result when the loan's source may hold owner-dependent text
(`text call result keeps a loan on owner-dependent text but carries no
borrowed state`). `TextContracts` decides that from the source's ownerless
provenance: the source is ownerless only when its text is known to hold no
formal's text and no formal's text field, such as a literal-backed local,
static text storage or an ownerless text field. An owner such as a
`std::string`, a text formal and any text of unknown origin are dependent.
Lowering keeps a conservative loan on the argument of a call that relays
ownerless text, as `relay(text)` does over a literal-backed `text`, and
semantics correctly gives that result no borrowed state; the provenance test
keeps that relay valid while a dependent relay without borrowed state fails.

A dynamic-text return summary is an upper bound on dependent alternatives,
not a requirement to invent an owner for static storage. A read-only summarized
function may return a literal-backed text view or a null C-string alternative
without an escaping loan. Program verification independently requires the exact
typed string/null literal producer, with no borrowed state, input operands,
loan, side effects or lifecycle events. A copied stored view is the other
witness: semantics rejects storing a dynamic view in a field or fixed-array
element, so one exact `Load` copying a place whose final `Field` or `Index`
projection has the returned view type, with no borrowed state, loan,
destination, receiver or lifecycle events, is also lifetime independent. A
reference field ends in a dereference and never qualifies. A third witness
is the result of a static call of a source text function that has no result
dependency, or of a text field relay, carrying no loan and no borrowed state:
the callee's own returns are proven ownerless, and a field relay without a
loan reads a field that holds ownerless text. A method whose predeclared
receiver relay its body never uses therefore returns `this.inner.label()` as
it returns a literal. Clearing a borrowed-state flag or a return loan does not
establish any witness. A function whose returns are all stored views or
literals summarizes no owner dependency at all. All dynamic alternatives still
publish and verify their exact escaping loan, and callers conservatively retain
the function's owner dependency regardless of the runtime branch.
A dynamic alternative may also return another text relay's result, such as
`return id(s, n);` or a recursive `return first(s, n - 1);`. Its Return loan
then transfers that call's own `Stored` result loan instead of the formal's
entry loan. The transferred loan must have the entry's exact source, access and
additional dependencies, with no parent or designated field. Its unique
producer must be a `Call` with an Argument origin and no field mapping, and the
returned value must be that call's result. `verifyMirBorrowProducers` already
proves that such a call's result loan names its selected input, so the formal
is the only owner either way.
A local copy of a text value relayed from a reference formal uses the same
entry authority. The abstract stored-entry publication proof checks the
call-result loan, its exact value input load from the formal's dereference,
and dominance of the call over the local initialization, including an `Invoke`
success edge. A matching declared return dependency alone cannot publish the
entry into an unrelated local.
The success loan always uses read-only access, including when the enclosing
method has a mutable receiver. A by-value nominal carrier parameter can provide
that success through its one exact designated reference path; the Return
operand retains both the external owner loan and the separately verified
runtime field/dereference place, never the carrier shell's address.

`verifyOwnerlessTextContracts` independently derives every ownerless text
input contract. A write to a text field, to a text array element wherever the
array lives (each element of an array literal included), to a payload-enum
text field, or any other checked text assignment outside a local text binding
needs an operand whose source is known: a string or null literal (including
`nullptr` for a `c_string` field), an ownerless field or array-element
projection, unprojected static text storage, text read from raw memory, a
temporary or block parameter whose every incoming value is known, a formal
parameter, or the result of a call to a source function or lambda whose
returns are known.
A formal becomes a requirement of its function. Requirements propagate to a
fixed point through calls and constructions and must equal the semantic
`ownerlessTextParameters` exactly. Virtual, override or native dispatch,
function items, lambdas and native exports cannot carry a nonempty
requirement. A local text binding is not text storage and may hold dynamic
text; a reader traces every write of it instead: its initialization and each
assignment, in any order and in a loop. A local already being traced on that
path adds nothing new, and a store through a mutable borrow of the local is a
checked text store, which holds ownerless text or text its function requires
ownerless. Semantics never nests
dynamic text in an array (`L-OWN-02`), and every safe element write is a
checked text store here, so an array element is ownerless text storage
wherever the array lives: a by-value array formal holds what its caller's
array held, and an element read is an ownerless result proof. Static text
storage counts as ownerless because semantics admits only ownerless
initializers and stores into it; MIR checks assignments to it but does not
yet re-verify global initializers.

An external C declaration returning an untracked `c_string` is also an exact
text producer. Its pointer validity and retention obligations belong to the
native boundary; the verifier does not require a source body for that call.
This admission requires C linkage, no source definition, and no result loan or
borrow contract on the target or call. It does not apply to GTI-owned strings'
dependent C-string observers or erase an annotated native result's dependency.
Text read from raw memory, below a raw dereference or raw index with only
field and element selection after it, is an exact producer for the same
reason: unsafe code took on its validity and lifetime obligations, and raw
writes are not checked text stores, so it is an inert native value with no
GTI owner to reconstruct. A checked dereference below the raw step reads a
referent elsewhere and keeps the ordinary rules.

A call's result holds what its callee's returns hold, mapped through the
call's arguments and receiver. `TextContracts` summarizes each source
function's returns once, from bodies alone and never from a declared
contract, so declarations cannot justify one another cyclically. A summary
is the least fixpoint over the returns of each set of mutually recursive
functions: it starts as that of no completed return and only grows. A
recursive call reads the summary computed so far, so a recursion that
permutes its formals, such as `return f(t, s, n - 1);` beside `return s;`,
reaches every formal that some depth of it returns. The first function of a
recursive set to be evaluated repeats its returns, with one evaluation of
each other member per pass, until no summary of the set grows, and then
completes them all; a summary that read an unfinished one stays provisional
until then. A recursive text result is therefore ownerless exactly when its
other returns are. A lambda invocation's result holds what the lambda's
returns hold in its own body; a lambda has no relay contract, so a returned
formal or capture there is unknown text. Semantics rejects every escape of a
capture's text (`L-OWN-02`), so such a result reaches only local uses, which
need no proof. An invocation through a GTI function
pointer holds what one of its targets returns. A virtual call to a root
without a result dependency holds ownerless text: each override keeps its
root's summary, which the dispatch table verification requires, and each
source override's returns are proven ownerless as any function's are under
that summary. A pointer value only comes
from a forming site naming one stateless function or lambda, so the
program's forming sites of the pointer's type are every target it can reach;
a target's returned formal or field is unknown text there. This covers
`std::function_ptr`, `std::function_ref` and any callable built on them.

Text read through a reference that a source call returned is summarized the
same way: the callee's Return loans name the referent, and its sources are
mapped through the call. A place rooted at a value, which is how a reference
pattern binds a payload, holds that value's text, so a payload accessor such
as `expected::error()` reads the extracted payload. A method that returns a
reference to its receiver's direct text field lowers it as a plain borrow of
that field. The text is the caller's, ownerless or under the method's field
contract, so the borrow needs no stored dependency in the body. Over a
receiver that carries a `Stored` loan designating that field, a view copied
through the reference carries the call's result loan, whose stored
dependency is that owner loan. MIR derives that the view holds the owner's
text and requires the binding to carry a loan that retains it, as for a view
copied from a field read (see the derived-carrier rule below). A method that
returns another call's text reference
publishes a `Return` loan that reborrows the call's result loan.
`TextContracts` follows that reborrow to the call, and the verifier's carrier
shell rule takes the expected stored dependency exactly from the forwarded
result loan, which is none when the callee's receiver holds ownerless text.

A construction may instead publish dynamic text (`L-OWN-02`, phase 1). Its
`dependentTextPublication` flag marks borrow facts that belong to the call: the
target constructor's own summary stays empty, because its other call sites
store ownerless text. The call has one `BorrowedFieldOrigin` from an argument
to a direct text field of a class without borrowed state. Its result carries a
read-only `Stored` loan that designates exactly that field. The
constructor-summary match accepts the call, and the text contracts waive that
argument's ownerless requirement, only when the constructor's sole text
initializer of the field reads exactly that formal and no other text
initializer does. The constructor body must not also store the formal as
ownerless text. Inside such a constructor, `TextContracts` reads a
publishable field back through `this` as that formal's text. A read in
another initializer makes the formal non-exclusive, and a read reaching
ownerless storage from the body fails the publication.

In a function that can carry a field-level contract (`fieldContractFunction`:
a non-generic source function or method, or a method of a generic class's
instance), `TextContracts` independently derives `ownerlessTextFields`:
- a formal's or the receiver's text-field projection is a field source, not
  ownerless text;
- so is a text payload extracted from a formal's enum field reached through
  that formal's accessor: a statically dispatched, non-virtual source call
  whose every return borrows `this.field`, or another such accessor's result
  on its own receiver (`accessorResultFields`, read from the callee's
  verified body);
- a use of the reference such an accessor returns, other than a payload
  extraction, uses each field its returns name, and so does a payload
  extraction when they name several (`accessorOperandFields`); an accessor's
  own returned borrows are its designation (`accessorReturnLoans`);
- field sources that reach ownerless storage or a `None`-summary text return
  become requirements;
- each other use of the whole formal or receiver requires all of its text
  fields, except a `CallInput` that a call consumes as a same-class formal or
  receiver of an in-scope callee, which inherits that callee's requirements.
  An accessor's returned reference is not a whole use of its receiver.
  The consuming call may be in a later block than its input, after a checked
  argument such as `n - 1`;
- a fixpoint shared with the text-formal requirements closes forwarding.

The derived set must be contained in the declared contract. A declared
requirement MIR does not derive only rejects more relays. A `CallInput` whose
operand is a dependent carrier must feed an in-scope callee that requires
none of its designated fields.

A payload construction that publishes dynamic text (`dependentTextPublication`
on its `PayloadConstruct`) is staged like a borrowed-field construction. Its
own read-only `Stored` loan names the text argument's owner and designates
the payload symbol, with no variant condition.
- `dependentTextPayloadPublication` admits it in the variant-table check.
  The result value has the enum's traits as a dependent value has them, and
  it carries no complete dependency set.
- `TextContracts` exempts the published operand from the ownerless payload
  requirement. A `PayloadExtract` of a text payload is ownerless unless
  `dependentDesignation` finds the subject's loan designating that payload
  (`directTextPayload`).
- A non-consuming switch's by-value binding of that payload names the
  subject's loan as its retained loan. Lowering maps it to the active MIR
  `Stored` loan after the binding's `Initialize` and records the binding as a
  carrier, so a local copied from the binding retains the owner as a copy of a
  published field read does.

Phase 2b extends designations to paths (`TextFieldPath`): a constructor may
publish at `[field, payload]`, a text payload of one of its payload-enum
fields.
- `initializerTextInputs` reads a payload-enum field initializer whose
  argument is a `PayloadConstruct`: each text operand's sources are
  requirements, and a formal that alone fills one payload is a published input
  at that path. The input is not exclusive when the body reads `this` or that
  field back (`readsPlace`). The initializer's own payload construction is
  then accounted by the initializer (`fieldInitializerValue`), not by the
  ownerless payload rule. `publishesText` and `dependentTextFieldFormal` match
  the call's loan path against it, and `directTextPath` checks the path's
  types.
- `dependentDesignationPath` finds a carrier's designation within an operand:
  from the carrier binding, from a field projection of it that prefixes the
  path (`switch (line.word)`), or from the producing construction.
- In a field-contract function, a `PayloadExtract` of a text payload from a
  contract formal's or the receiver's payload-enum field is that formal's
  payload source (`contractPayloadExtraction`), so stores require it and
  reference returns relay it. A whole use requires each text field and each
  text payload of its payload-enum fields (`formalTextFields`), and
  `heldFieldSources` maps a payload source onto the caller's argument.
- A loan that a call returning a reference to text produced is not a whole
  use of the formal it is rooted at, and neither is the receiver it borrows
  for that call: readers resolve the text through `returnSources`, so a
  method composing another accessor's reference relays what it relays.

Phase 2c adds re-rooting constructions and dependent returns:
- A construction that re-roots a class argument's text field has one
  `borrowFields` mapping with one `sourceFields` entry. The verifier's
  `dependentTextPublication` admits it when `dependentTextFieldRelay`, read
  from the constructor's initializers, maps that parameter's field to exactly
  the mapping's `resultFields`. In `TextContracts`, a dependent CallInput into
  such a construction is re-rooted (`publishesFieldRelay`), and a re-rooted
  formal field must reach no ownerless text storage in the constructor
  (`constructorBodyFieldNeeds`). `contractPlace` treats a constructor's
  by-value class formal as a contract place. `dependentDesignationPath`
  follows a value that a Move or Load produced, so a moved dependent argument
  is checked too; before 0.468.0 it skipped the relay check.
- A consuming receiver is a contract formal (`readOnlyContractFormal`). A
  consuming call whose result is a text field result of its receiver is
  exempt from the consuming-call borrow rule.
- In a field-contract function, an instruction other than a `PayloadExtract`
  that names a contract formal's payload-enum field place (a copy, move or
  borrow of the field) requires every text payload of that field. A switch
  observes its subject in place, in its terminator.
- `dependentTextReturnSummary` admits a function flagged
  `dependentTextReturn`: one Argument mapping over a read-only reference
  parameter, with no source fields, at a direct text path, or at a payload of
  an enum result. It replaces the complete-layout check for that summary. A
  return that is a Construct or Call without a loan and without publication
  is an ownerless alternative. Clearing the flag or the path fails
  verification.
- HIR gives a publishing contextual construction dependent traits, and its
  verifier requires the construction and its value node to agree. A by-value
  payload binding that semantics gives a retained loan carries that loan in
  MIR (`bindingLoans`).

Phase 3 verifies nested paths:
- `directTextPath` walks class fields and payload-enum payloads to text at
  any depth. `TextContracts` records a nested text class formal stored whole
  (relay key `(parameter, 0)`) or a class field of one, read directly or
  through an accessor whose every return moves that receiver field
  (`classFieldSource`). `publishesFieldRelay` and the verifier's
  `dependentTextRelayPath` accept a designated path through either, and the
  relay's own producers are exempt from the constructor's whole-use check.
- A call publishing a dependent class argument that its callee returns whole
  (`publishesWholeCallText`, `dependentTextCallPublication`) nests the path
  under the published field; returned fresh constructions derive those relays
  for class formals.
- `classReferenceRelay` derives the receiver payload that a reference to a
  nested text class designates, through payload reference bindings and
  composed accessors. Text read under such a call's result resolves to the
  origin's prefix (`classReferenceSources`), and a whole use of the result
  requires it. A relay of a nested path is contracted only through a payload
  prefix.
- A reference to text that an accessor returned (a `CallResult` loan whose
  operand is `string_view&` or `c_string&`) designates no path:
  `dependentDesignationPath` would otherwise read the path of the accessor's
  owner, the loan's source, and check it against the view. A view has no
  fields to relay; its owner is the result loan's stored dependency, which
  the carrier-shell rules verify.

A text field result (`textFieldResult`: one `returnBorrowFields` mapping of an
Argument or Receiver origin to one source field, no result fields) carries the
dependency the caller's value holds in that field:
- In the callee no loan derives the result from the origin. Lowering seeds no
  return-origin loan on a by-value formal; a reference formal keeps its
  ordinary entry loan, ended at the return. A return is the exact stored-field
  load, another field result's call, or a by-value text relay of the field
  such as `relay(label.text)`. `TextContracts` requires every return to read
  exactly the declared field or ownerless text.
- A by-value text call relaying a formal's or the receiver's text field
  (`L-OWN-02`) keeps its result loan on that field place, never on the whole
  referent the formal's entry loan names (`formalTextFieldPlace`), so the
  relay is a use of that one field, which the field contract tracks, and its
  return publishes exactly the declared source field. Program verification
  admits a call-result loan that names a text field of the selected argument
  source for a by-value text result.
- At a call, `TextContracts` analyzes the callee under its field contracts and
  maps each returned field source onto the argument or receiver:
  `heldFieldSources` gives that formal's field here, unknown text for a
  dependent carrier designating the field, or ownerless text.
- Lowering creates a result loan only when the origin's retained `Stored` loan
  designates the field. A call without one is valid when its origin holds no
  dependency at that field, or is a formal or the receiver of the enclosing
  function.
- A temporary origin is an SSA argument, not a place, so lowering takes its
  loan from `loanForValue`: the `Stored` loan its publishing construction
  produced, the loan `borrowOriginPlace` already selected for the mapping.
  That loan has no carrier and ends with the full expression; the result's
  own loan names the owner.
- A dependent temporary may be the receiver of a text field result. The
  temporary-receiver check admits that target over a publishing construction
  or call in place of an ephemeral-borrow result.

A function's `publishedTextFields` (phase 2a) are formals its returned fresh
value holds in a direct text field. `TextContracts` derives them
independently, in the same fixpoint as requirements, and the derived set must
equal the declared one:
- A Construct of the function's return type whose result a Return yields
  relays the formal that `exactTextFormal` finds at a published constructor
  input: a call stage over an exact load or move of the formal's binding. It
  derives no requirement for that input.
- A Call whose callee relays an input publishes a dynamic argument under its
  own read-only `Stored` loan designating the field (`publishesCallText`). A
  returned call relays this function's exact formal, and any other text there
  needs an ownerless proof.
- A call semantics marks `provisionalTextRelays` (recursion) requires every
  text argument ownerless and relays nothing.
The verifier admits a publishing call's borrow facts against its target's
`publishedTextFields` (`dependentTextCallPublication`), as it admits a
publishing construction against its constructor.

`isOwnerlessTextStorageProjection` excludes a direct text-field
place whose object carries a `Stored` loan designating the field: a carrier
binding of that loan, or the construction's own result value. A write into
such a field still requires ownerless text. A temporary's field view, whose
value has no unprojected place, takes its record type from that value.

A text field a base class declares is a field of each derived value, which
`isOwnerlessTextStorageProjection` finds through the instance's
non-interface bases. A class with a base keeps its text fields ownerless.
`initializerTextInputs` gives a constructor each requirement of the base
constructor its base initializer passes a formal to, settled over every base
chain, and `scan` applies a body's `InitializeBase` operands to the base
constructor's requirements as it does a `Construct`'s.

That exclusion, `dependentDesignationPath` and the relay-result check read
carrier lists, so a list must not omit a binding that holds a published
owner's text. `TextContracts` derives those bindings in every body
(`verifyDependentTextCarriers`) from the publications, never from carriers:
- A publication is a read-only `Stored` loan that a value-producing
  instruction designates at a direct text field or payload path of the value's
  type (`textPublicationLoan`): a publishing construction, payload
  construction or call. Its value holds the loan's owner text at that path.
- `Load`, `Move`, `CallInput`, `Initialize`, `Assign`, `Identity`,
  `std::move`, block parameters and hidden temporaries pass what they read
  on. A field projection keeps what is designated within the field, so
  reading the whole designated path yields a view of the owner. A
  `PayloadExtract` of the designated payload binds such a view, also through
  a payload-enum field.
- A source call's text result holds what the callee's body-derived returns
  (`returnSources`) map through its arguments and receiver: a returned text
  formal, or a formal's or the receiver's returned field. A call returning a
  reference to text records that for its loan's referent, and a `Borrow` of a
  holding place records the place's texts. A loan's referent also holds the
  text of each publication its stored-dependency chain reaches, so a view
  copied through a returned reference holds it.
- Holding is typed. A view, or a reference to one, holds owner text only
  as a whole, and a class or payload-enum value only at a designated path.
  An `int` read through a dependent value's `int&` accessor holds none,
  although that accessor's loan has the publication as its stored
  dependency.
- Each local binding that holds a publication's text must carry a loan that
  retains it: the publication's loan, a loan whose stored-dependency chain
  reaches it, or a relay call's own read-only `Stored` loan on the same owner
  place, which carries a relayed temporary's text past its full expression.
  Another loan on the owner does not retain a publication. A reference result
  observed within its full expression, which no binding holds, is not
  covered.

The rule changes no lowering. It makes the carrier lists that semantics
records evidence MIR checks, rather than a semantic decision MIR trusts.

Plain and fallible reference returns share the exact runtime-alias selection
for a reference parameter's entry loan. If a local alias stores an accessor's
different address, the Return operand names that alias's dereference place,
not the entry parameter's address. Verification follows its unique dominating
reference initialization, including a checked reference extraction from a
fallible carrier whose external owner matches the parameter. Matching types or
carrier membership alone do not establish the address. Parameter publication
of a referent cannot fall back to the weaker complete-carrier transfer rule;
forwarding an entire carrier still preserves its existing runtime value.
Direct field addresses use the same proof, including fields reached through a
previously initialized reference alias. Each alias initialization must dominate
its consumer; its declared reference referent is checked separately from any
subsequent field's type. Stable field-only paths preserve the exact formal and
field identities without admitting raw or opaque paths. The final runtime place
also retains the return expression's source-value identity: another same-typed
alias of the same protected owner cannot replace the selected address merely
because it would have a valid lifetime.
The MIR verifier and backend admission share this publication query, including
the dominating initializer chain and immediate entry-loan retirement; the
backend cannot accept a weaker carrier-membership approximation of that proof.

Checked reference extraction verifies the external owner through either a
named carrier or its fresh SSA producer, following explicit copy/move
transport. A cleanup-free fresh carrier can use its native SSA representation;
the checked extraction publishes its pointer into the CallResult loan. Scalar
and nominal observations read that pointer rather than an unassigned parallel
SSA object. Wrong-state failure still precedes any referent access, with normal
Invoke and deterministic cleanup. No referent is copied or consumed by these
reference publications.

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

Nested plain stores retain one terminal two-phase loan. Loan-flow verification
derives its reservation group from the exact unretained mutable `CallResult`
parent chain; it never infers a group from public container names. Intermediate
loans enter the reserved flow state together, leaving a lexical mutable parent
active. Terminal activation checks the entire group, suspends its ancestors,
and must still be followed immediately by the terminal assignment. An
unactivated failure restores the original exclusive ancestry before ordinary
child-first cleanup. A live sibling, retained carrier, escape, stored
dependency or ambiguous authority prevents this conversion. Every origin and
exact physical referent remains represented by the existing loan records.
Eligibility follows that ancestry to its canonical root; a global cannot gain
reservation access merely by being hidden behind an intermediate loan place.

HIR preserves a separately selected dereference/subscript projection operator
even when the mutation itself is built-in scalar arithmetic. MIR stages that
accessor result as one loan-rooted place and uses the ordinary ordered
load/arithmetic/conversion/assign schedule. The propagation of the target call
belongs to the call; it does not force the scalar update back into an opaque
compound helper or make a backend manufacture the target from syntax.

Checked fixed-array reference formation retains its semantic bounds origin on
the `Borrow` itself, not on an omitted scalar load or an eventual return. It
has an exact `Invoke` and failure record before reference publication. A failed
producer retires its provisional loan before receiver ancestry and callee
locals drain; successful reference return publishes the already-checked
pointer. The backend uses the existing typed-place bound-status operation and
the same success/failure routing as checked address formation. It does not
call a terminal aborting array helper to perform an unproven reference return.
Scalar and class referents use the same rule, preserving `GTI-R0007`, status
70 and deterministic cleanup in the caller as well as the accessor.

A fixed-array projection used as a later index's base completes its bounds
check before that next index is evaluated. Place-dependency lowering uses an
ordinary typed `Compute/AddressOf` stage with the prefix's exact semantic
failure origin and `Invoke`; it does not load or copy the selected subarray.
The synthetic pointer has a distinct SSA identity and never replaces the
source index's value identity. This applies through grouping, comma and field
projections to reads, writes, reference binding and explicit address formation.
The eventual access retains its own check; an earlier prefix failure prevents
all later index side effects and reports the prefix's source location.
Element receivers and direct reference/copy arguments also finish their terminal
selection before invocation or later arguments. Their address-only transport
must not erase the bounds check merely because no scalar load is emitted.
The constant evaluator validates an unused safe checked-address projection
against its existing constant aggregate place. It publishes no pointer and
does not admit used or raw-memory addresses into the constant profile.

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
Ahead of that operation-by-operation proof, a structural proof covers any
source GTI function that is not virtual, external or a program entry.
Verified MIR records every defined-failure source on the instruction that can
raise it and routes it through an explicit failure edge, so a body is
failure-free when nothing reachable from its entry over ordinary and
Invoke-success edges carries a failure source, other than an exact static call
to a target the same derivation proves failure-free. The body's parameter,
receiver and result types and its other operations do not matter. A live block
entered with a failure parameter or an active failure, and a terminator that
propagates, contains, exits or terminates on failure, keep the body
conservative. A call to a body still being proved is not admitted, so
recursion stays conservative. A proved call's failure route cannot execute;
the next lowering round lowers the call with `FailurePropagationKind::None`
and no Invoke edge.
Optimizer-produced literals remain classified by their retained source
operation, so folding cannot silently strengthen this effect summary. Its
destructor component can prove `false` for bounded source bodies
over the admitted scalar/class operations. A second closed proof admits exact
source constructors whose base-free passive-scalar class has one verified
initializer stage per declared field, their matching source destructors, and
free-function graphs over those scalar/class values. Both proofs admit a
`Move` of a trivially dropped, borrow-free passive value, such as
`int32_t y = std::move(x);` or a constructor initializer `tag(std::move(tag))`:
it carries no lifecycle event and no failure origin, its operand moves a place
the proof already admits, and it copies the value while leaving that place
unavailable, exactly as failure-free as a `Load`. Lowering records the move's
HIR ownership event again on the records that consume that HIR value, such as
its call input and full-expression boundary; the function proof admits such a
record only when its event equals the event of the exact `Move` it repeats.
The constructor proof admits a moved formal's owned-parameter initializer
when that formal's type is passive. A class move still needs its lifecycle
events, and a real failure source beside a move keeps the body conservative.
Its passive values are program-aware (RT-12): scalars, integral enums, fixed
arrays of passive values, payload enums whose every alternative payload is
passive and needs no active cleanup, and records with generated special
members, no destructor, no borrowed state and only passive fields. A
constructor whose class holds only such fields may take such formals, build
an enum constant or a payload construction whose operands match the selected
alternative, keep passive drop obligations (no destructor, no active cleanup,
no active child), move a passive input into its prepared call slot, and make
a consuming (`Once`) call of an already proved non-virtual member of a
passive record whose receiver is that slot, provided the call retires exactly
the slot's obligation. So `Slot(int32_t n) : state(state_t::ready(n))`, the
prelude's `expected(T value)`, `expected()` and
`expected(unexpected_value<E>)` for passive `T` and `E`, and
`unexpected_value<E>(E)` for an integral enum `E` are failure-free, and the
structural function proof then lifts result builders over them, such as
`expected<int32_t, E>`, `expected<void, int32_t>` and
`expected<Record, int32_t>` returns. A payload whose alternative needs active
cleanup, a checked operation, a non-consuming or lifecycle-free receiver call,
or a borrowed or actively cleaned value keeps the constructor conservative.
This is only the defined-
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
gate between that canonical pre-optimization program and production MIR. Its
original version-19 gate admitted only an in-place identity fold. The current
gate independently replays typed records for local/SCCP constants, branches,
scalar promotion, safety discharges, exact receiver dispatch, dead computations
and unreachable blocks, repairs the derived indexes, and exact-compares the
whole program. Arbitrary CFG, target, lifecycle, layout or header mutation is
still unauthorized. This comparison is structural rather than based on
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
out. Verification and backend admission require one consumer,
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

Within one body-verification invocation, cleanup-spine checks share a
precomputed predecessor-block index. The CFG is already shape-validated and
immutable during that invocation. Repeated edges from one source block count
as one predecessor, while any second source block, including an unreachable
one, still prevents a unique cleanup spine. The exact Invoke success edge,
active failure identity, reverse drop order and boundary-marker checks remain
unchanged; this index avoids whole-body rescans at each fallible cleanup step.

A `MirBody` owns:

- basic blocks with `goto`, branch, switch, return, unreachable, exit, invoke,
  failure propagation/containment, or cleanup-failure termination terminators;
- one concrete `PlaceDomain` copied from HIR;
- typed values with one defining instruction and indexed uses;
- places rooted in bindings, symbols, `this`, temporaries, values, or loans,
  with field/index/dereference projections and, where semantics supplied one,
  the corresponding value-owned `PlaceKey`;
- a field projection from a class-valued SSA temporary is rooted in that
  producer value, never in the projected load result. The producer dominates
  the place-root use and the following `Load`, so verification cannot admit a
  self-use-before-definition cycle. Place formation materializes the producer
  first: a call, construction, direct initializer, propagation, conditional,
  or a `Move` out of a place. `std::move(a).x` therefore moves `a` into a
  temporary (leaving `a` moved-from) and reads the field of that temporary;
  an owning class arms the temporary's full-expression obligation with the
  Move's lifecycle event. A built-in class assignment is the producer of its
  own value: an explicit copy `Load` of the committed destination placed
  directly after the `Assign`, before any later operand runs and before a
  selected accessor's store loan ends, while the `Assign`'s own result stays
  expression bookkeeping. `(p = q).x` therefore reads that copy, which a later
  write of `p` in the same full-expression cannot change. Only a copyable
  class without borrowed state or active cleanup has such a copy; lowering
  refuses a projection of any other assignment result rather than aliasing the
  destination. The copy `Load` records its own `Read` of the destination. The
  whole result is the same copy: an initialization, return or constructor field
  initializer consumes the `Load` (a constructor initializer, which reads its
  arguments after evaluating them all, takes the copy at the commit). When HIR
  planned a value obligation for the result, as for a temporary method receiver,
  the copy initializes a hidden temporary that carries the obligation, as a
  value conditional's join does, and the result moves out of it. A by-value
  argument, which HIR stages as `CopyValue`, copies a place carrying the
  destination's key directly after the `Assign` instead;
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
  it exactly once. When fixed-array place selection has already completed at
  an exact checked `AddressOf` preflight, the following copy/borrow checkpoint
  does not duplicate that selection's local failure metadata. The preflight
  retains the source HIR identity, exact place, bounds origin and assigned
  failure site, and its successful `Invoke` edge precedes publication. Other
  origin and propagation families remain on the operation that owns them.
  Scheduled ordinary constructors use this same path:
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
- each borrowed class element of a concrete formal pack has a `StoredValue`
  entry loan over its exact `PackElement` place, with all resolved field paths
  and access modes. Projecting its referents appends the ordinary field and
  dereference projections to that element, not to the whole pack. Forwarding
  reuses the canonical element place and its loan; copying stages an independent
  carrier while moving consumes only the carrier shell. Whole-program checks
  independently replay field paths and reject missing or substituted origins;
- typed lexical/value drop obligations and per-instruction initialize, move,
  reparent, replace, transfer-out, and drop lifecycle events;
- resolved call targets, static/virtual dispatch, constructor targets,
  intrinsic identity, C linkage, and external symbols;
- ordinary member calls retain a receiver exactly when their selected target
  is nonstatic. Whole-program verification checks its typed value or place,
  staged access mode, and exact owner/base relation; a same-signature method
  on an unrelated class is not a valid substitute. Verified exact-interface
  receiver presentations retain their existing dispatch proof, while raw
  member access retains its explicit unsafe pointer contract. Owning argument
  transfer is separate from the receiver borrow;
- source function, constructor and lambda by-value formal roots retain their
  exact flattened parameter roles and concrete copy/move facts. Verification
  resolves nominal facts from the class/enum/lambda catalog and follows inline
  aggregate alternatives rather than trusting independently editable binding
  flags; references and pack roots preserve their separate carrier contracts;
- concrete aggregate trait queries give a unique owning handle only its exact
  pointee's transfer/share policy, without inheriting the pointee's copy, move,
  drop or borrowed-state traits. Pack and scoped-task
  verification consume those same published nominal facts;
- ordinary staged calls for a concrete bounded call fold, with exact
  read-only `PackElement` places. HIR retains the source fold and selected
  expansion; executable MIR contains no `MirOperation::PackFold`. Earlier
  expression-inventory descriptions of that operation are historical, not a
  second execution path. An empty expansion emits no input evaluation or call;
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
  transfer, or active-drop result.
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
  edges. A fresh borrowing closure passed to such a parameter (`std::for_each`
  returning its visitor) carries its capture group out under the call's own
  result loan: the result must be that closure type, borrowed by value from
  exactly that argument, as the callee's return borrow summary states. An owned
  parameter returned by value leaves the same way, whether its transport is
  exact-return or an owned value: owned-value transport may carry borrowed
  state only when that return summary names the parameter, and a mutable owned
  parameter may use exact-return transport. The callee must either return that exact moved binding as a single-use
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
  `TerminateCleanupFailure`. Production backend emission consumes this verified
  schedule, including admitted partial-construction rollback. New unsupported
  startup shapes must extend its construction and containment proof, not an
  older source-tree execution path. The argument loop uses ordinary `Load`,
  `Compute(Add)` and `Assign` instructions rather than a fused `Modify`.
  `ComputeNextArgumentIndex` and `AdvanceArgumentIndex` bind arithmetic and
  commit separately. The exact verified loop edge establishes
  `0 <= index < count <= INT32_MAX`, so adding one cannot overflow; the verifier
  rejects changes to the step, direction, source value or commit identity;
- lexical scopes, cleanup edges, loans, carrier bindings, and source/HIR
  provenance.

IDs are body-local and start at one.

## Lowering And Verification

Thread-local objects have a separate `MirThreadLocalStorage` inventory and
`ThreadLocalInitializer` bodies. `EnsureThreadLocal` names the exact declaration
and initializer; independent verification checks inventory, passive data
provenance, cleanup recipes and dominating first-use guards. `DrainThreadLocals`
is an explicit hosted-exit operation, not an emitter-added source call. Passive
data storage uses native per-thread literal-initialized data without dynamic guard or
registration; owning storage uses a byte slot and explicit object lifetime so
emergency abandonment cannot accidentally run a native destructor.

Process storage with native-only cleanup retains an exact optional
`MirProgramInitializationStep::nativeRetirement` recipe. It must equal the
unique structural cleanup recipe for that root and contain no source
destructor, owning allocation release, borrowed state, polymorphic cleanup or
recursive cleanup. This authority composes through ordinary aggregates; it is
not a named-library exception. Process roots do not acquire lexical drops.
Publication follows the exact recorded final storage initializer, so failure
leaves only the successfully constructed prefix live.

`RetireProgramStorage` is a generated hosted-only operation with runtime,
unknown-write and drop effects, but no user-code or defined-failure effect.
Normal hosted teardown drains TLS, ends the C-thread runtime, then retires
native process roots in reverse initialization order. An ordinary failure
first finishes source cleanup, then drains TLS with the original primary,
ends the C-thread runtime only if begun, retires native roots and contains the
failure. Module-initialization failure uses that outer endpoint because TLS
cleanup may still access already-published globals. Cleanup emergency and
abort intentionally abandon further teardown. The closed program verifier
checks this exact schedule and C-thread target; body-local suffix acceptance
does not authorize an arbitrary call during failure cleanup.

LLVM uses one publication bit per root and a reverse-order helper, clearing
each bit before native cleanup. Hosted exit and the module unload destructor
share the idempotent helper. The helper first queries managed-worker retirement
readiness: terminal automatic-join quarantine pins all native roots and leaves
their publication bits intact. Explicit hosted failure can then contain its
original record without invalidating storage still accessible to that worker.
The unload wrapper rejects any remaining live roots after the helper, rather
than unmapping pinned storage. C-thread end requires the manual-worker registry
to be empty, which alone does not prove managed-worker quiescence. Embedding
shutdown must reject pinned storage; unsafe hosts must independently ensure no
foreign execution can access unloading storage. This does not enable general source global
destructors or change embedding activation/context lifetime rules.

`mirOwnedStoragePayloadInitialization` proves the existing compiler-private
opaque-storage constructor mechanism: one exact owned parameter becomes its
payload, followed only by that parameter's moved-from cleanup. It requires the
trusted capability identity, concrete parameter/place, unique active cleanup
recipe and independently valid cleanup-only body. Extra source code, borrowed
state, wrong recipes or inactive input cleanup reject. The query does not
weaken verification, change declared effects or select failure transport.
Conservative destructor effects remain conservative even though this particular
parameter cleanup observes moved-from representation state. LLVM consumes the
witness and uses structural fresh-shell transfer when the payload itself
contains native mutexes, including selected owning-sum alternatives.

Ambient persistent-storage effects are independently derived from executable
MIR, not trusted frontend summary claims. `mir_storage_effects.cpp` reuses the
existing canonical-place/CFG loan verifier to collect direct typed storage
accesses, closes exact helper/constructor/destructor edges, and replays hidden
accesses at each invocation's live-loan state. Initializer edges retain their
readiness condition through nested helpers. Captured referents have a distinct
canonical capture domain from same-thread TLS lookup. Native/raw-pointer
obligations remain the unsafe boundary; this is not a pointer-provenance solver.
An exact virtual effect edge requires an independently proved complete owning
receiver: an unprojected declared storage object, a by-value formal, or a local
with typed owning initialization. Consumer place types are insufficient evidence;
formal and persistent declarations must agree, and a reference-valued producer
cannot be relabeled as owning publication. The receiver must retain its exact
call-input stage and access mode, and every virtual root must select the same
source function and concrete instance as the invocation. Missing, ambiguous,
projected, aliased or construction-phase receiver evidence retains an unknown
boundary. This closes effects only; dispatch, failure edges, cleanup and argument
scheduling stay unchanged, and a readonly receiver does not imply a pure callee.
For an exact persistent-storage reference input, the independently verified
`CallInput` checkpoint also starts protection before later inputs execute.
Its unique consuming invocation (or an interrupted invocation's terminal
boundary) ends that explicit scheduling interval. The verifier walks that
interval through the existing CFG and uses the same canonical overlap rules,
including hidden callee effects; it does not infer another NLL lifetime or
invent a temporary borrow that an otherwise legal returned reference would
need to outlive. Already-loaned inputs retain their ordinary loan proof.

Every type-owned static function retains an exact owner but no instance receiver:
it cannot carry virtual/pure/override flags, virtual roots, mutable/consuming
receiver access or a `This`-root place. This is a structural invariant for all
static methods, including interface factories, not a public-name exception.
LoweredProgram verifies that declaration metadata agrees with the MIR instance;
backends cannot infer a function's purity from its enclosing type kind.

For each body, MIR lowering creates an entry block and root scope, seeds
parameter cleanup, lowers prologue/construction values, lowers statements,
synthesizes a legal terminal edge, rebuilds reachability and value uses, then
verifies structure.

Verification runs on every body several times per compilation, so its checks
index a body once rather than rescanning it for each instruction, loan or
failure record they examine: block predecessors, full-expression roots and
the values and open instructions that realize them, prepared call-argument
sites by staged value, blocks by active failure record, drop obligations by
full expression, loan producers and ends, and failure-origin assignments,
which verified failure metadata keeps strictly ordered by source range. Each
index lists candidates in the order the scan would visit them, so the first
reported violation is unchanged. The lifecycle dataflow still carries one
state per drop obligation into every block; a sparse representation remains
future work for very long bodies.

### Shared failure cleanup

Function and lambda bodies share failure cleanup across failure edges. An
edge's cleanup is a sequence of steps: drops in reverse construction order,
loan ends and cleanup-boundary markers. Many edges end in the same steps. A
failing call and the failing destructor of the next owner both release the
same remaining owners. Lowering records each edge's steps as a plan while it
lowers the body. Once the body is complete it interns every plan by identical
suffix and emits each distinct suffix once. Cleanup then grows with failure
entries plus distinct cleanup transitions instead of failure sites times live
owners. The `stdlib_surface_json` fixture's `main`, for example, went from
61,225 blocks and 29,941 failure drops to 3,362 blocks and 145 drops.

**Representation.**
- A shared suffix is a *continuation* failure record
  (`MirFailureRecordKind::Continuation`). It has no producer and one parameter
  block, its entry, which binds the continuation as both failure parameter and
  active failure. The suffix's blocks carry the continuation as their active
  failure.
- A chain enters a continuation only through a `ContinueFailure` terminator,
  which names the continuation and its entry.
- Producer records keep their exact provenance: their Invoke, their parameter
  block, and the failure-record write the producer performs before reaching
  that parameter block.
- A continuation carries whichever producer's failure entered it to its
  endpoint unchanged, so a continuation's `PropagateFailure` propagates the
  original failure.
- A fallible drop in a continuation routes its own cleanup secondary as in any
  chain. The secondary is a producer record whose primary is the
  continuation, and it terminates at once.

**Verified contract.**
- Each record's own chain, from its parameter block, is linear. It follows
  ordinary edges and the normal edges of fallible cleanup drops, and has one
  endpoint: its `PropagateFailure`, or one `ContinueFailure`. Every block of
  the chain carries that record as active failure. Each record's chain is
  walked once, so a shared suffix is never walked again for every failure
  that enters it.
- `ContinueFailure` leaves primary cleanup only, never a secondary's
  termination block. It enters exactly the named continuation's entry, and it
  is the only edge that changes the active record, apart from an Invoke's
  failure edge. A continuation has no Invoke, one parameter block, at least
  one continuation edge and one endpoint.
- Continuations form an acyclic graph. Drops along any complete cleanup path
  stay in strictly reverse construction order across continuation edges. This
  is checked per edge against each continuation's memoized first drop ahead.
- The lifecycle dataflow joins drop-obligation states at a continuation entry
  and validates each shared drop against the joined state. A drop that an
  entering path already performed, or one whose conditionality differs, is
  rejected.
- Loan flow state must be equal on every edge into a block. A suffix is
  therefore shared only between failure edges whose loans left live after
  cleanup agree.
- Hosted-startup bodies reject continuations.

Backends emit `ContinueFailure` as a jump. The incoming failure is already in
the invocation's single runtime failure record, and the cleanup context
activated by the producer's parameter block spans the shared suffix.

**Lowering rules.** A step is shared only when its meaning does not depend on
the path that reaches it:
- A drop's failure propagation must equal its destructor's own summary. A
  drop proved to retire a moved-from shell has no destructor on that path,
  but the proof walks back to its producer, so the drop stays in its edge's
  private prefix.
- A prepared call argument's drop stays private, because verification counts
  it along its producer's own cleanup chain.
- A lifecycle-bound loan's end stays private, because verification justifies
  it by the drop or failed producer before it on the same path.

Every step before a private step on its edge stays private too, and sharing
begins after it. A plan that shares nothing is emitted exactly as an unshared
chain. Cleanup boundaries are closed at the start and end of each emitted run,
so no boundary spans a join.

**Unshared shapes.** These keep one cleanup chain per failure edge:
constructor, field-initializer, static-field-initializer, destructor and
module bodies; the generated hosted-startup body; and edges whose remaining
steps are all private. These are lowering scopes, not language restrictions.
Normal exits share their own cleanup separately; see
[shared normal-exit cleanup](#shared-normal-exit-cleanup).

**Fallback.** If a body's shared schedule ever fails body verification,
lowering emits that body with one chain per failure edge. If a program lowered
with shared cleanup fails program verification, the program is lowered again
unshared. The corpus differential in
[verification](verification.md#shared-failure-cleanup-0402-2) triggers
neither.

### Shared normal-exit cleanup

Function and lambda bodies also share the cleanup of their normal exits:
each `return`, each `?` propagation and a void body's end. An exit drops the
owners live at it, newest first, and ends the loans its scopes hold. A body
with an early return after each owner used to emit returns times owners
drops. Exits now share that work, and normal exit code grows with distinct
exit states. With the synthetic early return after each of 80 owners, the
body's normal drops fall from 3,320 to 80 and its blocks from 7,440 to 1,040.

**Representation.**
- `walkScopeExit` is the one definition of the scope-exit sequence. An
  ordinary exit emits it as it walks. A shared exit walks it twice: first over
  a copy of the scope state to plan, then again to emit, so the plan cannot
  diverge from the emitted steps.
- Each unit of a shared exit is either a scope's loan ends or one drop with
  the loan ends that follow it. It begins a block, a *rung*, registered under
  the exit's remaining state: the carrier, plus the places still to drop and
  the loans still to end in each scope with work left. A later exit that
  reaches a registered state jumps to that rung and stops. Exits therefore
  form ladders whose rungs every passing exit shares.
- The remaining state determines every step the walk still emits, including
  the planned failure cleanup of each fallible drop. Everything else that
  emission reads is fixed by the eligibility rules below. So an exit that
  enters a rung also shares the rung's Invoke, failure record and planned
  cleanup.
- A returned value travels in a `Temporary` *carrier*, one per value type. An
  exit initializes the carrier before its own full-expression cleanup, and the
  ladder's tail loads it and returns it.
- The tail serves every exit that reaches it. Its reload and `Return`
  therefore name no source location, like
  shared failure cleanup. Debug information maps them to generated code
  rather than to the first exit that emitted them. Each exit's carrier store
  keeps its own source location.

**Eligible exits.** An exit shares only when all of these hold:
- the body has at least two exits;
- no temporary, pending owning result, return loan or constructor-published
  loan is live when cleanup begins;
- it shares at least one drop;
- it returns nothing, or a value of the return type that has block-parameter
  type: a scalar, enumeration or pointer.

Owning, class and borrowed results keep their own cleanup, because the C++
backend publishes them through proofs over their exact producers. So do
`main`'s implicit zero return, single-exit bodies, and every body kind outside
function and lambda bodies. A drop is shared only when its failure
propagation is its destructor's own summary. A drop proved to retire a
moved-from shell depends on the path back to the move, so it and every unit
before it stay private to the exit. These are lowering scopes, not language
restrictions.

**Verified contract.** No new verifier rule is needed. Every joined path must
satisfy the existing ones:
- The lifecycle dataflow joins obligation states at a rung. Exits sharing a
  rung have the same drops left, so each obligation the rung drops is active
  on every entering path. An exit entering a rung it does not match makes a
  drop's conditionality disagree with its path, and is rejected.
- Loan flow must be equal on every edge into a block, and exits sharing a rung
  have the same loans left.
- Each rung closes its own cleanup boundary. A fallible drop's boundary is in
  its Invoke continuation, before the jump to the next rung, so no boundary
  spans a join.
- A carrier is read only as a definitely initialized scalar place, so an exit
  that skips its store is rejected.

**Fallback.** If a body that shares exits fails verification, lowering emits
that body again with every exit destroying its own owners. If the program
fails program verification, it is lowered again without shared exits, then
also without shared failure cleanup. Lowering counts each fallback
(`MirLowerer::cleanupSharingStatistics`). The corpus differential in
[verification](verification.md#shared-normal-exit-cleanup-04211) triggers
neither.

Program verification also requires each function, constructor, destructor and
lambda body to retain its enclosing instance's exact body kind and return
type. Constructors and destructors return `void`. A well-formed body-local
return operand cannot authorize changing the callable's published result
signature. Backend
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
identity remains available as provenance. General complete-object constants use
the selected native representation. A program-level `native-object-layout` fact
survives semantic overlays, staged constexpr evaluation and result reuse, HIR,
MIR optimization and serialization, including queries erased before body
lowering. The C++ backend rejects that fact instead of silently consuming native
aggregate constants. Primitive and other previously supported bounded queries
do not set it. No new constant-execution operation is needed: staged MIR evaluates
the same retained literal, so early array extents and assertions keep their
existing semantic timing.

Early queries use resolved fields, concrete substitutions, enum alternatives
and lifecycle representation witnesses. Stored references and owning
indirection do not recursively demand their referents. The layout dependency
walk also checks generic class assertions in isolated semantic instance
contexts, reusing ordinary assertion analysis and its existing constant engine.
An open instance owns its own assertions, so an assertion can query its class's
layout without recursively evaluating itself. No object initialization or
unrelated generic argument instantiation is needed. Nested assertion queries
share a bounded instantiation-work budget and stop expanding after the first
resource failure; a depth cap alone would not bound branching invalid assertions.
Staged workers share that budget, and resource-exhausted speculative results
cannot enter the successful constant-result cache.
Checked size/padding
arithmetic in `object_layout.h` is shared with native record-layout verification;
native execution tests compare constant results with `ObjectSize` and
`ObjectAlignment` over unrelated user objects and library owners. A selected
inactive-drop pointer witness omits the physical active-state byte without
changing the semantic cleanup obligation.

Runtime object representation queries
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
production emission and hosted containment consume that same schedule.

`MirClassInstance` carries the selected `[[c_abi]]` record size, alignment, and
ordered field offsets. Program verification checks that this metadata is
structurally complete and within the record bounds; deterministic printing
exposes it for audits. C ABI unions retain zero offsets and a union-layout view
derived from the same semantic computation; verification rejects disagreement
in size, alignment, field identities, types or field layouts between those
views. Existing verified union field places and load/store operations own
execution; ABI opt-in adds no new body operation or recursive emitter path.
Passive ABI result copies include unions inside source wrappers. When a call
result has a verified placement slot, publication uses that slot even when the
result is also a passive ABI value; raw value assignment and placement-factory
publication are mutually exclusive emission choices.
MIR does not contain a native-layout instruction.

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
active state. A switch or extraction over a named place read-borrows the owning
enum place, and its extracted fields are immutable copies, so move-only fields
admit tag-only matching or a borrowed binding there. The borrow applies to an
enum that owns cleanup, to one whose folded traits forbid copying, and to one
an arm binds by reference. A cleanup-free alternative can still hold a
move-only field, which makes the enum move-only without giving it cleanup.
Only a copyable passive enum is read through a copied subject value.
Program verification rejects any `Copy` operand, in an instruction or a
terminator, that names a payload enum whose folded traits forbid copying, so
such an enum is only ever borrowed or moved.

The read-borrow transports only the place's address, so when that place selects
a fixed-array element no `Load` performs the bounds check. Subject lowering
therefore finishes each selection at its checked `AddressOf` prefix, with the
exact semantic bounds origin and `Invoke`, before the `Switch`, as for an
address-only call argument. Program verification requires, for every `Index` in
a read-borrowed switch subject's place, that the successful edge of the checked
`AddressOf` over the place prefix ending at that index dominates the reachable
switch.

A field projection performs no bounds check either, so a field read or store
below an element selected at a run-time index, as in `points[i].x` or
`points[i].x = 7`, finishes that selection at its checked `AddressOf` before
the access, and the field `Load` or `Assign` carries no second bounds origin. A
whole class element copy (`Point copy = points[i];`) uses the same prefix,
because its consumer constructs the copy from the element place; a scalar
element `Load` keeps its own checked read. A constant index keeps its static
bound proof and no run-time check. Program verification requires, for every
run-time `Index` that a field projection follows in an instruction's place,
that the successful edge of the checked `AddressOf` over the place prefix
ending at that index dominates the instruction.

A consuming switch first initializes its hidden subject binding from the
subject value, reparenting the subject's temporary obligation into the
binding's obligation, and registers that binding with the enclosing scope's
failure cleanup, so a failure while the subject's other temporaries end drops
it once. The `Switch` terminator then read-borrows the binding. Every arm retires it at entry, after its
entry-ended loans and before its first statement, and no scope drops it
again. A binding arm emits one `PayloadExtract` per field whose operand is a
`Move` of the subject binding place, each followed by its binding's
`Initialize`; the final extraction alone carries `TransferOut` of the
subject's obligation, so the drained enum is released without a second drop
of moved fields. Any other arm emits an ordinary binding `Drop` of the
subject. Program verification requires each consuming extraction to read a
projection-free binding of the exact enum type, to be entered only through the
matching variant edge of a switch over that same place, and to be preceded
only by `EndBorrow`, the arm's earlier extractions, and their binding
initializations. The arm's extractions cover every field of the variant
exactly once, no extraction follows the transfer, and the transfer appears
exactly when the subject owns a drop obligation; lifecycle flow then proves
the obligation is retired once on every path.
An observing switch over a carrier that stores several external references
selects each `auto&` reference with the carrier's Stored loan as parent. A
selection of the carrier's primary owner sources a place rooted at that loan.
A loan-rooted place always names the primary referent, so a selection of one
of the carrier's additional owners sources the owner place the carrier already
materialized for that dependency (the referent of a reference-typed owner).
Verification rejects a selection that claims the primary owner for another
field's referent.
A reference result names its referent through its call's loan, so an
observing switch over one read-borrows a place rooted at that loan. That loan
and the uncarried accessor results it reborrows outlive the subject's full
expression: the `Switch` and each arm's extractions read through them. Each
arm owns them, ending them at entry in a tag-only arm and after its payload
bindings otherwise, unless a reference binding sourced under them keeps them
until the arm's scope exit; the unmatched edge ends them before the exit. An
extraction is therefore not their consumer. The subject place takes its
overlap key from the owner its loans borrow, the place an arm's payload
borrow also protects. Ending such a loan before the `Switch` is a use after
the borrow ends.
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
when the lowering scope includes unreached compiler-trusted declarations; it
has no task adapter or executable use and public use remains
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
preserving the assignment operation and the exact call-result loan. A
checked-arrow member call is split the same way in HIR. A trusted builtin member
whose receiver semantics normalized into argument zero, such as a text-view
iterator factory, receives that access call's reference result as argument
zero rather than the written object. Reading an
owner handle does not read its pointee's moved fields; accessing those fields
or moving the whole owner still requires their availability.

Lifecycle events can prove a drop obligation definitely moved. Retiring that
shell cannot invoke the source destructor, so lowering omits its otherwise
conservative destructor-failure edge. When the drop's own block leaves the
obligation untouched, the proof follows invoke edges back to the obligation's
last event: from an invoke's success or failure continuation to the invoking
block. That covers a by-value parameter moved into a construction and retired
after it. Lowering creates each continuation with that single edge, so the
proof gives the same answer while the body is still being built. A block
entered any other way may later become a merge or loop header, so the proof
stops there, as it does at the entry, a cycle, or a conditional, tombstoned or
re-initialized move. An invoke's success lifecycle applies only on its success
edge, as in lifecycle verification. Lowering, program verification and the
backend share this one query, and verification replays it before accepting a
non-failing drop. The proof does not eliminate the shell's explicit drop.

The private unique-owner borrow intrinsics are local failure detectors, just
like checked storage reads. Both read-only and mutable access require one
`Invoke` edge before reference publication. The operation-specific verifier
checks the staged owner, pointee type, result loan/access, and one
`empty_owner_access` outcome with the semantic dereference/member-access
detail. Failure follows ordinary loan ending and reverse owner cleanup;
callers propagate the original record without re-siting it.

Scalar unique allocation likewise requires a local `allocation_failure` /
`unique_owner` detector, but no exhaustion `Invoke`. The operation either aborts
or completes and arms its owner obligation directly. Independent
verification checks its scalar pointee, zero/one exact initializer, result,
absence of constructor dispatch, and failure metadata. Named native callback
values are passive ordered by-value inputs, including expanded packs; an
ordinary callback parameter may also receive the already-admitted null value.
Native invocation uses the separate exact-signature call contract below; null
transport does not make invocation safe or admit foreign-thread entry.

Class unique allocation evaluates and prepares arguments first, then issues
`AllocateObject`. Its allocation outcome aborts without argument cleanup;
completion directly arms one `ObjectAllocation` obligation for unconstructed
storage. The immediately following allocation intrinsic call
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
rollback retains its existing structural admission. Targetless generated
copy/move and trivial generated default construction share this protocol;
independent verification checks the class's special-member policy and requires
a selected constructor schedule when default fields/bases need one.

`DefinedFailureOperation::mayUnwind()` distinguishes contained failures from
allocation-only terminal outcomes without erasing their canonical sites.
`mayAbortAllocation()` identifies detector-time reporting before publication.
Mixed-outcome operations retain their non-allocation failure edges. Unknown
outcomes stay conservative and are rejected by ordinary metadata verification.
Allocation remains an optimizer effect/barrier unless a transformation proves
the applicable process-elision rule; absence of a cleanup edge is not purity.

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
moved-from cleanup schedule.

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
single `TransferOut` to that exact call. Nor is a completed owning result
passed directly as a by-value argument of a call or ordinary constructor, with
no prepared-parameter shell: the place its `Value` obligation drops is exactly
one of that invocation's value operands, and the callee owns it before it can
fail. A member initializer such as `held(holder(make(ok)))` therefore keeps
the constructor's failure routing. Constructors and field-initializer bodies
retain failure routing across these calls. The status-constructor
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
construction schedule.
Initializer bodies may also evaluate lexical locals inside scoped result
regions. HIR exempts only exact program-storage and declared-field binding
identities from lexical obligations; those locals retain ordinary `Binding`
obligations and region-scope cleanup in MIR. A field-initializer rollback must
name an exact field from its owning class catalog, never a local merely because
it shares the initializer body or the field's type.
Each module initialization step separately inventories its lexical binding,
symbol and canonical place identities and nested full expressions. The C++
backend consumes that verified inventory to declare ordinary function-local
carriers inside the initializer function; only persistent plan bindings name
global storage rows. A step with lexical execution uses complete MIR body
emission, never the passive expression-only storage schedule.
A source constructor receives every unoverridden,
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
field constructor as named moved arguments.

The base-subobject family uses one `InitializeBase` after its exact typed
argument values. A fallible selected constructor has an `Invoke`; a
failure-free constructor or passive generated default arms the rollback token
at the instruction itself. Scalar parameter reads and literals are supported
base arguments, including derived constructors with owning fields and declared
base/derived cleanup bodies. Both trivial-cleanup and owning bases carry the exact
base cleanup bit in their rollback descriptor. The bounded source-free
generated-derived HIR constructor lowers through the same body schedule; it is
not a native-only default constructor or a terminal containment adapter.
A base that fails is not armed in the derived body;
later derived failure drains the completed base last, while successful
construction transfers its obligation to the enclosing object. Independent
verification binds the exact base relation, constructor, arguments, place, rollback type,
activation and retirement, and forbids aliases or ordinary
value/borrow uses of the base place. Base success must dominate both derived
field publication and observable field-argument evaluation. The verifier follows
SSA and place dependencies, including scalar joins and their controlling
conditions, using existing MIR effects to allow only harmless preparation before
the base. Selected generated base constructors retain their exact executable
target; targetless native defaults cannot satisfy a required generated schedule.
Every selected base initializer's generated-default flag must match the selected
constructor, including when the derived constructor is explicitly declared.
Base-argument cleanup and borrow eligibility use concrete class and enum traits.
An argument value can carry a transient dependency even when its passive
parameter type carries no stored reference fields. Direct parameter forwarding
retains that distinction: its entry-loan-backed load precedes `InitializeBase`,
and the entry loan must remain live through the base call. Verification checks
the exact formal, source place, load and unsuspended entry loan; an early loan
endpoint cannot be hidden behind the loaded SSA value. The base itself still
cannot retain borrowed state. This permits forwarding a dynamic text view into
an owning base constructor without mistaking it for a borrowed base object.
An exact incoming reference formal can also forward its address into the
selected base reference formal, with the same referent and compatible access.
The referent is a passive scalar, or a class or enum object that the type
catalog records as holding no borrowed state of its own; this includes owning
classes, payload enums, polymorphic classes and interface views. Its
`CallInput` names the constructor-initializer ordinal and has no ordinary
HIR call-site identity; it appears at its source-argument position and
`InitializeBase` is its sole argument consumer. Other arguments in this bounded
reference family must be inert literals or direct incoming passive scalar
formals. Effectful, failing, terminal, or owning later arguments remain valid
source but need the fuller ordered prepared-input protocol before native staging.
No terminal-abandonment relaxation is implied. The
source seal binds the runtime dereference place to the exact HIR formal, not
another same-typed parameter. A mutable formal retains its existing entry
`Parameter` loan and permission; no replacement loan or endpoint is invented.
The complete argument family is checked before publishing any such input, so
a later unsupported owning argument cannot leave partial staging or a false
constructor-state anchor. These MIR facts do not extend C++ backend admission.
Interprocedural effect analysis follows
the selected base constructor, so its allocation, memory, and other effects
remain visible in the derived constructor's summary and to the optimizer.

An owned base argument uses the ordered prepared-input protocol that calls
already use. Its formal is an owning class, a payload enum or a closure, and
the argument is either moved in from a derived parameter or created in place
by a temporary or call. It moves into a prepared parameter through a
`MoveValue` `CallInput` that names call site 0, the constructor-initializer
ordinal and the source-argument index. The `InitializeBase` alone transfers
each prepared obligation out, as an ordinary construction does. Until then the
derived constructor owns the prepared parameter, so a later argument that fails
drops it once. Once the stage begins, the selected base constructor owns it,
so a failing base drops what it received and the derived failure edge drops
nothing twice. A moved-from derived parameter is never dropped again.
Scalar arguments beside owned ones stay direct operands. These stages arm the
ordinary base rollback and route failure edges; they are not unarmed subobject
transfers. Construction still follows section 4.2.3: base arguments, then the
base, then derived fields in declaration order, then the body.

The ordered-invocation checks cover base stages as they cover calls. Each
owning input needs a distinct `PreparedParameter` obligation that the stage
transfers exactly once, and inputs precede the stage in order. Program
verification binds each prepared input to its exact stage, index, HIR argument
and type. The type catalog, not a value's own traits, decides that a formal
owns cleanup, so a value claiming trivial traits cannot stand in for the
prepared parameter. Some base arguments remain unstaged: an owned argument
beside a reference formal, a reference formal whose referent holds borrowed
state, a borrow carrier passed by value and an array of owning elements passed
by value. Such a body has no `InitializeBase`, and the native backend refuses
it as `UnstagedBaseInitializer`. The C++ backend does not represent staged
owned base arguments or forwarded class and enum references; those programs
remain native-only.

The C++ backend fuses observation-free scalar parameter/literal prefixes into
the exact native base initializer, including passive text-view parameter loads,
using its status overload when fallible,
and branches on the retained result before executing derived MIR. Checked
arithmetic, scalar calls and control flow instead execute in an admitted
pre-native-base partition of this same verified body. The immutable body plan
records the exact stage/CFG and scalar values/places crossing into the remaining
schedule; a private delegation carrier retains arguments, mutated scalar
parameters and the original failure record. Global effects are never restored
from a snapshot. Prefix failure disables the passive base gate, so no source
base constructor or destructor runs before successful preparation. Neither path
reconstructs source expressions in a native initializer list.
This preparation family does not transport owned/borrowed inputs, generic
carriers, or recursively owning/nested base shells; those remain named frame,
loan/transfer and inactive-shell prerequisites in the implementation sequence.
Passive field defaults include plain read-only enum values. For the selected
aggregate schedule, observable base cleanup runs at the explicit MIR `Drop`
and returns its secondary record before the caller tears down the empty native
representation. Other admitted base rollback finishes when the allocation
caller destroys the unpublished native object. That deferral is bounded to
trivial-cleanup bases; it cannot cross any observable cleanup order. Declared
base cleanup disarms its native lifecycle guard, and owning base fields retain
the existing emptied-shell proof, preventing duplicate native destruction.
No base is destroyed and placement-reconstructed as an
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
does not establish every cyclic ownership-type graph, owning or borrowed base
argument, or static-field initializer shape. Checked/effectful scalar
base-argument prefixes have their separate verified preparation partition above;
that evidence does not establish owned or borrowed prefix transport.
Constructors with historical entry-active declaration-field rollback
tokens remain ineligible: each declared field needs the typed constructor
schedule that arms it only after the base and that field successfully complete.
Those remain M-FAIL-01 prerequisites, rather than native representation exemptions.

M-LIFE-01 maps each HIR obligation to an exact MIR place and runs a separate
available/moved/uninitialized fixed point over lifecycle events. Parameters
begin active; value and lexical initialization activate obligations; move,
reparent, replacement, and transfer-out preserve exactly one owner; and normal
return/exit edges must retain no active obligation. Each assignment must carry
exactly one `Replace` event for every active-cleanup value obligation it
consumes. Verification checks this requirement independently of the events
present in the instruction, so deleting the whole event list cannot leave an
owning RHS scheduled for destruction after publication. The rule applies equally
to owned bindings, aggregate elements and checked borrowed referents; the latter
two do not acquire an independent destination obligation. An assignment that
converts its owning source into a distinct wrapper type instead carries
exactly one `TransferOut` of that obligation, as a converting initialization
does. The destination keeps its own obligation, which now owns the converted
value, and no later Drop names the transferred source. A same-typed `Replace`
across the conversion fails verification.
Full-expression cleanup is LIFO, lexical cleanup is reverse construction order,
and a temporary created
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
introduced. Borrowed class elements use their exact stored entry loans under
the same carrier copy/move contract; this does not admit untracked borrowed
closures or manufacture dependencies from representation.

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
the slot live at the call. In particular, every temporary-borrow input is
bound to the value-drop obligation with its exact place and source identity;
the converged lifecycle state must prove that obligation definitely available
at the input on every incoming path. Another live object of the same type,
an uninitialized arm, or prior cleanup is not evidence of availability.
The backend must not reconstruct that slot into
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
nominal method or exact stored-lambda callable target. A retained lambda's complete
closure type must match its selected lambda instance, not just its signature.
The staged capability has one exact invocation use.
The backend emits the selected receiver-qualified call against that retained
place, leaving outer full-expression cleanup unchanged.

`MirFunctionInstance::ownedPointeeProjection` describes a transparent source
accessor, not a consuming call. The bounded proof requires one direct declared
`UniqueOwner<T>` or compiler-bound polymorphic-slot receiver field and its
matching private read/mutable borrow, followed by the exact returned loan.
Unique-owner access has the intrinsic's empty-owner failure edge; inline-slot
access is one block with no defined-failure edge. The function-level failure
summary may remain conservative. Calls to other functions, writes,
alternate return origins, extra effects, and virtual dispatch do not satisfy
this proof. The verifier checks the concrete owner field/type, access, intrinsic
operand lineage, returned loan, failure path, and full-expression boundary;
ordinary receiver-derived borrow metadata is insufficient. This is the
accessor-side proof for retained pointee consumption.

For that caller, a `RetainedReceiver` stage has `retainedOwnerPlace` naming the
mutable field-only This owner anchor (including the unprojected consuming
receiver itself), while its executable operand is the
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
belongs to the enclosing MIR cleanup edge, not native closure teardown.
The caller's active failure remains authoritative across calls made by cleanup;
a callee's next failure cannot enter its own local-cleanup schedule before the
existing cleanup-emergency path consumes the exact primary and secondary records.
This inherited dynamic fact does not require a second source-level control-flow
rule or a public failure-context type.
Class lifecycle metadata contains every lexically dropped field in
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
When elements have an exact recursive cleanup recipe, the array also retains
one immediate-element recipe, recursively for each dimension. This includes
non-failing active cleanup, such as shared-owner release: the backend executes
the verified reverse-order recipe before retiring the inert native array
shell. Arrays embedded in allocated objects use the same recipe. Native array
destruction is not a substitute for releasing checked ownership resources.

## Controlled Optimization Edits

The private `verifyMirBodyWithPlaceQueries` entry observes the same body verifier
used by ordinary validation. It does not accept a cached proof or mutate MIR.
Its existing-storage profile covers direct integer/bool/char scalar roots and
direct non-borrowed-state owned roots with one exact binding/value-drop obligation.
The canonical scalar-initialization or lifecycle transfer supplies initialization;
ownership and the ordinary loan verifier's read-access check supply the other
required facts. Unsupported storage remains `Unknown`, with an `Unproven` reason.

Scalar worklist solving and stable-state observation replay share one transfer
implementation. Ownership observations run only during the existing stable
validation replay. Lifecycle observations use its existing stable replay, never
the path-refined exception used for a consuming operation. Borrow observations
reuse the ordinary read-access check and replace a block's observations when its
joined entry changes; cuts are reconsidered only after actual loan-state writes.
Neither observation changes a transfer or grants a verifier exception.
A requested readable point requires all applicable proofs; an inactive
point does not pretend to distinguish a moved/dropped/never-initialized history
that its current facts cannot establish. Required cuts retain consuming and
ambiguous CFG-entry boundaries even when later reinitialization restores
readability. Empty queries take the ordinary verifier path and allocate no
observation tables. Block entry and the boundary before a terminator are
distinct points, including in an empty block.

A separate private program-aware entry query checks concrete formal binding
identities and aliases against the canonical parameter-role comparison, and
ordinary member receiver roots against their exact concrete owner and access.
Its observation coverage includes non-borrowed-state member receivers and
concrete trailing packs whose nontrivial elements each have an exact,
entry-active Binding obligation. The read-only lifecycle replay requires every
such obligation to remain available; retiring one element ends whole-pack
readability. Unmatched overlapping obligations remain unproven. It still
requires the ordinary ownership and loan-flow observations; body-only queries
cannot invent these entry facts. Constructors do not acquire an initialized
whole receiver at entry. Source-local range production consumes the exact
receiver and concrete-pack entries alongside canonical body observations.
Lowered parameter fragments preserve the source pack binding, existing root,
concrete element index and freshly derived readable interval. Independent
verification reconstructs their complete inventory; missing or forged entries
cannot become positive debug availability. Borrowed packs without a complete
canonical referent/dependency proof remain unproven. A separately queried entry
boundary binds parameter metadata to the actual native entry block. Parameter
fragments query every instruction boundary and retain continuous proven
readability across source-inert glue, with explicit lexical, ownership and
borrow stops. Root-scoped source pack parameters do not need a statement anchor
to remain lexically visible; ordinary locals do. Generated operations receive
no invented source statements. Boundary and lexical-end indexes avoid repeated
body scans in the batched source-local query.
The semantic receiver-state producer records its accepted concrete type and one
implicit local with no binding, symbol or source token. HIR verifies that origin
against its accepted producer; MIR joins it only to an existing, unprojected
`This` root in the same place domain. A missing root stays unproven: debug
collection does not create execution places. Constructor partial objects and
borrowed-state receivers do not acquire blanket availability.

Opt-in lexical-debug transport carries the requested profile into MIR
and deep-copies collected HIR scopes, concrete local declarations, and source
membership into `MirBody::sourceDebug`. Initializer-region IDs are flattened
deterministically within that body. `None` allocates no table. The actual normal
scope-exit lowering records descriptive local ends even for trivial locals;
it does not emit fake destruction or change cleanup planning. The builder joins
source MIR declarations and membership back to exact accepted HIR facts.

Lowered receiver field fragments derive from existing verified scalar `Load`
results and their exact direct-field place identities. They are intersected
with freshly queried source-local readability and lexical ranges; unknown
availability cannot become a positive fragment. Canonical memory effects end a
cached field value after a possible write, including during an unreadable gap.
Lifetime and loan boundaries remain owned by canonical readability, not by a
second debug transfer implementation. Unrelated direct scalar local stores do
not invalidate caller-owned receiver fields. Fragments do not cross CFG edges,
and independent lowered verification reconstructs their complete inventory.

`queryMirDebugAvailability` validates local/binding/type/place-domain identity
before invoking the private canonical place query. It clips positive facts by
known source membership and lexical ends, returns `Unproven` when membership
cannot be established, and reports both execution and lexical required cuts.
Neither this transient batch nor a collected table is a reusable certificate.
The frontend selects collection before cache lookup. Lowered-program range
construction queries its owned final MIR, forms block-local half-open intervals
within known scope segments, and clips them at every required cut. A subsequent
initialization or assignment can reopen a range only with a fresh readable fact.
Independent lowered verification recomputes the range inventory, and its full
serialization participates in the ordinary construction seal. Native emission
consumes these registered boundaries; full representation coverage and the
complete two-host debugger matrix remain required for production admission.

`MirSourceValueEvent` binds a concrete local to an existing typed SSA value after
a specific instruction. Original source-MIR verification reconstructs the event
inventory from canonical scalar loads, initializations and assignments; ordinary
SSA dominance/order verification also checks every event. The bounded profile
requires unique direct scalar binding storage and excludes loans, address
escape, ownership moves, receiver/drop obligations and place-bearing edge
transport. It does not infer source readability from a value's type alone.
Lowering independently derives and verifies block-local value intervals from
these events, lexical membership and local ends. Indexed scope segments and
per-local event lists avoid rescanning all events for every range. No range
crosses an unknown lexical segment or merges values across control-flow edges.

The canonical scalar-promotion, unreachable-block and in-place dead-instruction
mutators share source-table repair. Promotion invalidates the affected existing
place; deleting a lexical anchor invalidates that local's value location and
removes the dangling cut. Declarations and scopes survive as `OptimizedOut`,
without new storage. Promotion retains eligible value events at the original
instruction identities, including tombstoned storage operations. Removing a
referenced value definition, event point or lexical endpoint invalidates that
local's value events. Optimization provenance replay and independent lowering
verification bind this repair to the actual transformation. The in-place
mutator journals changed local policies, removed cuts and value events and
restores them on refusal. These are transport and repair controls, not an
alternate lifetime model or permission to fabricate optimized availability.

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
through a known-disjoint projection. An ordinary reference without a stored
carrier dependency may also lend and read shared access while all overlapping
child accesses are read-only. Local and call-result shared children use the
same rule in either creation order; any overlapping mutable dependency still
excludes that access. A read-only child of a parent that
grants no mutation over the child's place shares access with every
overlapping read-only sibling, whatever the parent's loan kind; reborrowing a
payload of one read-only matched-enum binding therefore coexists with the
other read-only payload bindings. Verification rejects overlapping exclusive
use of a suspended parent, reads overlapping an exclusive child, a
read-only-to-mutable transition, or
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

Reference initialization records both the retained provenance loan and the
exact runtime address place. Those identities may differ when several shared
aliases protect the same owner but accessors select different referents.
`referenceAddressRetainsLoan` checks a loan-rooted address against that retained
loan: it is either a descendant with exact typed call publication or a parallel
read-only publication over identical source and dependency identities. The
body verifier and independent C++ admission use the same identity query;
neither infers an address from carrier order or a coarse owner key.

A read-only local reference may capture an active, unretained read-only
call-result descendant while its mutable provenance parent is suspended. This
is permission only for exact `Initialize` into a reference binding already
listed as a carrier, not for an executable read or write through the parent.
Loan flow requires the complete address-producing ancestry and stored
dependencies to be definitely active, and permits no live mutable sibling.
The child keeps its existing full-expression endpoint and the retained parent
keeps its existing carrier lifetime. Calls, loads, mutations and returns do not
gain a general suspended-parent exception. This supports read-only nested
container references and unrelated source-defined accessors without changing
overload selection, escape rules, IR serialization or runtime loan tracking.

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
An accessor's dependency may instead name a checked field projection of that
formal, or of a uniquely initialized reference carrier retaining the same
parameter loan. `referenceParameterContainsPlace` proves that stable field-only
path back to the formal; it does not substitute the projected owner for the
accessor's returned address. Typed call publication still checks the exact
physical projected receiver, while a captured reference may retain the whole
formal's conservative lifetime authority. Raw, indexed or opaque origin paths
are not admitted by this field-containment proof.
Without an explicit runtime place, only the exact parameter referent type is
valid. Carrier membership or a same-typed unrelated alias cannot substitute
for this executable address evidence.

A lambda body holds each by-reference capture through a `Parameter` entry loan
on the capture slot, the closure-body analog of a reference formal's entry
loan. Call and construction lowering resolve a capture-rooted operand through
that loan exactly as a binding-rooted operand resolves through its binding
loan. A carrier made by a method of the capture's referent is therefore a child
of the capture entry, and it borrows the dereferenced capture slot, the
referent place a reference formal's receiver supplies, not the slot itself. A
read-only formal entry parents only read-only carriers: an exclusive borrow
taken through shared access to synchronized storage, such as a mutex guard, is
a root loan, as it is when the formal has no entry loan. A carrier returned by
a method of an accessor's result keeps that transient accessor chain alive,
child-first, up to the semantic owner, exactly as an accessor result passed to
a constructor does.

The closure environment, not a binding, holds a capture's entry loan for the
whole call, so that loan has no carriers and a body-local reference alias never
joins it as a formal's alias joins the formal's entry. The alias carries its
own physical loan below the entry instead: the call-result loan of an accessor
on the capture's referent, a fresh `Local` loan of the referent place it names
through the capture or through another such alias, or the loan of the alias it
copies. The semantic identity stays on the entry loan, and releasing a finished
alias never folds the entry in as a transient link, so the body can still use
the capture after the alias's last use.

Semantics decides structurally whether a closure is a cleanup-bound carrier
(owner decision 2026-10-27): its environment is an aggregate of its captures,
so moving in an owner with a destructor beside a borrowing capture leaves an
ordinary loan that ends at the closure's last use. Only a capture that is
itself cleanup-bound makes the closure bound. Each lambda instance records
that decision (`lifecycleBoundBorrowCarrier`, `mir-v118`); program
verification re-derives it from the captures, as it does for a class from its
fields. A closure formation that moves such a carrier in produces the
closure's lifecycle-bound `Stored` loan. That loan follows the closure's
obligation through moves and by-value parameters, and ends only after the
closure's drop or a complete handoff, under the same rules as a class
carrier's. Inside the lambda body a by-value capture belongs to the closure
environment, which owns any release obligation it contains, as a caller owns
a method's receiver; the capture's stored entry is therefore not
cleanup-bound there, so the body is not bound by a destructor it never runs.

A body-local reference may share an existing semantic owner identity while
capturing an accessor's different runtime referent. Lowering accepts that
address only when its live, nonescaping call-result chain reaches exactly the
retained owner without widening access. The reference `Initialize` records both
the owner loan and the actual returned-address place. A later field projection
through this binding (or a direct reference alias of it) stays rooted at its
captured address; canonicalization must not append the referent's fields to the
enclosing owner or range. The transient accessor loans still end normally,
while the reference retains the owner's checked lifetime and overlap authority.
The same identity applies to a directly captured projected field address and
to an unprojected dereference of its reference carrier, not only to another
field access on that carrier.
This applies equally to source-defined accessors and allocation-backed members.

`MirRangeIteration` owns the bounded range exceptions. It names the exact
owner/range/iterator/sentinel bindings, strategy, access mode, parent range
loan, child element loan, condition/body/increment/cleanup blocks, and
owned-temporary drop. A fixed-array plan ties its dynamic index place to that
inventory rather than claiming general indexed-place disjointness. A
structural mutable plan carries the exclusive parent through the
source iterator returned by `begin_mut()`. An inline reference proxy keeps
three distinct identities: traversal authority, the producer-owned stored
cursor dependency, and the dereference call-result loan protecting cursor
storage. A projected accessor retains that actual returned address, not a
field appended to the coarse range owner. Nested shared loops cannot reattach
physical cursor/element bindings to a reused semantic owner loan. External
stored-reference-field dereferences remain a separate ordinary projection
shape; borrowed element type alone does not select the inline-proxy profile.
The hidden reference binding itself always contains borrowed state. Physical
proxy classification instead uses the selected dereference producer's resolved
nominal result traits. An ordinary owning class reached through an abstract
Stored-parent cursor retains that exact parent's iterator carrier and dependency,
without inventing a second physical cursor. Body verification checks the
producer's original HIR identity, receiver place, access and referent type;
program verification independently checks its selected dereference declaration
and nominal traits against the concrete class catalog. Rewiring a physical
cursor and falsifying the producer traits cannot manufacture the abstract
profile or shorten the proxy's lifetime.
Lowering ends descendants child-first before
increment and on `continue`, `break`, `return`, normal cleanup, and failure
cleanup. An escaping owned temporary is impossible because its drop follows
the final parent loan on every path. Every owned temporary with lexical drop
names its exact obligation, whose cleanup type is the owner's type; that
includes passive cleanup, where the class has no source destructor and no
field needing cleanup.

Read-only reference bindings over independent value yields additionally name
their iteration-local `elementOwner`, original `elementAccessSource`/value,
selected dereference declaration, and exact producer/owner drop obligations.
The producer is the selected read-only dereference on the hidden cursor, not
an arbitrary same-type call. A lexical result transfers its existing drop
obligation into the hidden owner through ordinary verified publication;
primitive values retain their trivial cleanup. The written element borrows
that owned place. Parent, stored and additional dependency paths must all end
before condition, increment, range cleanup or exit, and a body-local guard must
drop before the value it borrows. Both source and owner obligations are retired
at those boundaries. Out-of-body place accesses and escaping loans are invalid.
Reference yields instead preserve the producer's exact physical loan through
the hidden alias without copying or shortening its dependency set.

`mirRangePublishedInstruction` and `mirRangePublishedElementOwner` expose these
authoritative publication identities to the editor and optimizer. Current
canonical replacements preserve the producer, initialization and stable owner;
independent evidence replay checks the same rule. Unrelated computations in the
loop body remain optimization candidates. An actual scalar-promotion miss for
this owned binding reports its stable per-iteration publication requirement.

For a fresh hidden exclusive cursor, a zero-argument read-only endpoint may
need the range shell while the cursor retains its exclusive child dependency.
Semantics/HIR explicitly prove that the selected endpoint returns a value with
no reference or stored borrow. `MirRangeEndpointParking` then binds the exact
begin call, endpoint receiver stage/call, cursor, source range, and
`ParkRangeCursor`/`RestoreRangeCursor` instruction identities. Parking suspends
permission, not lifetime: the cursor and its retained dependencies cannot be
used, transferred or retired. The only permitted intervening instructions are
that exact read-borrow receiver stage and static endpoint call. Restore is
immediate after a nonfailing call or first on both Invoke continuations, before
sentinel publication or failure cleanup. The prior loan states are restored
before a cursor destructor can access its owner. Arbitrary control-flow edges,
extra operations, borrowing sentinels, forged origins and missing restores
fail closed. Ordinary independently written endpoint calls gain no such
permission. These instructions are loan-schedule optimizer barriers with no
runtime state; backends only erase their verified neutral representation.

A read-only plan may share a mutable parent. When the range expression is a
call returning `mut T&`, the plan's range loan is that call's mutable result
loan, and the element-owner and element loans may name that loan directly
instead of a read-only child. Verification still requires the declared
element access to be read-only in that case, and a mutable element access
requires a mutable range loan.

Verification requires semantic, HIR, and MIR access modes to agree; a mutable
element loan must be a non-escaping child of the exact mutable range parent.
It rejects a detached or widened child, overlapping active owner use, missing
or premature `EndBorrow`, inconsistent loop edges, and a mismatched or missing
temporary drop. The backend consumes this verified plan. It does not infer range
lookup, invalidation, extraction lifetime, or cleanup from emitted C++.

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
admitted ordered call, method, operator and construction profiles, the verifier
requires a function receiver when applicable, argument inputs in exact
source/parameter order, and the final `Call` or `Construct` to form a strict
dominance/order chain. A read or mutable callable receiver uses its exact
borrow capability; an explicitly moved receiver uses a `MoveValue` receiver
checkpoint with its caller-owned full-expression lifetime, while a fresh
consuming receiver uses
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
and the final call transfers every stage exactly once. Scheduled ordinary
constructors use the same prepared-parameter transfer at invocation; their
verified construction-rollback obligations own the subobjects completed after
entry.
Borrow-source, callable-source, ownership, and loan-flow checks trace through
the checkpoint's one underlying operand.

A `ReadTemporaryBorrow` or `MutableTemporaryBorrow` receiver input names one
mutable materialization place produced by exact construction or a GTI
function or lambda call. A linear value uses its
value-rooted place; a branch-only value uses
the one hidden temporary slot already required to carry its conditional drop
through a CFG merge. The shared MIR materialization query accepts only those
two exact forms and resolves one producer/source identity for verification and
backend consumption. The producer strictly precedes the input; the input
strictly precedes all arguments and invocation. Exactly one matching value-drop
obligation is activated by successful production and remains attached to the
enclosing full expression. Verification also requires the selected non-static
GTI target, a call dispatch equal to the target's own (`Virtual` exactly when
the target is virtual, for read, mutable and consuming members alike), an
owner that is the temporary's concrete class or one of its recorded base
instances, receiver qualifier, result
non-escape traits, producer type, place type/access/source identity, and drop
descriptor to agree. This is an explicit MIR borrow schedule, not a native
rvalue convention.

An admitted borrowed carrier reuses this materialization and drop schedule.
`findMirTemporaryCarrierDependency` binds the exact producer/value, one
nonescaping `Stored` loan with its complete stable owner-dependency set, and
full-expression value obligation. A separate `CallArgument` loan over the
temporary's physical shell
names that loan through `storedDependency`, not `parent`: the shell is not a
subplace of the external owner, and mutable shell access cannot upgrade the
external dependency. The receiver input records this physical loan and ends it
after invocation or before failure cleanup. The complete stored dependency set
and required ancestors remain live until the carrier's drop. Lifecycle verification rejects
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
Non-lifecycle-bound constructor entry loans also remain live until rollback
finishes. A later initializer can publish such a reference into a field and
change its final loan classification after earlier failure edges were built.
Those edges retire entry capabilities together with published dependencies,
child-first after subobject cleanup, rather than ending them before rollback
based on an incomplete publication inventory.

A read-only operator on an eligible stable-dependency borrowed-class prvalue uses
the same explicit `ReadTemporaryBorrow` schedule as an ordinary method. Other
already-supported read-only borrowed-class operator observations retain their
ordinary ordered `Value` receiver stage. Its source has an explicit full-expression
materialization/lifetime obligation and retains its separately verified stored
dependency loan. The stage aliases the source result without copying, moving,
or transferring that obligation. Native emission requires the exact source
value, stage site/role/type, unique result slot, activation, and scheduled drop;
it does not reclassify this carrier as a fresh owning temporary.

An ordinary-call argument may use `ReadTemporaryBorrow` only when its HIR
input names an exact fresh class/struct producer or a passive payload-enum
construction, and its selected parameter is read-only `T&`. Lowering
materializes the producer's exact mutable place, emits `BorrowRead`, and
attaches one `CallArgument` loan to the `CallInput`;
an explicit zero-input ordinary class `Construct` can be the bodyless generated
default producer. Body verification recognizes its exact structural shape; independent
program verification also requires the concrete owner's generated default
constructor and no missing initializer schedule. This shares the receiver
producer query and preserves all materialization, call-loan and drop checks.
The shared invocation-target query uses that same shape and concrete lifecycle
proof to classify this producer as executing no source body, rather than an
unknown call that could access unrelated persistent storage. Per-instruction
and transitive summaries retain its initialization without inventing user-code,
runtime, allocation or escape effects. An unproven construction boundary keeps
conservative unknown memory/call/trap and escape effects; persistent-storage
checking cannot dismiss it merely because the instruction is a construction.
A required explicit
or implicit field initializer, or inherited-base initializer, instead has an
exact concrete constructor schedule whose transitive effects remain checked.
Missing or forged generated-constructor authority cannot suppress those effects.
As for receiver inputs,
a linear value uses its value-rooted place. A call evaluated only in a
conditional arm or a `&&`/`||` operand borrows the one hidden temporary slot
that carries the producer's conditional drop through the CFG merge. The merge
retargets the producer to that slot, and the loan, which already ended inside
the arm, names it. Both the loan rule and the input rule admit only those two
forms through the shared materialization query.
An argument may also name an initialized join-result slot through that
query's exact self-slot Load/Move transport. It does not create a second
object or activate a second owner. The independent lifecycle check requires
the slot's exact value-drop obligation definitely live at the borrow on
every incoming path; the non-escaping loan still ends before its later
transfer or cleanup. This applies equally to ordinary conditional results
and scoped macro results, without a library-type exception.
The loan is active while later arguments are prepared and ends in
reverse temporary-argument order immediately after the call, or at the start
of both normal and failure successors for an `Invoke`. The producer's one
full-expression drop remains independent and follows the loan end; for a
branch-only value it is the conditional drop after the merge, and failure
edges inside the arm drop the definitely active slot. Verification rejects a
mutable/projected/wrong-type place, a hidden slot detached from the exact
producer identity or naming another temporary's slot, a native or
declaration-only producer/target, borrowed state, an escaping result,
producer/input reordering, premature or missing loan ending, and a missing,
duplicate or unconditional post-merge drop. The backend therefore consumes a
complete schedule rather than relying on C++ reference lifetime rules.

Fresh read-only callable arguments extend that same schedule with exact
`Closure` and `FunctionItem` producers, not an implicit owning copy. A closure
containing borrowed captures must have its own producer-published `Stored`
dependency group. Its `CallArgument` shell retains that exact group as
`storedDependency`; independent closure verification still proves every capture
identity, selected origin, access and parent dependency. The shell loan ends
after the call, while captured dependencies survive until the full-expression
closure cleanup finishes. Missing, substituted or prematurely ended capture
dependencies are invalid MIR. The ordinary confined callable contract and
invocation capability checks remain required independently of materialization.

An ordinary enum observer and the generated `has_value` observation for
source-defined propagation use the same schedule. Their materialization may be the
exact typed `PayloadConstruct` producer, checked against the enum's alternative
table, rather than a class constructor. The complete full-expression lifetime
obligation remains mandatory, including for passive payloads; the call loan
still ends on every normal and failure successor. This does not permit an
observation to return or retain a reference to temporary enum storage.
Passive enum representation retains the existing SSA value, rather than
allocating a class lifetime slot. Its view requires an exact unique
`PayloadConstruct`, typed alternative, materialization, read loan, call stage
and full-expression obligation. Subsequent passive copy/transfer stages keep
that same value; owning or reference-bearing alternatives are not admitted by
this representation rule.

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
schedule through the implemented `Module/0` body.

`PackFold` retains a bounded HIR expansion proof, but each concrete element now
has a unique ordinary call identity and the same `CallInput` schedule as other
calls. HIR verifies the source-pack order, concrete types, source-prefix copies,
parameter positions and selected function instances before MIR lowering. MIR
independently verifies the concrete pack binding, read-only element projection,
resolved traits, exact target/signature, unique staged-input consumer and input
order. Prefix borrowing therefore starts at its checkpoint, not merely at
invocation: later input evaluation, including a TLS initializer, cannot
invalidate it. Each invocation completes before the next element's fresh input
evaluations. An empty expansion evaluates none of those inputs.

Legacy compact reference operands retain invocation-time protection against
hidden persistent-storage effects even without a named source loan. This
general operation-family check does not substitute for the earlier checkpoint
of an ordinary staged input or authorize new complex input producers. Scalar
reference inputs of calls also require independently initialized storage;
raw address formation and storage-lifetime-start destinations remain distinct.
Physical pointee checks distinguish an object place from a reference-valued
formal-pack element. For an exact single `PackElement` projection, the verifier
resolves the binding's concrete pack and element index, checks the complete
projected type and carrier facts, and derives the element reference's pointee
and access. The immutable carrier does not make its referent immutable. This
does not unwrap arbitrary reference-typed places or waive unrelated type,
origin, index or access mismatches; ordinary exact/public-base compatibility
still applies to the derived referent.
No metadata asks a backend to iterate a source pack or instantiate a target.

Program v103 adds the resolved `threadConfinedCallback` declaration fact and
`ThreadConfined` adapter policy. It represents first-use binding in the existing
`NativeCallback` instruction, not an AST-only emission path. The program-aware
effect query preserves passive legacy pointer formation, but thread-confined
conversion is runtime-invoking, potentially trapping, non-removable and
non-speculatable. The context-free query conservatively permits binding effects.
MIR verifies adapter policy against its target's declaration fact, and detached
LoweredProgram v19 also verifies that fact against its declaration row.

Native callback conversion has one program-level representation contract.
Each `MirNativeCallbackAdapter` copies the exact HIR adapter identity and adds
the concrete target's defined-failure effect, the
`TerminateInvocation` containment policy, and the requirement to catch native
exceptions. The producing MIR value references that row. Verification rejects
a missing, stale, reordered, owner/member, external, signature-mismatched, or
unsupported concurrent GTI adapter before a backend sees it. The exact
C-thread entry additionally retains `CThreadCallbackKind`: `ThreadEntry` for
`int32_t(void*)`, `OnceInitializer` for `void()`, `ThreadConfined` for an explicit
source contract independently of native signature, and `None` outside the concurrent
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

`mir-v101`/`lowered-program-v17` retain `ProcessMemoryRuntimeOperation` on the
exact private native declarations of the process allocator's runtime ABI
(`std/detail/memory_capabilities`'s `gti_internal::runtime` block): the native size and address
alignment queries, the POSIX pointer-size/`ENOMEM`/`EINVAL` queries, POSIX
allocation and release, and the Windows aligned allocation pair. Semantics
classifies only C-linkage, bodyless, non-generic, compiler-private
declarations of the trusted `std/detail/memory_capabilities` unit whose symbol and signature both
match the shared table in `process_memory_runtime.h`; an application
declaration of the same symbol stays an ordinary native boundary. HIR and the
lowered builder exact-compare the field, the lowered verifier requires private
identity, and MIR verification re-derives the operation from the symbol and
signature and requires C linkage, declaration definition kind, no owner or
arguments, and the target-selected family (Windows aligned pair, POSIX family
elsewhere). Calls remain ordinary native `Call` instructions; no body
operation changes.

`deriveMirProgramEffectSummaries` gives a classified declaration this local
summary instead of the conservative native boundary, and `summaryForAddress`
lets callers consume its verified stored summary. Every operation invokes the
runtime and is target-dependent; none synchronizes, runs GTI code, traps,
reads place or unknown memory, or retains an argument. Allocation additionally
`allocates`; POSIX allocation writes its `int32_t*` status pointee, and release
retires a block that holds no live object under the lease contract. Both are
stated as `writesUnknownMemory`, the current vocabulary's sound stand-in for
an exact-pointee write and for retiring storage. The free-to-next-allocation
ordering inside the native allocator carries no GTI-visible value, so it is
not `maySynchronize`. Removal, reordering, and speculation remain denied;
allocation elision is a separate mechanism. Verification rejects a forged
symbol, signature, linkage, definition kind, operation, unrelated
classified declaration, or target family, and an erased classification
whose callers keep the precise summary.

M-LIFE-01 supplies body-local temporary identity, lifetime start, transfer or
reparenting, active drop, and LIFO full-expression obligations. MIR v18 added
caller-owned ordinary-call parameter stages and an edge-initialized cleanup-
owning call-result shape. Subsequent slices cover the currently admitted
borrowed-state inputs, result and target places, operators, branches and
constructor publication through their exact verified contracts. A new input,
inline-publication or immovable-transport profile must extend those facts; the
original v18 scope is not a current list of missing whole operation families.
Program initialization already has a dedicated plan, per-block
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
cleanup-owning result on its success edge. Any call that arms a result drop on
its success edge receives this failure control flow, whatever the result's
drop kind: a cleanup-free payload enum materialized in a temporary slot for a
call's read borrow arms its slot obligation there too. A lexical result that is the direct
initializer of one binding may instead name that exact destination; its sole
`Initialize` consumer is required on the normal successor. The original
checked-scalar profile is not the
complete present-day failure vocabulary: owning results, constructors,
field/module initialization, failure-capable destructors and double failure
have their separate publication and cleanup contracts described above.
A completed constructor subobject arms `ConstructionRollback`, so failure
drains only already-built subobjects. Selected generated-default and
field-initializer schedules preserve the same successful-publication rule;
an unstaged construction shape is not automatically admitted. Loan-producing
operations likewise need their exact success-publication proof: a failure path
must not end or use a loan that the operation never produced, or end its
parent ahead of a live child. The admitted checked-borrow and reference-result
protocols supply those facts; they do not establish a blanket exemption for
new borrowed-result families. The general body emitter validates each complete
caller-to-callee record route and hosted containment before emitting failure-form text. Broader
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
physical-place operation in this family. A selected class subscript stages its
exact returned reference as one loan-rooted place before the primitive update;
the accessor is evaluated once, not separately for the read and write. The
older unstaged class-index representation retains its closed compound boundary
only where no returned-reference place has been staged. The emitter admits
each operation in this fixed-integer allowlist only after both MIR verification
and representation preflight.

The bounded `Invoke` family accepts a trivial scalar result, an eligible
cleanup-free nominal call result, or the exact cleanup-owning result obligation
of an eligible ordinary call. A nominal call needs its failure edge in nested
argument positions as well as at a full-expression root, even without a result
drop identity: the consumer's prepared slot is initialized only after the
producer succeeds. A fresh class constructor used as
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
obligations rather than trusting new serialized metadata. These focused argument
probes do not certify every combination of nested
owning parameters/results, borrowed results, constructor destinations,
assignments and class-valued joins. Admitted forms use their separate verified
materialization, transfer and cleanup contracts; unsupported combinations must
extend those general facts rather than bypass them. The
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
