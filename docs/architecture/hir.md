# Typed HIR

Status: Implemented concrete-instance, typed-value, normal-exit lifecycle, and
defined-failure identity foundation.

HIR is GTI's backend-independent graph of concrete program instances and
executable typed values. It sits after semantic analysis and before body-local
control-flow lowering. It is not a pretty AST and it is not an independent
type system.

Concrete function instances preserve the semantic owned-pointee accessor
summary (exact receiver-field identity and private borrow operation), refreshed
from concrete instance analysis. It is proof metadata, not a synthetic field
access: the accessor call and its returned loan still describe execution. In
particular, lowering must not replace a source accessor with a caller-side
access to that accessor's private storage.

## Inputs And Outputs

Expression-valued assignments retain their complete written target in the AST
`PlaceSet` node. Semantics checks its ordinary mutable-place authority and
active result-loan conflicts. HIR `PlaceSet` retains the exact typed target
and right operand; an overloaded compound operation instead retains its
selected ordinary call. MIR evaluates the target dependencies once, then
assigns through that resolved place or returned loan using ordinary store,
failure, replacement-cleanup, and full-expression loan endpoints. The backend
does not infer writability from a native C++ call result or inline an accessor
into its private field.

`HirLowerer` in `include/gti/hir.h` consumes the checked `Program`, target, and
`SemanticVisitor`/`SemanticModel`. It produces `HirProgram` plus diagnostics.
Concrete generic validity is obtained through semantic instance reanalysis.
Concrete class fields retain that analysis's exact binding type, traits and
selected initializer, including checked owned value-expression substitution and
nominal-field defaults. Open-primary lifecycle unavailability is not an explicit
source `= delete`: the concrete structural trait query determines copy/move
availability while preserving written policy. HIR does not ask a backend to
evaluate generic arithmetic or synthesize missing field initialization.
`HirProgram::instanceSymbols` retains the exact ordered semantic symbol records
introduced only by those concrete analyses. Their request-wide IDs are disjoint
from primary and other instance symbols. HIR checks concrete binding types and
access against these records; lowered-program construction copies them into the
ordinary symbol inventory. The backend never discovers missing locals from AST
or fabricates their types. Different substitutions and shadowed branch locals
retain independent source/type identities; discarded arms contribute no facts.
Generated default construction retains the already-admitted concrete owner's
discovery depth: its field schedule is not a second source-instantiation edge.
Each distinct generic constructor still consumes the request's instance budget;
ordinary constructor bodies and all other discovery limits remain unchanged.
Every reanalyzed body has its own loan-number domain, including a range whose
types do not depend on generic parameters. Plan verification retains exact
non-dependent range types and selected protocol operations, compares loan
relationships across primary/instance domains, and independently checks the
actual body-local loans, access modes, source bindings and carriers. Numeric
loan equality with an unrelated primary analysis is not a provenance proof.
Successfully checked static assertions are skipped entirely: they are semantic
obligations, not HIR declarations or executable values.

### Reachable executable prefix

Concrete analysis must cover every declaration, because instance reanalysis
reports the diagnostics of generic bodies. Executable lowering needs only what
the program can run. With `FrontendOptions::lowerReachableOnly`, which artifact
builds set, `HirLowerer` first seeds only the program roots and drains the
worklist:

- the hosted entry;
- library exports;
- every C declaration and `[[c_abi]]` or `[[c_opaque]]` record, because the
  native header and layout checks publish the C boundary whether or not GTI
  uses it;
- non-generic enum identities;
- the concrete classes that template declarations name in signatures, fields,
  and bases, because templates are emitted whether or not they are
  instantiated.

It then seeds every other declaration exactly as complete lowering does, and
drains the worklist again.

Instances are appended to their tables when first enqueued, and are finished
once processed. Ordinary on-demand enqueueing discovers everything else the
roots use: callees, constructors, destructors, virtual members of reachable
classes, callback and thread-task adapters, and argument-append targets. So
the tables after the first drain are a closed prefix of the final program, and
a later instance is never referenced from them. Instance identities are table
positions, so truncating every table to the prefix keeps each identity. The
truncated tables are classes, functions, constructors, destructors, lambdas,
both adapter families, enums, and instance symbols.

`HirLoweringResult::reachable` carries that prefix only when it is smaller and
passes `verifyHirProgramPlans`; otherwise lowering uses the complete program.
The frontend lowers the prefix to MIR. The lowered-program declaration
inventory then omits a non-generic class, function, or constructor that has no
instance. Generic declarations remain, because retained templates can name
them. Without the option, seeding and instance order are unchanged, so
inspection of every declaration's lowering keeps its established form.

The concrete discovery worklist retains the first outer requesting source span
alongside its existing depth bookkeeping. Missing-definition diagnostics from
semantic instance reanalysis use that requesting call as primary and preserve
the internal call, declaration, and distinct immediate request as related spans.
This is diagnostic provenance only: it changes neither instance keys nor
discovery limits, semantic selection, or executable HIR/MIR behavior.

For early constant requests, `lowerConstantFunction` and
`lowerConstantConstructor` reuse ordinary body lowering on a closure rooted in
one completed constexpr function, method, or constructor. Semantics supplies
the completed-definition inventory after primary body and scope analysis. A
fully selected body has also passed loan-flow validation. A generic primary
with deferred compile-time branches is available only for concrete reanalysis;
its incomplete primary flow is not a return, movement, or borrow proof. Every
discovered source callee must belong to that inventory; registration alone does
not authorize lowering. Concrete reanalysis must select every compile-time
branch and validate the resulting flow before HIR can publish its body.
The root carries exact semantic owner/function/constructor type/value
substitutions; ordinary instance
discovery and semantic reanalysis validate concrete generic bodies and callees.
The staged closure admits passive structural class catalogs with ordinary exact
field substitution. Discovered constexpr methods and constructors must likewise
belong to the semantic completed-definition inventories. Constructor rows retain
semantic constexpr eligibility explicitly; a generated native constructor does
not establish it. The constant executor currently admits read-only methods and
explicit constructor field initialization. Staging still excludes destructor
execution, active cleanup, bases, polymorphism, compiler-capability owners, and
generic packs.
It produces no program-initialization or hosted-entry plan and
deliberately omits the whole-program analysis seal. Its output can feed the
ordinary failure-metadata builder and MIR lowerer, but cannot pass normal
whole-program plan verification or be published to the backend. Normal
`lower` retains its original seal and program-plan checks.

`HirProgram` owns:

- the selected execution profile copied from the pre-semantics target facts;
- the semantic analysis seal, artifact program kind, exact
  program-initialization plan, hosted-entry or dormant embedding-export plan,
  and merged module initialization body;
- concrete class, function, constructor, destructor, and lambda instances;
- exact native callback adapter identities linking a callback type to one
  concrete free-function instance;
- exact managed-thread task adapter identities linking one concrete spawn
  value to its task type, exact `void` or owned result type, and once-invocation
  contract;
- enums and resolved class/base/lifecycle metadata;
- executable `HirBody` values, bindings, statements, and root statements;
- snapshot-local full-expression identities plus typed lexical/value drop
  obligations for values that require structural cleanup;
- source-expression to concrete-value mappings used by optimization analysis
  and cross-phase verification.

IDs are stable only inside one `HirProgram`; zero means no identity.

Enum discovery keys each row by its complete semantic type: primary enum
identity plus canonical type/value arguments. Semantic substitution supplies
the exact alternative signatures and folded lifecycle facts. Generic primaries
are not concrete representation rows. Discovery publishes a concrete enum row
before visiting its payloads, then recursively discovers every alternative's
concrete class/enum dependencies, including alternatives never constructed in
source. This permits finite recursive owning sums without inventing a second
generic type system or omitting an unused alternative from layout.

Namespace and member type aliases discover exact enum identities separately
from class layouts.
Walking an alias's type arguments may register enum rows, but a class appearing
only in that metadata retains a forward identity rather than an executable
class instance. An enum's actual alternative fields still discover the class
layouts their representation requires. Semantic alias validation is unchanged;
validating a type is not authority to instantiate its construction or cleanup.

Class instances retain an ordered type-alias catalog pairing the primary
semantic `TypeAliasId` with the exact owner-substituted type. Semantic instance
analysis resolves aliases, including checked value-generic expressions and
exact nominal specializations; HIR verifies the declaration order and closed
types against the primary semantic catalog. MIR preserves these rows without
source pointers. Detached lowering checks them independently against the
resolved declaration inventory. Retaining this catalog does not itself enqueue
class layouts; executable fields, calls and signatures discover the concrete
instances they need. The C++ representation planner can therefore publish
ordinary concrete classes containing aliases without substituting a
representative instantiation's field type or construction operation into a
source template.

Consuming `expected<T, E>` combinators receive their exact operation, mapper
identity, and result type from semantics. `transform_error`, `transform`, and
`and_then` share one expansion into a state test,
mutually exclusive active-alternative extraction/move paths, an ordinary
call to the selected function or concrete lambda on the applicable path, an
Unexpected wrapper, and an Expected-valued conditional join. Error mapping
changes `E`, success mapping changes `T`, and success chaining retains the
mapper's complete expected result with the same exact error type. The mapper's ordinary
defined-failure edge and the existing full-expression lifecycle pass govern
cleanup. `void` success constructs the empty success alternative without a
payload extraction. Synthetic values retain stable IDs; lowering does not keep
pointers into the values vector across appends.
For a void-returning success mapper, a value's ordered `sequencedInputs`
complete the ordinary void call before default construction publishes the
`expected<void, E>` success. These are structural void-evaluation dependencies,
not a backend expected special case. They share the full-expression lifetime;
verification rejects missing, duplicate, cyclic, non-void, nonreturning, or
cross-expression dependencies and verifies the selected void mapper is not
lost. Reachability, lifecycle assignment, and synthetic-default checks include
these graph edges. MIR schedules the existing call and construction operations.

The conditional's explicit `conditionalInputs` retain receiver-then-mapper
evaluation before branching. They participate in full-expression assignment,
module reachability, and parent-edge checks; synthetic default values cannot
carry them. Plan verification ties the ordered pair to the selected semantic
call's source expressions and rejects missing, reordered or unrelated inputs.
MIR emits them once on the incoming path. Lambda target/environment identity,
read invocation capability, and callable failure propagation reuse ordinary
lambda call lowering, not a second callable ABI. A named environment is reused;
fresh owning captures remain live for cleanup even when the error arm is skipped.
An explicitly moved lambda receiver uses a `MoveValue` stage independently of
its read-only invocation capability. Its value obligation retains cleanup for
the full expression; the receiver stage does not transfer that obligation.

Borrowed success extraction preserves the semantic read-only place category.
`expected<T&, E>` propagation uses an `ExpectedValue` call carrying
receiver-origin borrow authority and never adds a success `Move`. Only its
error transport follows the carrier's value category. Borrowed `expect` uses
the same extraction as the single normally completing arm of its conditional;
the other arm is terminal panic. HIR verification checks the exact receiver,
referent type, access, plan and transfer identities, including the absence of
a referent copy/move. No public type spelling supplies these rules.

`expect(message)` uses the same selected-combinator record and eager input
contract, with receiver then counted message rather than a mapper. Its success
arm extracts/moves the active owned value, borrows the reference success, or
performs the void observer; its
error arm is the same terminal `Panic` intrinsic used by the trusted panic
declaration. The intrinsic carries the call's semantic failure origin and
caller-tracking contract. HIR plan checks reject a normal continuation or a
different message on that arm. MIR conditional lowering joins only normally
completing branches; the panic arm cannot initialize a result slot or add a
fictional success value. Existing cleanup obligations own receiver/error and
message-storage cleanup after the panic record has been captured.

Each concrete source constructor selects its omitted declaration field defaults
into its own HIR body. Explicit initializers override defaults; the combined schedule remains
base-first and field-declaration-ordered. Each selected initializer has a distinct
full-expression before constructor body statements. The declaration initializer
catalog remains available, but is not a second execution site for these owners.
Generated defaults with declaration-initialized fields also receive a concrete,
source-free constructor instance. Its zero declaration identity is not a
fabricated AST constructor: semantics selects the declaration defaults through
`ClassLifecycleInfo::generatedDefaultFieldInitializers`, HIR lowers their exact
concrete expressions in field order, and MIR owns publication, failure edges and
rollback. Verification checks each selected expression against the original
field declaration and rejects missing, reordered or substituted defaults. This
does not add implicit initialization for fields without a declaration default.
Selected defaults retain existing scalar initialization conversions through one
semantic compatibility rule; literal-dependent narrowing is still selected in
semantics, never reevaluated by an IR verifier. Owning/passive `expected` field
initialization selects the exact success or error alternative. MIR publishes
the destination wrapper as a distinct typed lifetime using the same
`TransferOut`/`Initialize` protocol as local initialization, with rollback for
the complete field type rather than its input payload. A passive selected
alternative still receives the field's exact variant cleanup schedule; this
does not extend borrowed-success conversion rules.
The generated identity survives in MIR and is checked against the detached
constructor catalog, so a backend cannot infer or erase it from parameter arity.
Concrete class rows independently retain the semantic obligation to use a
generated schedule, even when no default construction of that specialization
is selected. Removing a constructor row and its call target therefore cannot
turn the operation into native default construction. Unused defaults need not
be instantiated merely to prove that obligation. Detached declaration metadata
and optimization replay retain the same immutable fact.
Final-address object construction and prefix emplacement propagate the same
selected generated constructor failure while preserving their own preflight
checks and publication rules.

Selected initializer source values retain their resolved expression types;
the field target type need not be identical when semantic initialization
permits a conversion. HIR checks the existing context-free scalar, null, or
selected expected-alternative conversion rather than retagging the source
value. Literal-dependent range decisions remain in semantic contextual typing.
Owning expected initialization preserves the payload and completed wrapper as
distinct lifetime identities for MIR transfer and rollback.

The existing bounded generated-derived family also has explicit body-local
execution: a nongeneric derived class with no instance fields or destructor,
whose nongeneric state-bearing base selects an exact zero-parameter source
constructor. `ClassLifecycleInfo::generatedDefaultBaseConstructor` supplies the
semantic selection. HIR discovers that base and records a source-free
`generatedDefault` constructor instance, with zero declaration identity and
one exact base initializer. It does not fabricate an AST constructor. HIR
verification binds the empty generated body, concrete owner/base identity and
constructor target to semantics, and rejects erasure of the target/failure
propagation together. Empty generated defaults and other unselected shapes keep
their existing route. Once every selected constructor owns a declaration default,
the backend emits no duplicate native in-class initializer for that field.

Struct designated construction is resolved in semantics to a compiler-owned,
full-field construction declaration that is not a source-callable overload.
Its transfers use the same ownership analysis as explicit moves, by retained
compiler-generated expression identity rather than a public spelling match.
HIR receives declaration-ordered, exactly typed inputs; omitted generic field
defaults are concretely reanalyzed in their declaring class context. MIR uses
the existing prepared-argument and `Construct` schedule, including rollback
if evaluation of a later input fails. The C++ backend consumes those MIR slots
and does not interpret designator names or AST field lists.

Contextual collection lists retain a `ResolvedCollectionInfo`: the exact
collection/element types and the selected `collection_init` and
`collection_insert` declaration identities. Semantics validates this public
owned-value protocol without recognizing any standard-library class by name.
HIR builds a chain of ordinary static calls with ordered ownership inputs,
starting with the written count and then transferring the accumulated owner
and each next element. These values share the original list's source anchor
and full-expression lifetime. Existing MIR call staging, effects, failure
edges, transfer, and cleanup own execution; the backend never interprets raw
collection braces. Program-initialization effect analysis also records the
selected protocol calls and conservatively rejects unclosed generic effects.
Even cleanup-free intermediate collections get concrete temporary lifetimes
so a protocol call must publish successfully before the next element starts.

A concrete by-value parameter pack retains one binding but a distinct
`HirDropObligation` for each lexical-drop element. Each obligation records the
binding, concrete pack index, exact element drop type, and source-order
construction ordinal; it begins active at body entry. Trivial elements have
no drop row. MIR consumes these element identities rather than relying on
native parameter destruction for unused or partially forwarded packs.
Read-only reference pack elements retain their exact reference type and a
`ReadBorrow` expansion role. They have no element ownership or drop obligation;
MIR projects their existing parameter carrier and stages that borrow at the
selected call. A reference element cannot acquire a copy/move role from the
referent's traits.
Concrete function-instance signatures flatten pack elements in order, including
zero elements, whether discovered through a scalar call, forwarded pack, or
virtual declaration. This does not flatten the body's source bindings: a named
pack still owns one grouped binding and its per-element cleanup rows. Thus a
virtual root and override share the same concrete signature without depending
on which call first caused either instance to be discovered.

Each closed payload enum carries a pointer-free lifecycle table in HIR: exact
ordered variants, exact field types, a per-field active-cleanup bit, folded
semantic traits, and the enum-wide active-cleanup bit. Payload construction
values still name only semantic enum/variant identity and ordered operands;
the table is the phase-owned proof used to decide whether those operands and
the produced enum value require transfer/drop obligations. HIR never derives
that policy from a backend representation or alternative name.

For the admitted direct by-value callable family, HIR uses the ordinary
ordered-input schedule rather than a payload-specific call node. A direct
active enum selects `CopyValue` only when its folded traits permit copying and
otherwise requires an explicit or fresh move path. Results retain the exact
enum type and ordinary callable identity. An admitted ordinary class or struct
field retains that enum type in its existing initializer and lifecycle rows;
the containing class's folded move/drop policy is the transport authority, not
a new payload-specific HIR operation. Concrete generic owners and ordinary
storage-backed wrappers preserve the same exact fields, ordered inputs, and
drop descriptors; a generic type argument alone is not a stored field.
This keeps callable selection, containment eligibility, and
copy/move policy in the frontend while leaving concrete field publication and
result lifetimes to MIR.

The execution profile is immutable program metadata, not a body operation.
HIR preserves it for later concurrency boundaries but does not repeat the
global/static validity check owned by semantics or infer runtime capabilities.

## Program Initialization And Hosted Entry

HIR copies the semantic `ProgramInitializationPlan` into one
`HirProgramInitializationPlan`; it does not reconstruct order from discovered
instances or module-body iteration. Each namespace global and non-generic
static field has one exact binding in the merged module body. `DataOnly` steps
have no module statement, value, or executable initializer root. `Initializer`
steps have one exact statement/root and must retain the nonzero source-backed
initializer value plus its source plan identity. Static fields of generic
classes remain in their concrete class instance bodies and are excluded from
the program-wide plan.

An exact frontend-computed program constant becomes a
`programConstantSubstitution` value carrying the same `ConstantValue`,
`ExpressionInfo`, source expression, and type. This is the downstream proof
that a later data-only storage declaration need not be loaded early; the flag
cannot be inferred from the binding's constant metadata. Every other reachable
module value remains an ordinary storage or executable value.

`HirLowerer` independently recaptures the supplied Program/target identity,
matches the semantic analysis seal before seeding instances, and runs
`verifyHirProgramPlans` before exposing a valid result. The verifier exact-
compares the semantic and HIR seals, initialization and hosted-entry plans,
module binding/statement/value inventories, full representation-relevant
binding metadata, initializer provenance, and constant-substitution facts.

For a library request, `HirEmbeddingPlan` maps every selected semantic
`FunctionId` to its one concrete non-generic free-function instance and copies
the canonical symbol, qualified name, signature, ABI version, and request
span. That instance is the future wrapper root; ordinary declaration seeding
may still retain other source functions for the current whole-program model,
but they do not become public exports. Plan verification exact-compares the
semantic row and concrete instance, requires strict public-symbol order, and
rejects a hosted entry or native callback adapter in a library snapshot.

For an owned-argument entry, `HirHostedProgramEntryPlan` maps the exact semantic
entry, append function, vector constructor, and string constructor to concrete
HIR instance identities. It also copies the `main` anchor and exactly two
adapter-local failure operations. Owner generic arguments remain in the
constructed type/class instance, not in constructor-local argument lists.
Hosted construction and an ordinary source call of the same constructor thus
share one concrete identity. HIR and MIR plan verification reject spurious
constructor-local arguments on the selected hosted constructors.
The two adapter-local failure operations are native negative-count validation
and checked GTI count conversion. Allocation failure remains owned and sited by the exact
constructor or append callee; `allocation_failure/hosted_arguments` is reserved
and currently unproduced. This plan is immutable data only. A later M-FAIL-01 /
M-EXEC-01 cutover must generate the executable hosted-startup HIR/MIR schedule,
ordered around program initialization and final parameter transfer, without
re-siting callee failures.

## Instance Discovery

Lowering seeds non-generic declarations and then drains growing worklists.
Processing a field, call, operator, constructor, destructor, lambda, return, or
parameter type can discover another concrete instance. A fixed pass over the
initial declarations is therefore incorrect.

Canonical discovery is bounded to 64 new-instance levels along one request
chain and 16,384 distinct generic class/function/constructor/destructor instances per
HIR request (ordinary or staged constant closure). Reusing an exact instance
does not consume another discovery level. The bounds are deterministic compiler
resource limits, not a proof that polymorphic recursion terminates: a runtime
condition does not stop concrete body discovery. Exceeding a bound reports
`GTI-S2105` at the requesting source, relates the declaration, and stops the
worklist before reanalyzing deeper bodies. The inventory includes generic-owner
members and empty-pack generic declarations; ordinary declaration roots do not
consume it. Ancestry remains intact when generic discovery reaches a new
nongeneric instance, rather than resetting at a bridge. Ordered depth/provenance bookkeeping
belongs to discovery, never to instance identity or backend representation.

Declaration seeding also closes over nondependent type components in generic
function and constructor signatures, fields, and bases. A concrete return such
as `pair<size_t, bool>` needs a class-instance identity even if its generic
owner is never instantiated: retained declarations still refer to that type.
Dependent components remain symbolic, and signature discovery does not enqueue
the unused generic owner or its executable method bodies.

Before a class type enters the worklist, lowering asks the semantic model for
its exact-specialization identity recursively. A matching primary application
therefore enqueues the specialization's distinct `ClassId`; a nonmatching
application enqueues the primary plus its concrete arguments. Resolution
builds a complete canonical key only for a class whose primary has a recorded
specialization. Reusing an existing class instance does not rediscover its
arguments, which were discovered when the instance was created; discovery is
monotone. Only a first instantiation site still propagates to them. Both rules
keep a deeply nested generic, such as polymorphic recursion through `Box<T>`,
proportional to its size rather than its depth squared. Before them, the
`a06` reproducer took about 20 seconds to reach `GTI-S2105`; it now takes
under one. HIR does not
compare specialization syntax, rank candidates, or defer selection to C++.
Each canonical generic class instance also retains the first source span that
requested it. That span is diagnostic provenance for concrete semantic
reanalysis, not part of instance identity or backend representation.

Generic declaration defaults are exhausted before an instance reaches HIR.
The worklist therefore sees the same complete ordered type/value identity
whether an argument was written, inferred, or filled from a default. HIR does
not store default syntax, evaluate dependent arithmetic, or ask a native
template to complete an instance.

A signature-pattern primary retains its public signature arguments in class
instance identity. Semantics publishes the resolved path from each extracted
type/pack parameter to that argument; HIR uses these projections for ordinary
field, constructor, and method substitutions. It does not repeat pattern
matching or flatten the public signature into a different class identity.

`HirInstanceIndex` (`include/gti/hir_instance_index.h`, compiled in
`src/compiler/hir.cpp`) answers "has this instance already been discovered?"
in constant time. It is a lookup structure only: the ordered instance vectors
in `HirProgram` assign identity and are the sole thing iterated, so instance
numbering and emitted output do not depend on it.

Concrete class instances retain substituted bases, fields, kind,
abstract/polymorphic state, transfer/share facts and nominal policies, virtual
roots, and structured base/field initializers. Function instances retain exact
linkage, external symbol where applicable, resolved dispatch identities, and
callable/borrow summaries. A global borrow summary carries its symbol root and
projection path in `BorrowOriginPlace`; call values copy the same metadata and
qualify the corresponding `PlaceKey` into their concrete body domain. HIR does
not infer this place from the callee body. Each `HirLambda` retains its exact
concrete semantic type, including the lexical declaration, physical
signature/captures, and enclosing generic identity. Function-instance indexing
and callable-target selection compare this full type; matching only a lexical
lambda ID or closure
shape would merge distinct generic bodies. Capture records retain the source
and environment-binding symbols, copy/move mode, type, traits, and initializer.
Each closure-producing `HirValue` carries its initializer operands in written
left-to-right order; an owned capture is an ordinary HIR `Move` with the same
place/ownership event used elsewhere. Ordinary by-value closure arguments use
the same ordered prepared-parameter schedule as class arguments: copying a
copyable local or field creates a fresh owned stage, while moving retains the
exact source ownership event. This applies to ordinary calls as well as packs;
the stage, rather than an untracked backend copy, owns argument cleanup.
Eligible read-only overloaded operators and ordinary immediate methods on a
semantically admitted single-owner borrowed carrier use
`ReadTemporaryBorrow` or `MutableTemporaryBorrow`. HIR preserves the selected
producer, its stored dependency and access, exact call target, receiver mode,
and full-expression lifetime; MIR establishes the distinct physical shell loan.
Already-supported read-only operator observations outside that contract retain
their ordered `Value` receiver input. HIR gives that source carrier an explicit
full-expression lifetime slot while retaining its existing dependency loan;
the receiver stage is only an identity, not a copy or ownership transfer. It
does not itself grant a temporary receiver borrow or permit mutable access
that semantics rejected.
Callable parameters and call values
use an explicit boundary enum rather than a generic "non-escaping" boolean. The
`Confined` boundary retains invocation/forwarding requirements. A confined
parameter may additionally retain one exact same-type return transport when
all signatures are repeatable and the body returns only the exact moved
parameter. The bounded `Owned` boundary retains either an exact same-type
generic return or an exact generic owner/field destination, after semantics has
required an explicit source move. Concrete lowering substitutes the exact
callable and destination types, drops inapplicable symbolic contracts, and
retains constructor parameter-to-field move evidence. Confined
signatures retain exact `void`, `bool`, or context-supplied non-reference value
results without tracked borrowed state or lambda identity after concrete
generic substitution. Every signature carries its required read/mut/once
invocation capability and the concrete selected lambda or class-operator
capability. Callable call values carry the selected capability independently
from their confined boundary. Callable parameter records are canonicalized by
parameter index before crossing into MIR. Forwarding summaries are relations
between a source parameter and a concrete target parameter, so branch-local
source call sites that resolve to the same relation are collapsed after
semantic move-state analysis has proved their mutual exclusivity. `Once`
requirements reach HIR only after direct invocation or transitive forwarding
has an explicit ownership move; a selected trailing-`&&` call operator remains
distinct from reusable read and mutable targets. A
program-entry function additionally retains its semantic entry kind. The
owned-argument form turns the exact resolved source-defined append
`FunctionId` into a concrete `HirFunctionInstanceId` for the canonical
vector/string specialization; HIR does not rediscover that operation from
`std::vector` or method spelling. This target is a program-root reachability
edge even though the user `main` body contains no source call to it.

A concrete `[[c_abi]]` class instance additionally retains its semantic record
marker and ordered field-layout facts. HIR does not recompute padding, consult
LLVM, or infer C ABI eligibility from the emitted C++ representation.

An exact contextual conversion from a GTI function to a named native callback
becomes `HirValueKind::NativeCallback` plus one interned
`HirNativeCallbackAdapter`. The adapter contains only the concrete function
instance and complete `SemanticType`; it contains no C++ name, calling-
convention spelling, or executable thunk body. `verifyHirProgramPlans`
exact-checks the source conversion, function declaration/instance, ownership,
linkage, parameters, result, and every value-to-adapter reference. This makes
callback reachability a concrete-instance edge rather than a backend lookup by
function spelling.

Native value invocation is a separate `HirValueKind::Call` family. Semantics
records `ResolvedCallInfo::nativeCalleeType` and any enclosing generic callable
boundary; HIR preserves that exact signature without manufacturing a selected
GTI declaration or adapter target. Its receiver is the callee value, staged as
a passive value before the ordered arguments, and its invocation capability is
read-only. HIR plan verification checks the signature, result, parameters,
receiver, lexical foreign-pointer unsafe effect, and absence of unrelated
static/lambda/adapter targets. Concrete generic callable signatures may select
the exact native type as an alternative to a function or lambda target.

A selected GTI-only namespace function becomes
`HirValueKind::FunctionItem`. Its `SemanticType::FunctionItem` contains the
nominal declaration ID and exact signature, while `functionTarget` names the
one concrete `HirFunctionInstanceId`. It has no operands, capture environment,
native adapter, or backend address. HIR plan verification requires the target
to be the same body-bearing, GTI-linkage, owner-free namespace function and
exact-compares result and parameter types. This preserves declaration-order-
independent semantic selection without making an overload set available to
later phases.
Unambiguous names use the same record outside contextual calls. Their zero-state
values can be ordinary bindings, captures, fields and value-pack elements.
Calling a function-valued field does not make the containing object a method
receiver: the callee operand retains evaluation while the selected namespace
target remains receiver-free.

An exact trusted thread-spawn call creates one `HirThreadTaskAdapter`. Its
identity retains the concrete task type, the exact spawning `HirValueId`, and
the required outer `Once` invocation capability. It also retains exactly one
concrete `HirFunctionInstanceId` or `HirLambdaId` task entry plus the selected
read, mutable, or consuming receiver capability. A function-item task uses its
already selected namespace function instance. The spawn value simultaneously
retains the ordinary owned callable transport and
`SynchronizationOperationKind::ThreadSpawn`; explicit and automatic join
values retain `ThreadJoin`. Program verification requires every adapter to be
nonzero, type-complete, referenced by its exact spawn, rooted in that exact
function/lambda instance and owner type, and ordered deterministically. HIR
does not erase the task, invent a native entry point, reopen `operator()`
selection, or recognize the source-defined `std::thread` wrapper.

## Executable Values

Same-type fresh-result construction and class-value grouping are non-executing
source aliases. `HirDirectInitialization` retains each wrapper's source,
immediate producer, exact concrete type and shared `HirValueId`. A construction
also retains immutable provenance issued by its selected concrete semantic
model, binding the source, immediate producer, exact type, and program snapshot;
grouping has no construction proof. A class-instance ID alone is not evidence
that an expression denotes construction. Lowering returns
the producer's existing value rather than adding an Identity/Construct value
and competing drop obligation. Full-expression roots, initialization plans and
range plans resolve these aliases with `HirBody::valueForSource`; the public
source-value index also retains every wrapper. HIR verification rejects a
missing/retyped producer, a forged selected-construction decision, a transfer
disguised as fresh construction, and a mismatched shared value identity. An
ordinary/lambda/deferred invocation or executable outer value cannot be erased
by adding an alias, even when its result and argument have the same class type.
This includes nominal call operators and dependent branches whose primary model
has no selected-call record: concrete construction provenance remains required.
Grouping an explicit move stays transparent without making that move fresh.

MIR consequently receives the same actual producer and initialization/loan/drop
schedule as the unwrapped expression. It verifies those executable operations
and failure/publication edges normally. There is no new backend elision choice
or source-only executable path. The constexpr compatibility adapter likewise
forwards to the existing producer evaluator; staged MIR erases the wrapper
before execution, rather than implementing another constructor rule.

HIR bodies retain source operand vectors and attach semantic type/category,
access, ownership, selected call/operator/constructor, intrinsic,
synchronization, dispatch, unsafe, move, and borrow facts to explicit values
and statements. A synchronization record names a thread spawn/join, atomic
load/store/read-modify-write/compare-exchange, or mutex lock/unlock operation.
Atomic records additionally retain the selected success/general order and,
for compare-exchange, failure order. The record is backend independent and is
attached only after semantics resolves a trusted private capability; HIR does
not infer it from `std` wrapper names or ordinary call spelling. Generated
range operations and constructor initialization use the same resolved call
records as ordinary source.

When semantics marks an exact named return as an implicit move, HIR preserves
the source place value and adds one unary `HirValueKind::Move` as the return
root. That value carries the same ownership event, exact place identity,
borrow transport, and intrinsic identity as explicit `std::move`; HIR does not
reinterpret the return spelling or infer last use. Copyable concrete generic
instances retain their ordinary place value and therefore remain copies.

For the bounded named-Expected payload range, `HirRangeIteration` carries
`expectedPayloadProjection` in addition to its exact source value and range
loan. Verification requires that marker to equal the semantic range fact, the
source to be exact `ExpectedValue` with receiver-origin read access, and the
hidden owner to be reference storage. A backend therefore cannot authorize a
general retained Expected reference by rediscovering source call syntax.

For a selected call or construction with omitted arguments, HIR lowers the
semantic model's exact default-expression suffix at the caller after every
written operand. The expressions use the selected target instance's concrete
semantic model and become ordinary typed `HirValue`s in the call's
full-expression. `HirCallArgument::defaultArgument` preserves provenance in the
ordered call plan. Constructor-initializer records retain both the complete
lowered argument vector and `explicitArgumentCount`, including implicit base
construction through a constructor callable with zero explicit arguments.
Neither HIR nor a backend relies on native C++ defaults.

A class binding initialized from an exact `nullptr_t` constructor remains one
ordinary construction in HIR. The source literal owns the constructed value;
lowering adds one source-less `nullptr_t` operand so MIR retains the selected
constructor's real argument type. A selected member `operator=` is represented
as an ordinary call with an explicit named-place receiver and exact right-hand
argument. Neither form is inferred from a public standard-library type name.

The bounded M-EXEC-01 invocation slices give an eligible ordinary call,
resolved class operator call, or ordinary constructor a `HirCallPlan`.
Eligibility is deliberately narrow: the
target is concrete and non-intrinsic; argument cardinality exactly matches the
selected parameters after caller-side default expansion; no pack expansion is
present; and every
parameter is either a supported scalar/reference form, an exact class value
without borrowed state, or the narrowly scheduled borrowed input described
below. Reusable mutable temporary permission remains confined to
ordinary methods and reusable `operator()`; consuming value operators also
receive a temporary stage with exact `Once` capability. Non-borrowing read-only
operators selected on an admitted fresh temporary also receive a plan so MIR
does not reconstruct their receiver. Lambdas, unresolved deferred callables,
and unsupported construction remain outside the slice. A concretely selected
`operator()` or namespace function item uses the same exact target even when
its generic source call still has a deferred-callable base record; the
instance's selected signature, not that inherited record, plans each argument.
A derived class object for a base-reference parameter is therefore the same
ordered borrow input as in a direct call. A planned receiver is
a read or mutable borrow for a place, a value for a reusable value receiver, or
a `MoveValue` for an explicitly moved receiver. Ordinary consuming methods
and value operators retain the same exact invocation capability. A fresh
owned receiver uses `MutableTemporaryBorrow` plus `Once` capability: it aliases
the full-expression materialization instead of synthesizing an ownership move.
Conditional receivers retain their existing join slot. `std::move(this)` has
an ordinary Move value with an exact whole-This ownership event; HIR does not
infer it from a returned name or from the selected backend.
An exact immediate nominal or stored-lambda field invocation authorized by
semantics inside consuming `this` instead uses `RetainedReceiver`. Its Move-shaped HIR value
records `retainedReceiverProjection` and creates no new drop obligation;
transparent grouping preserves that fact. MIR stages the exact field place
without physically moving it or transferring the enclosing receiver's cleanup.
Stored-lambda calls with fixed arguments retain an ordered call plan; concrete
pack calls stage the same retained receiver before expanding their arguments.
For the proved transparent unique-owner accessor form, `retainedReceiverOwner`
names the already-lowered accessor receiver separately from the returned loan.
Lowering does not re-evaluate the owner expression. MIR turns this identity
into the retained owner anchor while keeping the accessor result as the actual
call receiver.
An admitted direct class-value temporary instead uses
`ReadTemporaryBorrow` or `MutableTemporaryBorrow`; the input names the
unwrapped producer value and copies semantic access rather than rediscovering
freshness from `HirValueKind`.
For an ordinary call, each index recorded by semantic analysis as an ephemeral
argument borrow is likewise lowered as `ReadTemporaryBorrow`, with the
unwrapped producer, exact read-only parameter type, written parameter index,
and enclosing full-expression identity. HIR rejects duplicate, omitted, or
out-of-range semantic indices and never infers this role from a class-shaped
argument on its own.
An ordinary call or constructor may instead receive `MoveBorrowedTemporary` for an
exact fresh GTI call result whose class value carries borrowed state. This is a
transfer into the selected parameter, not a copy and not a loan of
the producer. The input retains the producer, parameter index, concrete
parameter type, and full-expression identity so MIR can reparent the one
source drop to the parameter and verify single destruction. The external
owner dependency remains separate from the transferred carrier's cleanup.
This bounded route permits a move-only iterator result such as
`source.begin()` to initialize an inferred constructor parameter, and a fresh
borrowed-view factory result to pass directly to an ordinary helper, without
weakening ordinary class-value admission.
This preserves a once-callable requirement even when exact overload selection
legitimately falls back to a read or mutable target. Ordinary constructors
require an exact constructor target and at least one parameter;
generated/default zero-argument construction and copy/move special construction
remain unscheduled. The plan
names a function receiver, when present, once and records each argument in
source order with its exact concrete parameter type. Constructors have no
receiver. An input role is value, class-copy value, class-move value, read
borrow, mutable borrow, read temporary borrow, mutable temporary borrow, or
move-borrowed temporary. A
class place is eligible only when it can be copied;
a class value is eligible only when it can be moved. The operand vector remains
source provenance; MIR consumes the plan as schedule authority for these
slices.

A contextual `operator bool` is an explicit synthetic HIR call whose receiver
operand is the already lowered source expression. It has its own target,
dispatch, failure, receiver, and call-plan identity. When one source expression
therefore produces both a value and an outer conversion call, full-expression
root mapping selects the outermost lowered value and reaches the producer
through its operand. MIR never synthesizes a contextual call from side
metadata.

`HirValueKind::PackFold` is a separate bounded schedule, not a request for HIR
to interpret a general fold expression. Semantics has already selected one
non-overloaded free generic `void` target and proved that the source
pack occurs once in the target's read-only element position. The concrete HIR
value retains that declaration identity, the source pack and argument
position, its fixed named-place operands, and one exact resolved element-call
record per substituted pack type in source order. Each element record carries
its concrete type and selected function-instance identity. An empty pack is an
explicit fold value with an empty element sequence. HIR neither reopens
overload resolution nor derives element calls from emitted template behavior.

This bounded invocation-input plan is not yet a complete executable schedule.
Borrowed-state class values, whole-pack forwarding and general pack expressions
beyond the bounded fold, unresolved callable calls, operators other than
`operator()`, special/default construction, conditionals, target-place
formation, result destinations, and generic static initialization remain
outside this particular schedule. The source-graph initialization plan is
separately lowered into the verified `Module/0` MIR body.

M-LIFE-01 maps each AST full-expression root selected by `SemanticModel` to a
snapshot-local `HirFullExpressionId`; HIR does not infer endpoints from
`HirStatementKind`. Lexical bindings and materializing values whose concrete
type requires cleanup carry typed `HirDropObligation` records; class records
retain the exact concrete destructor identity and whether their own declared
cleanup needs active-drop state. These records describe lifetime and cleanup
authority, not an evaluation schedule. Under the accepted D-EXEC-01 contract,
later M-EXEC-01 slices must extend named concrete child roles to the remaining
expression families, retain destination-materialization intent, and add the
semantic program-initialization step identity before those families are
linearized into MIR. HIR does not select source endpoints, repeat borrow
validity, or infer that vector position alone is sufficient for every
expression kind.

An explicitly defaulted default constructor retains its written declaration in
semantic selection, but HIR uses the same generated lifecycle route as an
implicit structural default: normally no body target, or the explicit
source-free generated-base schedule described above when eligible.
A defaulted destructor creates no `HirDestructorInstance`; only a
destructor with a source cleanup body has executable HIR and active-drop state.

`HirValueKind::LayoutQuery` preserves layout-query provenance while carrying
the semantic model's exact `uint64_t` constant and numeric literal value.
Lowering neither recomputes the target layout nor turns the operation into a
backend query. This distinct HIR kind lets structural tests prove that the
frontend-owned result survived the syntax/semantic boundary.

Defined wrapping, saturating, and checked-result arithmetic remains an ordinary
`HirValueKind::Call` whose resolved intrinsic identity distinguishes add,
subtract, multiply, and all three modes. Constant public-wrapper calls carry
the exact semantic integer or checked-result constant. HIR does not replace
these calls with native overflow behavior or reinterpret them as the checked
built-in operators.

Floating HIR values retain `float` or `double` semantic type plus an exact
GTI-owned `BinaryFloat` bit pattern for constants. Mixed operations already
carry the selected binary64 result type from semantics; HIR does not repeat
promotion or consult host floating behavior.

An overloaded assignment has a synthetic receiver value because its left
operand is represented by the assignment path rather than a standalone AST
expression. Semantic operator resolution therefore records the exact receiver
`ExpressionInfo`, including the dereferenced type, access, and nominal traits.
HIR copies that record and any resolved semantic place directly; it does not
reconstruct receiver meaning from the declaration symbol or dispatch-owner
spelling.

For exclusive reborrows, HIR copies the semantic child-loan identity, mutable
parent identity, stable source place, access mode, and selected child endpoint
set into each concrete body. It does not rediscover place conflicts, decide
whether sibling children are disjoint, or decide when a suspended parent has
no active children remaining; those are resolved semantic facts carried for
MIR lowering and verification.

For a by-value parameter whose concrete type directly stores one reference,
`HirLoan.kind == SemanticLoanKind::StoredValue` records that the parameter is a
carrier shell for an existing external dependency, not a reference parameter
to the shell itself. The loan is an entry loan, carries the parameter binding,
and retains the semantic access, place identity, and exact designated stored-
reference field symbol. A checked carrier or exact stored-referent return keeps
`BorrowOriginKind::Argument`, the exact parameter index, and the returned
value's concrete type. HIR does not turn a shell field or projection into that
stored dependency; MIR lowering must transfer the entry loan explicitly, and
MIR verification rechecks the transfer.

References to carriers retain a separate `HirLoan.storedDependency` edge to
the semantic stored-owner loan. The physical reference's place/access and
parent remain distinct from that inherited authority. Body extraction closes
over both parent and dependency identities; receiver-only abstract entries are
selected by their declaring function or source destructor, while function/lambda
reference entries are selected through their concrete formal bindings. Lifetime endpoints sort
both edge kinds child-first. HIR preserves these facts for independent MIR
verification rather than deriving them from a carrier's backend layout.

A source destructor receives the same caller-owned reference-field entry as an
ordinary method. Its declaration identity selects the body; its concrete class
and mutable receiver provide the physical place. This entry does not acquire
another cleanup obligation for the object being destroyed.

For shared multi-path carriers, `HirLoan.additionalDependencies` preserves
every semantic owner place or opaque parameter field path in the same concrete
place domain. Constructor instances retain `borrowParameters`; call values
retain `borrowArguments`; function instances retain `returnBorrowParameters`.
These complete origin sets supplement the primary origin without fabricating
a merged storage root. MIR independently checks their publications and all
owner conflicts; neither HIR nor the backend infers a public wrapper's meaning.

Global-origin call values likewise carry the exact semantic place and access.
The semantic full-expression or retained-loan endpoint remains authoritative;
HIR neither treats static storage as a receiver nor replaces its symbol with a
backend address.

M-OWN-02 gives every concrete HIR body a `PlaceDomain` with one process-local
frontend-snapshot generation and a deterministic body ordinal, then lets
`HirValue` carry the semantic `PlaceKey` and ownership event selected for that
source operation. The generation prevents stale facts from separate analyses
from comparing as one domain; it is not persistent artifact identity. Body
qualification distinguishes concrete generic/function instances while
retaining the resolved root, field, constant-index, or conservative
dynamic-index projection. `HirLoan` uses the same key type. HIR does not rebuild
a key from value shape, repeat the source ownership checker, or answer overlap
differently. This first implementation carries fixed-array
read/move/reinitialization facts. Dynamic-index precision and general lifetime
epochs remain future proof work; they are separate from the explicit
normal-exit temporary/drop obligations.

HIR retains syntax provenance needed for diagnostics, lowering, and
optimization-coherence checks. `HirProgram::sourceValueIds` may map one source
expression to several concrete generic values; a source-level optimization
replacement is valid only when all concrete instances agree. Backends do not
receive this map: `LoweredProgramBuilder` consumes any required construction
evidence before publishing the backend contract.

Function and constructor instance keys retain type arguments and `uint64_t`
value arguments separately. A constructor key combines its selected
declaration, concrete owner, and ordered constructor-local arguments; its row
also retains substituted parameter types and the first instantiation site.
MIR function and constructor rows and the detached lowered inventory copy those
arguments and parameters so later verification never reconstructs inference
from native layout. The HIR-to-MIR and MIR-to-lowered coherence gates compare
the complete vectors, including a function value that does not occur in any
runtime parameter type.
Contextual braces have already become a concrete
fixed-array `HirValue` under the selected parameter type; HIR does not infer
their element type or extent. Forwarding one symbolic array extent to another
generic declaration preserves the parameter identity until concrete
substitution, so distinct extents cannot collapse into one instance.

[Execution §4.10](../language/execution.md#410-defined-runtime-failure)
requires each concrete checked detector to retain an exact bounded set of local
failure category/details and a canonical source anchor. Semantics remains the
producer of possible outcomes; HIR binds them to concrete detector operations
without choosing control-flow cleanup. A division or shift may have more than
one local outcome, so a single `mayFail` flag or optional category is
insufficient.

Local origins and propagation are separate facts. A checked arithmetic,
indexing, owner, observer, storage, allocation, or trusted host operation owns
its local outcome set. A source call or future join merely records that it may
propagate an already formed record; it does not acquire a transitive category
set or replace the origin site. Conservative propagation can later be refined
by the one function-effect authority without changing this distinction.

`SemanticModel` now records a `DefinedFailureOperation` for each classified
source expression. HIR copies that operation without reclassifying it. One
operation may carry several `DefinedFailureOrigin` records because one lowered
expression can contain distinct detector sites, such as bounds and arithmetic
checks in an indexed compound assignment. Each origin owns a sorted unique
outcome set and a snapshot-local `SourceUnitId` plus line/offset anchor. Direct,
virtual, constructor, callable, and future task-join propagation remain a
separate enum with no transitive category set.

After HIR, the backend-independent failure-metadata builder consumes those
local origins together with `SourceGraph`, `SourceManager`, and the
direct/project logical root. It applies the canonical pre-optimization rules:
definition anchors coalesce across concrete generic instances, outcome pairs
are unioned and sorted, logical names never contain incidental absolute paths,
and external names use exact source bytes plus the shortest lexicographically
least include route. It then assigns one-based `FailureSiteId` values and
computes the SHA-256 identity over the immutable descriptor serialization.

`FrontendResult::failureMetadata` owns that product. MIR copies it and maps
every local detector origin to its exact site; propagating calls remain
un-sited. HIR still owns only compiler vocabulary and snapshot-local
origin/propagation identity. It does not acquire artifact IDs, failure records,
cleanup edges, or backend representation policy.

## Boundary

HIR owns concrete identity and typed executable structure. It does not decide
source validity, repeat overload resolution, own body-local CFG repair, define
object ABI/layout, or choose a backend representation. New syntax reaches HIR
only after semantics owns its meaning. New body-local dataflow generally
belongs in MIR. HIR is an input to `LoweredProgramBuilder`, not an input to a
production backend.

Current gaps and future instance/backend work are tracked in the maintained
[`docs/plans/implementation-sequence.md`](../plans/implementation-sequence.md),
the [language restriction ledger](../plans/language-alignment.md), and the
[optimization plan](../plans/optimization.md).
