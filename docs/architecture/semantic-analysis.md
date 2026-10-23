# Semantic Analysis

Status: Current implementation.

Member visibility is shared by semantic field/call/constructor/type checks and
completion filtering. `canAccessMember` uses the lexical class context, the
selected declaration's access and owner, substituted inheritance paths, and an
instance receiver's static type. Protected access through a derived context
requires a receiver of that context's type or a descendant; static members and
base-subobject initialization have no instance receiver restriction. Inaccessible
selected members use `GTI-S2007` with their declaration as related information.
The existing `AccessModifier` metadata carries protected sections through AST,
semantic symbols and LoweredProgram. HIR/MIR retain the existing resolved
field/call/constructor identities and execution operations; the declaration
planner preserves section spelling, while lifetime-slot constructor helpers
retain their existing semantic-authorized native-public representation.
Generic constructor initializer matching compares the substituted direct base
with the resolved initializer type during concrete reanalysis. Value-dependent
default eligibility is deferred only after proving an accessible default
constructor exists, so an open base argument cannot manufacture one beside an
argument-taking constructor.

## Unsafe expression permission

Value-producing `unsafe { expression }` uses the existing expression grouping
model with a retained keyword. Semantic analysis enters the same lexical
permission stack as a statement unsafe block, analyzes the operand normally,
then restores permission. Grouping queries keep the exact place, borrow origin,
access, callable boundary, ownership state and fresh-initialization source.
No new function or local storage scope is synthesized. AST cloning preserves
the permission keyword and expansion identity for concrete analysis. Advisory
tracking identifies each region by its keyword; expression warnings omit the
statement-only keyword-removal edit. Constant evaluation reuses existing
grouping and staged MIR execution rather than gaining a second operation rule.

## Local reference inference

`analyzeInferredVariable` resolves written `auto&`/`mut auto&` from the analyzed
initializer's referent type. Reference initializers enter the same loan-establishing
analysis context as explicit references, including mutable-use tracking. Ordinary
locals share `validateReferenceReferent`, `validateReferenceBinding` and
`recordRetainedBorrow`; range/generated locals retain their existing structural
contracts. Concrete generic reanalysis records the concrete reference, not a
second inference rule. HIR/MIR consume resolved binding/access and loan facts;
the backend never asks C++ `auto&` to decide type or lifetime semantics.

## Generic payload enum signatures

Nominal registration inventories class and enum identities and their ordinary
generic binders before resolving defaults and payloads. Demand-resolved class
and enum defaults share recursion detection and avoid declaration-order-dependent
type acceptance. Value defaults retain the existing earlier-visible constant
rule. A concrete enum type retains its
primary identity and complete canonical arguments; existing substitution
produces its exact alternative types. Payload admissibility is revalidated
through aliases after field facts are available. Ownership, cleanup and
concurrency traits fold actual substituted fields, not generic metadata.

`SemanticModel::enumTypeFor` exposes retained exact signature rows to immutable
snapshot consumers. Semantic resolution and concrete instance discovery record
these rows through the existing substitution service; compiler-owned hover and
completion therefore use the same signatures without implementing generic
substitution in tooling. Replacing a primary descriptor invalidates its earlier
signature rows so pre-payload registration cannot leave stale field metadata.
Request reset clears those rows with the rest of the snapshot-owned catalog.

By-value layout traversal distinguishes exact class and enum instances, so
repeated use of a generic primary cannot hide a cycle through different
arguments. Growing class recurrence uses the existing structural nominal-growth
guard; finite decreasing nesting remains traversable. Owning-indirection
metadata is not a by-value containment edge.

## Exact-owner construction

For one completed value of a class's exact owner type, semantic construction
first distinguishes a fresh produced result from a transfer of an existing
place. `SemanticModel::isFreshClassResult` uses the selected construction/call
identities and return type; grouping preserves that fact, but the Move
intrinsic does not acquire it merely from `ValueCategory::Value`. An exact
one-source class construction around a fresh result records
`ResolvedConstructionInfo::directInitialization` before overload selection.
It selects no extra constructor or lifetime and does not require a move
constructor. Compiler-private placement/storage calls are not one-source class
construction expressions and retain their selected lifecycle protocol.

`SemanticModel::directInitializationProof` issues an immutable, source-bound
construction proof only for that selected exact fresh-result rule. It binds the
immediate producer, concrete type and program snapshot; consumers cannot create
a proof for an ordinary invocation by presenting a matching class ID. HIR keeps
the concrete proof when symbolic primary analysis skipped a dependent branch,
without retaining or copying the entire concrete semantic model.

For a named source or explicit transfer, construction still selects that
owner's copy or move lifecycle according to the analyzed value category.
A generic by-value converting constructor instantiated with that
same owner type cannot outrank or resurrect a deleted lifecycle operation;
unrelated argument types retain ordinary inference/constraint selection. The
selected `ConstructorKind`, declaration and argument facts continue through
HIR/MIR. Completed-value optional publication therefore transports the selected
lifecycle value rather than invoking another converting constructor at the
private storage boundary. No public callable/container name defines this rule.
Stored-borrow and raw-field provenance pass through a direct wrapper to its
producer. The wrapper adds no invocation effects or new borrow permission;
ordinary producer validity, unsafe obligations and escape checking remain
mandatory. Tooling retains a selected construction occurrence whose hover
explicitly explains that no additional copy/move is selected.

## Declaration and return construction

An explicitly typed class or struct declaration may have an initializer of
another type that no assignment rule admits. `analyzeDeclarationConstruction`
then selects a constructor for it, through the same
`analyzeConstructorCallArguments` call a direct initializer makes, with the
initializer as the sole analyzed argument. Overload, access, ownership and
default-argument rules are therefore shared rather than restated. In this
contextual mode, selection considers only constructors whose
`ConstructorInfo::implicitConstruction` records a valid `implicit` modifier;
declaration collection rejects the modifier on copy/move policies, packs and
constructors that cannot take zero or one explicit arguments (`GTI-S2121`), and
the parser rejects it beside `unsafe`.
`SemanticModel::recordContextualConstruction` keeps the selection and its site
beside the initializer's own expression, call and construction records, which
still describe the initializer itself. When no implicit constructor
accepts the initializer, the selection's own `GTI-S2012` is dropped and the
declaration reports its `GTI-S2003`, with a hint naming the explicit form;
every other rejection keeps the selection's diagnostic. The construction records its program-storage and cleanup effects,
and conservative raw-pointer field origins, at the declaration, as the direct
initializer's expression would.

The rule runs where the exact `nullptr_t` construction does, for declarations
and field constructor initializers, and explicitly declared value returns
through `visitReturnStmt`. A selected contextual construction/conversion
already produces a fresh destination, so return checking does not reinterpret
the original argument place as a copy of that destination. A return destination
containing open type parameters, including a computed type projection, defers
selection to concrete semantic reanalysis. Concrete instances still enforce the
one-step selection and ordinary access and ownership checks. Ordinary
exact-type implicit return movement is unchanged.
It does not run for `constexpr` or
range-element bindings, reference fields, unions, abstract classes,
borrow-carrying declared types, or initializers typed by their destination
(brace lists, contextually typed calls and function names). Reference returns,
calls and assignment never consult it.

Bare class-valued returns use the same constructor selector with zero explicit
inputs and the implicit-only filter. They do not create a fake expression:
`SemanticEmptyReturnConstructionProof` binds the exact `ReturnStmt`, selected
construction and semantic snapshot. Concrete generic models issue their own
proof. HIR carries it in `HirEmptyReturnConstruction`, verifies the exact target,
defaults, statement result and full-expression root, and emits an ordinary
constructor call with the usual failure/cleanup behavior. Instance-delta resets
clear these records. Unknown raw-address fields stay unknown rather than
becoming trusted because the return had no written argument.

## Conversion operators

`operator Target()` is an `OverloadedOperator::Conversion` member named
`__gti_operator_conversion`; every target shares that overload set, and
`sameFunctionSignature` adds the resolved target to a conversion's identity,
so aliases of one target collide and `implicit` does not distinguish
declarations. `operator bool()` keeps its own `ContextualBool` member.
`validateOperatorDeclaration` admits read-only, `mut` and consuming
receivers and rejects parameters, `void`, references, text views,
borrow-carrying targets and the owner class.

One selection serves every conversion site. `conversionOperatorCandidates`
returns the source class's operators to exactly the target, with every access
and receiver. `conversionConstructorCandidates` runs the constructor
candidate loop of `analyzeConstructorCallArguments` as a probe: it collects the
constructors whose parameter types take the source, whatever access or
ownership selection would then require, and reports and records nothing. A
site whose source declares a conversion to the target and whose target also
has such a constructor is ambiguous (`GTI-S2122`); otherwise
`selectConversionOperator` calls `resolveOperator` with the exact target, and
with only implicit operators for a declaration. Receiver modes, loans, access
and raw-pointer effects are therefore those of any operator call. An explicit
conversion, `Target(source)`, `int(source)` or `bool(source)`, records the
operator on its `Call` or `Conversion` node; `explicitConversionReceiver`
names the receiver for the ownership, constexpr and HIR consumers that
otherwise locate an operator receiver by node shape. A declaration or declared value return records a
contextual conversion on its initializer or operand, in the map that holds a condition's
`operator bool`, and the declaration's sites accept the initializer through
`recordsDeclarationConversion`. Every contextual conversion now records its
target's program effect edge, so a condition's `operator bool` contributes to
initializer-order effects like a declaration's conversion does.

## Use-based accessor selection

An accessor pair (a read-only overload returning `T&` and an otherwise exact
`mut` overload returning `mut T&` to the same referent) is selected from the
use of its result, as the language rule in
[static semantics](../language/static-semantics.md#36-calls-and-overloads)
states. The decision is made before the call is analyzed, because the call
records its receiver loan and checks it against live borrows then; upgrading a
read-only selection afterwards would skip the mutable-loan conflict check.

The enclosing construct therefore supplies the use top-down. A `ResultUse` is
keyed by the child expression's identity and lives in request-session state
for the child's analysis (`ResultUseScope`), so no other expression can see
it. It lists the projections applied to the result, innermost first (a field,
a member call, an operator, or a `Bind` to declared target types such as each
candidate's parameter at an argument's position), and the access the
outermost use needs. Only the forms that can be or pass on an accessor result
consult it (`consultsResultUse`): calls, field reads, subscripts, groupings,
conditionals, unary operators and the comma. `accessorResultUse` walks the
steps over the referent type: a member or operator whose overloads all have
read-only receivers decides `ReadOnly`, one with only `mut` receivers decides
`Mutable`, and another accessor pair continues to its own use. An unresolved
step or a use no construct supplies is `Unknown`, which keeps the mutable
preference, so a missing context can only reject a program the rule would
accept, never accept an unsound one.

Selection is the only change: the chosen overload's ordinary call, loan and
borrow-origin facts flow through HIR and MIR unchanged, and MIR verification
checks them as for any other call. Body-local state capture saves the use,
and a lambda body starts without one.

## Native C scalar target facts

`NativeCScalarKind` identifies a C declaration family independently of numeric
width. `TargetInfo::nativeCScalar` resolves that identity to a numeric value
domain and layout for the selected target, not the host compiler. Accepted
macOS/Linux targets use LP64; Windows uses LLP64. Pointer-sized typedefs resolve
to their primitive C identity, and plain `char` retains its separate identity
with target-specific signedness (unsigned on Linux arm64, signed on the other
accepted profiles). `None`, invalid identities and unsupported targets have no
native scalar facts. A consumer must not substitute an equal-width family.
`fixedCScalarIdentity` records the underlying native primitive of the existing
fixed-width C ABI vocabulary: `int64_t`/`uint64_t` use long families on Linux
and long-long families on macOS/Windows. This is a declaration-identity fact,
not an extra integer-width distinction; consumers can canonicalize genuine
aliases without conflating different native primitives. GTI's `char` code unit
does not acquire C `char` identity through this query.

The trusted prelude declares the `c_*` families as ordinary global aliases.
Their private declarations bind `NativeCScalar` by declaration identity;
application spellings cannot manufacture the capability. Resolution produces a
primitive numeric type, not an executable nominal class. A type retains its
native primitive identity separately from its numeric value domain only when
it differs from that domain's fixed-width C typedef. Genuine aliases therefore
canonicalize, while equal-width `long` and `long long`, or plain `char` and
`signed char`, remain distinct. Exact overload matching, generic instance
identity, HIR hashing, MIR types and native declarations preserve that fact.
Numeric conversions and operations use the existing checked GTI rules; equal
width integer arithmetic chooses the higher C declaration family independently
of operand order.

MIR owns the selected target and validates native scalar identities across its
declaration, aggregate, callable and body type inventories. Lowered-program
verification requires the same target; native headers and C++ representation
consume these verified facts. Bindgen retains the primitive family supplied by
Clang rather than replacing it solely by bit width. Existing fixed-width GTI
types and GTI's distinct `char` code unit retain their meaning.

## Optional function results

Function result-discard policy belongs to the selected declaration. Callable
registration records `[[discardable]]` in `CallableDeclaration`, then publishes
it in `FunctionInfo`. Expression-statement analysis consults the resolved call
identity, never a public function spelling or the result's representation.
Signature queries expose the same fact. This only admits the existing discarded
full-expression path; HIR/MIR cleanup, failure and borrow rules are unchanged.

## Optional raw-address auditing

`RawPointerAuditMode` is an immutable frontend policy (`Off`, `Warn`, `Strict`).
Off is the default and bypasses raw origin/field traversal, result/effect
queries, loop substitution and wrapper assertions. Ordinary raw type/constness,
unsafe permission, checked ownership/loans, and explicit reference contracts
remain mandatory. Audit facts do not authorize HIR/MIR execution or backend
representation. In particular `RawReferenceAssertionInfo` is recorded even
with the audit disabled: the programmer asserts the raw validity/owner relation,
and the compiler verifies the resulting witness loan and escape boundary.

Warn and Strict perform identical inference. Semantic/query workers emit
typed warnings, split into observed violations, potential violations and
incomplete analysis. The frontend promotes these warnings only after the
requested frontend phases for Strict; promotion cannot poison query summaries.
Audit-only conflicts are excluded from mandatory loan endpoint planning and
inspect its completed liveness facts only for diagnostics. No checked loan,
cleanup schedule or native output may depend on audit mode.
Audit-only field-type queries look up existing declaration identities; they do
not register unrelated methods or change mandatory MIR symbol/place identities.

The remaining section describes the optional analyzer, not extra mandatory
conditions for compiling an unsafe implementation.

Local raw pointer flow retains address origins as existing `PlaceKey` values,
with the address source span and originating checked-loan identity when one
exists. These facts travel with semantic scope/value-flow snapshots; joins
union possible origins instead of replacing a known source with unknown
validity. They do not create or extend loans. Unsafe dereference, indexing,
member writes and selected mutable receiver calls consult source availability
and the same checked-loan conflict/last-use machinery as ordinary accesses.
The backend does not reconstruct this rule from native pointer syntax.

An origin's monotone `possible` flag records path/call uncertainty, including
choices where the other alternative carries no known origin. Singleton origin
sets are not completeness proofs. Weak writes, loop substitutions and source
summary imports conservatively retain this flag; exact direct linear copies
can retain definite evidence. Potential facts cannot become demonstrated
violations merely because merging removed duplicate origins.

Class field symbols are established in active declaration order immediately
after member collection, before layout, stored-reference, borrow-summary, or
instance queries can first-reference them through lookup maps. This includes
interleaved static and instance fields; inherited lookup aliases and concrete
instances reuse the primary declaration identity. Hash/address iteration cannot
assign identities later consumed by printed IR. Symbols retain declaration-owned
field types before the catalog is sealed, in every audit mode. Isolated workers therefore retain
canonical projected field identities even when a constructor's class body has
not yet been visited by the caller; worker-local symbol allocation cannot
silently lose a returned field's provenance.

Explicit pointer type calls resolve to `RawPointerConversion` by the target
type, not a library spelling. Erasure adds a type-only provenance record with
no origin place; it grants no lifetime or access fact. Copies, joins and source
result queries retain it alongside the complete storage-origin records.
Conversion/result auditing reports a known contradictory restored pointee.
Lifetime invalidation and witness containment do not treat the place-less type
record as an allocation. Concrete HIR resolves dependent target type calls;
MIR lowers the inert operation to a tightly checked pointer `Convert`.

Symbols retain a projection-keyed raw-field map alongside direct pointer
origins. Field paths use the same resolved `SymbolId` and array projections as
ordinary place checking; designated construction and generated aggregate
copy/move preserve that map. Assignment replaces the affected map subtree,
while uncertain call writes weakly merge possible input origins. Scope exit
and owner replacement mark retained origin facts expired/invalidated, so a
later binding incarnation cannot silently revive an earlier address.
The traversal includes inherited fields and raw-bearing payload-enum
alternatives. Tagged alternatives currently share a conservative origin union
rather than variant-sensitive projections; enum pattern bindings preserve that
union. An observer of a stored pointer does not substitute the live carrier
shell as its origin.

Loop entry seeds temporary symbolic pointer-input origins. Copies and field
flow carry these symbols to actual memory-use sites; reachable backedges
substitute them transitively until the finite origin set closes. A reseat
before use removes the input symbol. Merely carrying an expired address across
the backedge is not itself rejected. Nested callable bodies isolate this
transfer state. Deferred bridge uses retain shared backedge-origin/diagnostic
records, allowing call-contract finalization to demand a fully resolved loop
use without replaying source execution or dropping known lifetime facts.
Namespace raw storage also receives loop-input symbols. Source write effects
through loop-carried aliases defer their destination substitution and weakly
close projected writes over backedge symbol facts before checking deferred uses.

Loop entry also drops the complete borrow dependency set of every writable
binding in scope. The single body visit cannot prove an incoming alternative
for every backedge iteration; a dominating construction inside the body can
establish a fresh set. A binding nothing can write keeps its set: only a move
ends it, after which value flow rejects any read reached through the backedge,
and loan flow keeps a read-only reference's referent unwritable while a later
iteration reads it. Each loop records the bindings it dropped, so a selection
that needs a missing set names the loop boundary in its `GTI-S2067` hint.

Source calls query normalized raw-result origins before caller memory uses and
alias writes. A request-local cache keys the selected function or constructor
and concrete owner/function type and value arguments. An isolated ordinary
semantic instance worker analyzes the body; it does not reenter or reset the
active caller and is not a second execution engine. Results retain separate
raw-field projections. Formal, receiver and global raw inputs substitute the
caller's current origin facts (including omitted default arguments), while
addresses produced from callee-local/by-value storage are expired and actual
global addresses keep their place identity. Global pointer inputs are symbolic
in cached summaries, not snapshots of the first caller's value. Source and
generated-default constructors collect field origins; generated copy/move
construction preserves existing maps. Imported facts retain lifetime-ending
flags and participate in the ordinary checked-loan/write machinery.

Each query worker is a fresh analyzer that copies the caller's current semantic
model and value flow, so a nested query observes exactly the caller's facts at
the demand. Its catalog is a sealed snapshot of the request catalog; because
the catalog is sealed before body analysis and asserted unchanged across every
instance analysis, all snapshots are identical, and a worker takes one released
by an earlier query from the request-local query state instead of
re-registering every callable declaration. Resolving a raw origin's root place
back to its binding compares each candidate binding's declaration key (source
unit, name span, generated lexeme) with the identity's `SymbolRecord`; this is
the relation `symbolForDeclaration` establishes, evaluated without a hashed
lookup per candidate. A root that no live binding names (an ended scope, or
another body's local) still visits every namespace symbol of every unit, and
the per-query model and value-flow copies remain the audit's dominant cost on
library-heavy programs.

The same workers collect raw memory-access preconditions: normalized input
origins, read/write permission, and whether the operation checks loans or only
address lifetime (pointer arithmetic). Import substitutes actual caller facts
before mutation invalidation and checks the caller's existing loan flow, with
diagnostics anchored at the invocation. Requirements propagate through source
helpers and constructors; loop uses retain their original loan-flow context.
Operator resolution supplies a nonowning normalized invocation view while its
exact receiver expression or synthetic receiver place and argument slots are
available. Ordinary, contextual, and projection operators use the same access,
result, write, and invalidation helpers. Projection results retain the selected
member's relative field paths rather than replacing it with the whole referent.

An exact concrete lambda type may carry an access-free fact obtained from its
ordinary body analysis. Inert copying/comparison does not create a raw access;
unknown source, foreign, or non-proven primitive execution prevents that proof.
Source summaries separately record access completeness, so an unresolved nested
call with no currently known origins cannot certify an empty precise summary.
Identity-bound address-conversion/transport and checked-carrier observation
primitives are classified by their semantic operation, not their public name.
Restoring swap/exchange/indexed swap is access-free only for a concrete semantic
relocatable type whose existing structural active-cleanup query is false;
possible source cleanup prevents this proof. Lexical wrapper cleanup alone
does not imply user execution when all of its owned components are passive.
Checked-reference returns collect projected raw fields too, so operator-arrow
and ordinary reference helpers cannot certify an empty raw-field result merely
because the returned carrier is borrowed.

Dynamic access-free proofs use existing resolved virtual-root/member facts:
either a canonical unprojected owning receiver identifies one concrete class,
or every effective target in a sealed nongeneric declaration family must have
a complete, empty ordinary access summary. Open generic families, unknown or
foreign/bodyless targets, recursion, and query limits block the proof. This
waives only access fallback, never unresolved dynamic result/write facts.
Erased callable allocations do not retain a concrete subtype in these checked
places; their open target families therefore remain conservative. Unmatched
carried raw-field paths (for example derived fields behind a base reference)
are checked conservatively, not dropped. No callable effect type system or
second source-execution/call-graph engine is introduced.

The same ordinary assignment visitors collect normalized raw-write effects for
source functions and selected constructors. Destinations are global/static
storage, mapped checked-reference formals, raw-pointer aliases, or method
receivers; values retain the same projected origin records as returned values.
Call-site import runs after conservative mutation invalidation, so a proved
fresh setter can establish a new origin. Exact destinations in a structurally
linear ordinary function body permit replacement; constructor write effects,
conditional control flow, ambiguous aliases, and dynamic indices weakly union
facts. Constructor writes remain weak until initializer dominance is proved;
their returned receiver-field origins are still collected precisely.
Namespace flow is not part of lexical scope snapshots, so writes in caller
branches, short-circuit/conditional expressions, and loops also remain weak.
This can conservatively retain overwritten facts within those regions; an exact
unconditional reset after the region can still replace them.
This bounded linearity check classifies
control structure, not source execution. Failed or recursive write queries
retain unresolved facts on possible raw-bearing destinations. Function,
constructor, destructor and lambda declaration analysis saves and restores
namespace raw-flow state: checking a body is never execution of its writes.

Class static fields are the same program storage. Their declarations live in
the sealed class catalog, so body analysis copies each one into value flow,
keyed by its declaration as its place identity is, when the catalog is
sealed. Qualified and in-class unqualified accesses, root-place resolution,
call-write import, query-worker input seeding and the declaration save/restore
all use that copy, so raw writes never mutate the catalog that query workers
share.

Lambda/deferred calls currently have no exact write substitution; they weakly
mark raw globals and mutable raw-bearing actuals unresolved. Lambda captures
retain their raw fields using capture-binding projections in the ordinary
value-flow map, so later reseating the source binding cannot change the captured
alias. Unsupported callable effects follow carried mutable raw origins to
known raw-bearing destinations, without marking unrelated locals.
Source-destructor effects similarly follow stored mutable aliases and retain
unresolved raw-global writes when source cleanup can run, including value
temporaries and scope exits. These conservative boundaries preserve existing
expired/conflicting facts and permit inert transport; they are not precise
destructor/captured-alias write summaries.

Each independent top-level demand is bounded to 32 active bodies and 2,048
distinct family entries. Its nested summaries never participate in cross-root
reuse. Completed top-level outcomes have a separate 2,048-entry cache; when full,
further demands are evaluated uncached under the same family budget. Thus cache
warmth or declaration order cannot change completeness. Lightweight diagnostic
evidence is interned separately from bounded heavy summaries. Query
workers and staged constexpr workers preserve both request-local query
accounting and the existing constexpr work/depth budget. Their mutual calls
cannot restart either limit; mutable body-analysis state remains isolated.
This changes neither constant eligibility nor the staged execution engine.
Recursive,
dynamically dispatched, failed/budget-limited queries, lambda/deferred-callable raw
results, and raw-bearing source results with expanded parameter packs preserve
an unresolved-source marker. Constructor-relative self addresses and raw fields
behind unresolved stored checked references do likewise; concrete retained
loans are resolved first, without recursively treating borrowed fields as owned
storage. Memory use/borrow assertions report incomplete analysis for these
markers; inert transport and comparison remain permitted. Abstract source dispatch signatures remain
unresolved even without a body; readonly receivers still carry mutable raw
aliases whose possible write destinations must remain unresolved. Native,
statically selected bodyless, and runtime-bound outputs instead retain the
explicit unsafe unknown-address boundary. Unknown outputs never
erase other known or unresolved facts.

Complete, valid aggregate-result queries own their projected output maps;
foreign or unresolved results additionally retain conservative raw input and
reference/receiver-derived origins. Temporary member/index projections use the
ordinary field/index paths. Unsupported raw-bearing expressions keep available
origins plus an unresolved marker, never an empty precision claim. Complete
write summaries also supersede the older input-union fallback: a proved
no-write method must not manufacture a receiver-shell origin in a null field.
Unknown mutable receiver/reference effects still preserve possible writes by
weak union. Scalar pointer results and unresolved effects remain conservative;
these summaries are not a complete interprocedural alias proof.

Explicit `borrowed_from` attributes resolve to existing declaration-relative
return-origin/access facts. Inference cannot replace an explicit contract,
including an invalid one. Normal return checking proves it. A direct raw
dereference returned in lexical unsafe additionally records
`RawReferenceAssertionInfo`: a resolved witness place and checked reference
type. HIR synthesizes that witness input and lowers the assertion to the same
typed raw-borrow MIR operation; no private source declaration or public-name
recognition is needed. The backend validates and consumes the MIR loan.
Known raw-input obligations propagate through source wrappers and receiver/
aggregate-field transport. Reference fields use stored-loan dependencies to
resolve their external referents. Polymorphic raw-assertion bodies report
incomplete optional auditing until their dispatch-family observations are
represented. Source-produced output facts use the bounded queries above;
this does not imply complete campaign or release acceptance.

## Exact structural member signatures

Source member requirements carry a typed receiver, exact parameter types/access,
and an exact owned result. Their synthetic bindings are tooling identities, not
runtime variables or loans. Concept parameters have semantic generic identities;
dependent compound arguments substitute through the existing type machinery and
canonical exact class specialization selector. Eager concept resolution follows
class/specialization registration; primitive constraints needed during class
registration resolve lazily. No formatter or stream spelling selects a rule.

Generic body analysis uses only signatures guaranteed by its requirements,
checking argument and receiver access plus the retained unsafe-call obligation.
Safe structural facts imply the weaker unsafe capability; the reverse does not
hold. The existing conjunction/alternative fact calculation preserves that
direction, including forwarded requirements. Concrete instance analysis selects
the ordinary method and publishes the normal HIR/MIR call. A requirement whose
subject is a dependent compound type, such as a class instance or lambda whose
signature names enclosing parameters, first consults active facts and then
inspects the subject's own declaration after ordinary substitution, using the
same exact signature matching as a concrete type. Only a bare parameter relies
on facts alone; a bare argument that itself needs member lookup fails closed.
Comma-pack fold targets
may carry requirements: each concrete element passes ordinary instantiation and
constraint checking before a resolved fold can enter HIR. Signature requirements
do not create a second executable call path or bypass actual borrow checking.

Semantic analysis is GTI's authoritative language layer. It resolves meaning
over the syntax-preserving AST and records facts in `SemanticModel`; HIR, MIR,
backends, and language queries consume those facts rather than repeating
lookup or type inference.

The public dependency boundary has three layers. `semantic_core.h` owns IDs,
places, semantic types, traits, and basic expression/binding facts.
`semantic_model.h` owns published records, `SemanticDatabase`, `SemanticModel`,
queries, and instance-analysis results. `semantic_analyzer.h` is the narrow
`SemanticVisitor`/`SemanticInstanceProvider` facade. HIR, the frontend result,
backends, drivers, and LSP state include the model or core contract rather than
the facade. Declaration registration, lookup, overload selection,
flow/lifecycle analysis, generic reanalysis, and AST visitor algorithms remain
one request-local authority but compile behind a private implementation
contract. `semantic_analyzer_impl.h` contains the single
`SemanticVisitor::Impl` state owner, nested records, and method declarations;
it is not a public or cross-phase header. `semantic_analyzer.cpp` owns only the
public facade. Body/constexpr visitation, ownership and borrow rules, call
resolution, type/generic resolution, declaration/catalog processing, and
program/value-flow finalization compile in the specialized
`semantic_*_analysis.cpp`/`semantic_*_resolution.cpp` units. Post-check
artifact export selection compiles separately in `semantic_embedding.cpp`.
Snapshot/database operations compile in `semantic_model.cpp`; canonical type
rendering compiles in `semantic_type_printer.cpp`.

## Structural Relocation Facts

Structural relocation is recorded in `SemanticTypeTraits::relocation` as
`Unavailable`, `Structural`, or `Bitwise`. Semantics derives it from primitive
capabilities, concrete bases/fields/captures, generated movement, declared
cleanup, and borrowed-state restrictions, not from a public library spelling
or emitted C++ traits. `std::relocatable` and `std::bitwise_relocatable` are
ordinary source concepts over declaration-bound private constraints. HIR
preserves the trait; MIR replays its structural proof before the backend may
select a bulk relocation. It is independent of assignability, transfer/share,
and the MIR defined-failure proof: an empty-destination transfer does not prove
that replacing a live destination can skip cleanup.

During generic body checking, dependent fields retain relocation guarantees
from their type parameter's declaration and active `requires` clause. The
ordinary aggregate query composes those facts through fields and payload
alternatives; it does not grant unconstrained parameters relocation or upgrade
structural relocation to bitwise relocation. Disjunctions retain only guarantees
common to every branch. Concrete reanalysis and MIR verification still derive
the final traits from actual substituted types.

Lifecycle, cleanup, and transfer/share queries distinguish an exact type cycle
from growth of a generic substitution. A closed non-generic declaration breaks
that substitution dependency: `Wrapper<Envelope>` containing an `Envelope`
which itself contains `Wrapper<Leaf>` is not a growing generic recurrence.
The queries still inspect the leaf's movement, cleanup and concurrency facts,
and retain exact-cycle checks across that boundary. Uninterrupted nondecreasing
generic recurrence remains conservatively bounded. MIR independently derives
relocation from concrete instance identities; it does not accept a semantic
trait simply because a wrapper's name appeared earlier in a query.

`ClassLifecycleInfo::generatedDefaultBaseConstructor` records the exact base
constructor selected for the bounded executable generated-default family:
nongeneric owners/bases, no derived instance fields, and an
exact zero-parameter source base constructor. Construction carries ordinary
constructor-failure propagation; HIR consumes this identity rather than
reselecting a constructor. Independently, `generatedDefaultFieldInitializers`
selects a concrete generated constructor schedule when the ordinary structural
default is already available and all instance fields have declaration
initializers. Those defaults carry constructor failure propagation even without
a source constructor declaration. HIR preserves each exact initializer and MIR
owns partial-object rollback. These facts change representation of already
permitted construction, not source overload availability or omitted-field policy.

## Analysis Order

Named nested classes/structs have explicit semantic enclosing-class identity,
declaration access and an owner's direct nested-type inventory. Registration
recurses through lexical class bodies before resolving members, aliases and
lifecycles; type lookup and constructor lookup reuse those same identities.
Access checks include every enclosing type, and nested method analysis cannot
borrow an implicit outer instance. The first profile admits non-generic owners
and generic children; generic-owner capture requires preserving specialization
arguments in nested identities and is rejected before HIR. HIR/MIR retain
ordinary nominal types and existing lifecycle operations. Lowered declaration
parent/owner identities preserve nesting separately from namespace scope, so
the C++ planner never opens an enclosing class as a namespace.
The C++ representation gives each child an independent namespace-level native
identity derived from its resolved declaration/source identity. Text-planning
views do not mutate the sealed lexical declaration tree. Ordinary layout
scheduling follows bases and by-value fields, not enclosing scopes: mutually
pointing children remain finite even when each outer type stores its child.
Representation friendship preserves semantic-authorized lexical access. Native
constructors are callable by lifetime-slot helpers; semantic constructor access
still rejects unauthorized source calls. Deferred/concrete body contexts retain
enclosing static members without adding an enclosing receiver, and
generic children reuse ordinary concrete-instance naming. These choices do not
alter GTI visibility or introduce an outer object.

`SemanticVisitor::check(const Program&)` is staged because later facts depend
on declaration-wide registration. It currently:

1. registers namespaces, aliases, namespace/class type aliases, enums,
   concepts, and primary class/enum generic binders, then resolves nominal
   defaults against the complete primary identity inventory;
2. resolves concepts and aliases after primary class identities exist, then
   registers canonical exact class specializations before resolving
   inheritance;
3. registers function generic parameters and namespace symbols;
4. collects root native-storage symbols, class members, and summary-only
   namespace-global storage identities;
5. resolves inherited members, stored-reference contracts, and function borrow
   summaries;
6. derives transfer/share facts and validates inherited interface capability
   requirements;
7. records class types and lifecycle facts, validates alias target constraints,
   then seals the declaration catalog;
8. copies namespace-symbol value state into the body-owned value-flow session
   and analyzes declaration bodies in a resumable lexical pass (see "Body
   Order And Demand" below);
9. finalizes callable forwarding and argument contracts; and
10. finalizes the program-initialization plan and semantic occurrences.

After an otherwise valid program check, `Frontend` applies the immutable
artifact `ProgramRequest`. Executable requests admit no exports. A dormant
library request rejects `main` and process-lifetime callback adapters, resolves
each requested qualified name to exactly one non-generic namespace source
function with a body, validates the ADR 026 signature and C-symbol families,
and publishes a public-symbol-sorted `SemanticEmbeddingPlan`. This is artifact
policy rather than GTI source meaning, but semantics remains the sole authority
for declaration identity and boundary-type eligibility. HIR and later phases
consume the selected `FunctionId`; they never repeat lookup by spelling.

Closed executable requests (the explicit `requireEntryPoint` policy) and
embedding library requests enable source-definition checks in the semantic
visitor, including concrete instance reanalysis. Once call/operator resolution
selects an exact ordinary GTI declaration, `GTI-S2106` diagnoses a missing body
at the use and relates the declaration. Unused declarations, pure virtual
contracts, registered runtime bindings and compiler intrinsic definitions are
not rejected. Default fragment analysis still resolves bodyless signatures
without promising executable closure. Function items/native callbacks already
require bodies through their structural eligibility rules.

This ordering prevents declaration-order dependence. A new declaration kind may
need registration, source-unit publication, tooling-symbol creation, body
analysis, and finalization—not only a visitor method.

### Body Order And Demand

Stage 8 walks the program's declarations in source order through explicit
frames: the program list, each open namespace body, each active conditional
branch, and each class member list. Every declaration is analysed exactly once,
in its own position; the frames hold no analysis state, and the
declaration-level environment (program scope, namespace path, open class
scopes, declaration-level loan flow) is rebuilt from them whenever they change
under a paused body.

A callee-body fact demanded by this lexical walk is whether a function returns
a moved by-value parameter, which decides what a by-value borrow-carrier result
(an iterator returned by a generic `find`, for
example) borrows from. A consumer of such a fact asks for the body through
`ensureBodyAnalyzed` instead of reading whatever the walk has produced so far.
Other body-dependent facts, such as raw access/effect summaries and staged
constant execution, use their separately described isolated query services:

- when the walk has not reached the callee, the caller's body-local state is
  set aside, the walk advances through every intervening declaration in order,
  analyses the callee in its own position, and the caller resumes. That state
  includes the caller's `constexpr for` expansion frames and budget, so a
  demand from inside an expansion neither consumes nor resets it;
- a compiler-trusted body the pass deferred is analysed in
  its own declaration context;
- a body already in progress (recursion), concrete-instance reanalysis, a
  demand from outside a function body, and a declaration the walk cannot reach
  (a local function, an inactive conditional branch) leave the fact
  unavailable, exactly as a later declaration did before.

Lexical visibility is a property of position, not of analysis time. Each
declaration carries a lexical order. A namespace global declared at or after
the current declaration is invisible to it (`GTI-S2001`), and a `constexpr`
definition or constructor is available to constant evaluation only when it is
declared before the current declaration (`GTI-S2057`), whether or not a demand
has already analysed it. A body analysed early therefore sees exactly what it
would have seen in sequence: the globals and constant definitions that precede
it, including ones that follow its caller. Concrete reanalysis is not
positional. A generic function, constructor or destructor instance, and a
class instance's field initializers and static assertions, are reanalysed
after the primary pass has checked their names in place, so their dependent
constants see every analysed `constexpr` definition and namespace global. The
isolated staged and initializer-effect workers, which hold no declaration
orders, follow the same rule.

`TrustedBodyAnalysis::Deferred` is a test mode of `FrontendOptions` that
analyses compiler-trusted bodies after the application bodies, in reverse
lexical order. The `body_analysis_order` test proves that it produces the same
diagnostics as the complete pass over the examples and the standard-library
tests, and checks fixtures for both orders of the moved-return generic, global
and `constexpr` visibility across a demand, and mutually recursive demands.
`TrustedBodyAnalysis::Reachable` extends that infrastructure for artifact
builds and editor snapshots/completion. Function lookups during body analysis
record demand in a scoped sink. Deferred trusted functions are analyzed to a
fixed point, in their original declaration contexts. Declaration registration,
all application/package bodies, entry-file bodies, constant definitions and
virtual/interface instance methods remain checked. Ordinary static interface
helpers follow the same demand rules as other static functions. Constructors and destructors are not
deferred. The only cross-snapshot semantic reuse is the bounded reuse of
concrete instance analyses described under "Instance Reuse Across Snapshots";
primary analysis always runs in full.

Concrete generic instantiation may select another primary function body that
nothing analysed demanded, typically a library body reached only through a
dependent call. The semantic visitor completes such a body while HIR runs
(late demand) instead of discarding the analysis:

- HIR asks its `SemanticInstanceProvider` for the body before recording a
  request, and the visitor analyses it in its declaration context through the
  same deferred-body drain, together with every body that analysis demands;
- a concrete instance analysis that consulted a still-pending function is
  analysed again after that function's late demand, so no instance keeps a
  fact the body would have changed. Request finalization never infers
  callable contracts for a pending body, so its own analysis establishes them
  first, as in the pass;
- the batch is then finalized on its own: declaration-fact discovery and
  diagnostics cover exactly the bodies it analysed and the calls, callable
  forwardings, C-thread uses and advisory candidates it recorded, and only the
  source units it extended re-sort their occurrences;
- a late batch only appends records keyed by its own syntax and fills its
  functions' own declaration facts. New symbol identities continue above every
  instance delta's identities (the skipped identities are inert and resolve to
  nothing) and lambda identities above every delta's lambdas, so existing
  deltas, which read through to the base model, stay valid.

Late demand is equivalent to having analysed the bodies in the pass, by the
same order independence the `Deferred` mode proves. Where that argument does
not hold it is not used: a batch that reports an error (invalid primary
analysis never reaches HIR) or analyses a body an initializer's program-effect
closure already consulted marks the request failed, and raw-pointer audits keep
complete analysis. HIR then returns the bodies as explicit semantic requests
and remains invalid; the frontend repeats semantic analysis with a
monotonically growing required-body set, discarding partial HIR before
replacing its immutable base snapshot. At most three such retries precede
conservative complete checking. Metrics report late-demand bodies and time
(semantic time, not HIR time), retries/fallbacks and cumulative semantic/HIR
time. `gti check` retains complete analysis. Artifact raw-pointer
audits run a separate complete diagnostic analysis before the same reachable,
audit-off artifact analysis used without auditing. Strict findings still stop
compilation; successful audit diagnostics are retained, but the audit model
cannot change emitted identities or macro isolation. This deliberately costs
an additional analysis only when an optional audit is requested.
This bounded staging avoids unchecked code generation without
claiming that all generic dependencies are known from declaration-time calls.

Text-return relay predeclaration consults the owning class's registered instance
fields for implicit receiver spellings as well as explicit `this` paths.
Parameter names take precedence; static fields/methods do not nominate a
receiver. This is a candidate contract, not a body-local lifetime proof:
ordinary body lookup and return-borrow validation must confirm the actual
origin, rejecting local shadowing and unrelated borrowed call results. HIR/MIR
and the LSP consume the same validated receiver/argument summary.

`FunctionInfo::completeReturnBorrow` is a separate complete result contract,
not an inferred owner for an empty alternative. The current dependency-free
enum slice collects every structurally reachable explicit return and demands
callee bodies before transporting their facts. Concrete generic result queries
reuse ordinary instance analysis in isolated workers keyed by declaration and
exact owner/type/value arguments. Recursive or unavailable evidence stays
unknown; a callable with no normal return does not obtain a vacuous empty
contract. The synthetic residual return of a prefix `?` contributes its
selected conversion's complete set, with no dependency and the constructor's
alternatives as guards; only a conversion without a complete construction
contract leaves `loanFlow.unknownImplicitResultReturn` set. Explicit
unknown entries shadow base-model facts during concrete reanalysis. These
contracts carry typed alternative inventories and declaration-relative mappings,
never callee-local loan/place IDs. Guarded nonempty mappings remain a separate
completion step with ordinary lifetime/escape obligations unchanged.

A mapping's `BorrowedFieldOrigin` is an argument, the receiver, or static
storage (`Global`, naming the storage symbol in `globalRoot`). A free or static
function's return origin is `Global` when every dependency names the same
global path. A global dependency lives in the shared loan-flow place domain, as
`BorrowOriginPlace` loans do; a snapshot-only domain would make it may-alias
every local. A mapping may name caller storage that contains the referent
(an accessor receiver, an owned element buffer, or an owning handle's checked
pointee) when that storage is reached without a stored reference and stores
none itself. A reference returned by an ordinary call reborrows its declared
origin: construction instantiation unwraps plain `Argument`/`Receiver`
reborrows to the caller's place. A local carrier's `StoredValue` loan relays its
external referent to an escaping return, but the chain must still reach
caller-owned entry authority, and `isParameterDerivedBorrow` accepts a complete
scalar set rooted at the parameter. A consuming method on a fresh or moved
carrier production uses the production's own access, as a fresh-temporary
receiver does, so consuming a read-only carrier claims no exclusive access to
its owners.

Dependent generic type defaults retain their resolved symbolic semantic type.
Only while resolving that default, constraints on open type arguments defer;
name lookup, arity, and closed constraints remain checked immediately. Selecting
an omitted argument substitutes the earlier arguments, then checks nested class
constraints and the defaulted parameter's constraint. An explicit argument
bypasses the unused default. Ordinary symbolic fields do not enter this context.
HIR and tooling consume the already selected, validated type rather than asking
the native compiler to instantiate or reject a default.

`FunctionInfo::ownedPointeeProjection` is separate from ordinary return-borrow
origins. It identifies a bounded transparent accessor whose entire body returns
the declaration-bound private unique-owner borrow of one direct receiver field.
The field identity, private owner type, returned pointee type, and read/mutable
access must agree; static, virtual, effectful, parameterized, inherited-field,
and ordinary reference-forwarding methods receive no such summary. Concrete
reanalysis substitutes the field type before publishing this fact. It is not
inferred from a public wrapper name and does not itself authorize moving through
a reference. HIR preserves the summary and MIR checks it against the actual
accessor body before any consumer may rely on it.

Immediate moved invocation inside consuming `this` may use the mutable form
of this proof. `ResolvedCallInfo::retainedReceiverOwner` identifies the exact
accessor receiver expression, restricted to a writable field-only This path.
Availability consumes that owner wrapper, while execution still uses the
accessor's returned loan. This does not authorize extracting a pointee or
moving through an ordinary reference, and it does not bypass existing aliases.
Concrete HIR maps the retained owner to its already-lowered value identity;
MIR keeps the separate executable referent and lifetime owner.

Class-scoped type aliases use that staged boundary deliberately. Registration
walks active conditional members while tracking access labels and records the
owner declaration before alias targets are resolved. Once primary class IDs
exist, resolution binds each alias to its direct owner, enters the owner's
generic-parameter scope, resolves the transparent target, and installs a
separate type-member map. Constraint checks on these alias targets defer until
the class member and lifecycle inventory is complete; every resolved target is
then validated before catalog sealing, even if the alias is never used. This
separates exact type registration from structural constraint validity without
changing alias identity or making constraints optional.
Value and callable collection consults that map for
same-owner name collisions but does not turn aliases into storage members or
inherit them with base value/function members. Type lookup, concrete generic
reanalysis, and tooling all consume the same resolved target. Type lookup
through a concrete type alias substitutes the containing specialization
at every member step, preserving access checks on the qualifier chain and
source identities for both qualifiers and selected members. Written specialized
type qualifiers use `TypeRef::qualifiedOwner` to retain each segment's generic
arguments (`Box<T>::nested::value_type`), rather than appending them to the final
alias. The ordinary class selection and substitution path resolves each owner;
the same direct-member alias lookup serves concrete aliases and written generic
owners, including staged dependency resolution before member maps are complete.
Owner constraints, source visibility, private qualifiers, and alias access are
validated before a resolved type reaches HIR. Lowered-program
rows retain the source declaration so the C++ backend can emit the alias in its
owning class; no HIR or MIR instruction is introduced because an alias has no
runtime effect.

## Compile-Time Iteration And Reflection

`constexpr for` is expanded by semantic analysis and is gone afterwards.
`visitConstexprForStmt` analyzes the sequence expression, requires a class
type with one type-pack argument list (`compileTimeSequenceElements`), and
defers when the type contains an open generic parameter, using the same
deferral marks as a dependent `if constexpr`. Otherwise it clones the body
once per element with `cloneStmt` (`ast_clone.cpp`), builds the element
binding `[constexpr] auto name = <element>()`, wraps both in a block and
analyzes the block. Every copy's tokens carry an expansion id
(`Token::expansion`), which is part of the declaration-symbol key
(`DeclarationKey`, `SymbolRecord::expansion`), so the declarations of one copy
have identities distinct from the original's and from other copies' while
uses of outer declarations keep theirs. The generated element initializer is
bound to a default construction of the element by `visitCallExpr`. The
expansion is recorded on the statement (`recordConstexprForExpansion`;
`nullopt` means deferred) and the copies are owned by the model
(`adoptExpansionArena`) because HIR and instance deltas point into them. HIR
lowers the recorded blocks as a `CompileTimeBranch`, exactly like a selected
`if constexpr` branch, so MIR and the backend see ordinary statements.

The trusted `std/meta` unit binds two kinds of declaration. Functions in
`gti_internal` (`meta_field_count`, `meta_field_name`, `meta_field_is_public`,
`meta_type_name`, `meta_type_kind`) are intrinsics bound by unit and name
(`trustedIntrinsicKind`); `analyzeMetaIntrinsicCall` records the call and,
when its generic arguments are concrete, the answer as a program constant with
a substitution, so the call never executes; `metaIntrinsicConstant` gives the
constexpr interpreter the same answer under its substitution. Classes carry
`@compiler_type("<binding>")` (`ClassInfo::compilerType`): `meta_list`,
`meta_field` and `meta_type` are ordinary classes the compiler must be able
to name, and `meta_field_list` and `meta_field_type` are computed types that
`rewriteCompilerType` rewrites in `selectExactClassSpecialization`, the
canonicalization point both `typeOf` and `substituteType` pass through, as
soon as their arguments are concrete.

`meta_callable_return_type` projects the exact logical result already recorded
on a function item, native function pointer, function signature or lambda.
Open generic projections retain declaration-bound `ComputedTypeProjectionKind`
metadata in the semantic and lowered class inventories. Native callable
templates replay that projection when checking their signature against the
concrete MIR instance; a mismatch still rejects representation. Their native
carriers expose the MIR logical result as type metadata, never infer it from a
C++ invocation whose ABI may instead return a failure status or use an output
parameter. Native function pointers have no GTI metadata carrier; their
projections are
resolved from exact instance substitutions, not deduced from the C signature
(whose pointer return representation can differ from GTI's logical result).
No new runtime intrinsic or storage is involved. Confined-callable
escape analysis distinguishes a projection operand from stored callable state;
ordinary concrete reference origins and escape checks remain authoritative.
A contracted reference projection is checked as its resolved reference type;
references in the query's callable metadata are not nested stored references.
Deferred generic call records defer return-origin proof only while no resolved
function or lambda call exists. Concrete lambda instantiation must prove the
declared origin, including rejecting a different reference argument.

`field.get(owner)` and `get_mut` on a `meta_field` value are intercepted in
`visitCallExpr` (`analyzeMetaFieldAccess`): semantics synthesizes the
projection `owner.<field>` as an expression owned by the model, analyzes it in
the caller's context, and records it as the call's alias
(`recordExpressionAlias`). Classification, place walking (`semanticPlace`),
HIR lowering and the HIR source-alias table (`HirBody::sourceAliases`) follow
the alias, so every access, borrow and lowering rule is the direct access's.
The call keeps a `ResolvedCallInfo` with the `MetaFieldGet`/`MetaFieldGetMut`
kind for passes that read the call shape; it has no borrow origin because it
is the place itself. While the synthesized projection is analysed
(`analyzingReflectedFieldAccess`), the member-access privacy check admits
private fields of a `[[reflect_private]]` owner (`ClassInfo::reflectPrivate`);
the lowered class record carries the same flag so the C++ representation
emits every member public, since GTI access control is already enforced here.

Declared attributes are parsed by `bracketAttributedDeclaration` into
`DeclarationAttribute` (name path, argument list, optional designators) on
`VariableDecl` and `ClassDecl`; a qualified name after `[[` distinguishes
them from compiler attributes, which `registerClassAttributes` extends with
`attribute` and `reflect_private` (`ClassInfo::attributeType`,
`reflectPrivate`). They are evaluated once per declaration by
`analyzeDeclarationAttributes`, which runs at the start of the body phase,
before any body can query them: in the owner's declaration context it
resolves the attribute type, checks it is an eligible struct, analyses each
argument with `analyzeInitializer` against the field type, evaluates it with
the constexpr interpreter, converts it with `convertConstantToType`, fills
omitted fields from literal declaration defaults, and records the resulting
object constant (`ResolvedDeclarationAttribute`, keyed by the declaration in
`SemanticModel::declarationAttributes`, base-fallback aware). Attribute
queries are folded intrinsics like the field queries, described by one shape
table (`metaQueryShape`: leading type arguments, trailing `uint64_t` indices,
result type) that `analyzeMetaIntrinsicCall`, `metaIntrinsicConstant` and
`metaQueryConstant` share; enumerator, payload, base and argument queries use
the same table (`metaEnumeratorFacts` reads `enumTypeFor`, so generic enum
instances are substituted), and `meta_type_trait<T, N>` carries the boolean
traits (payload enum, generic instance, reflectable, signed integral).
`field_attributes`, `type_attributes`, `enumerator_list`, `payload_list`,
`payload_type`, `base_list` and `argument_list` are further computed
compiler types rewritten by `rewriteCompilerType`.

`attribute<A>()` is intercepted like `get` (`analyzeMetaAttributeAccess`):
the call records the attribute's object constant, which the interpreter uses
directly, and is aliased to a synthesized designated construction of `A` from
clones of the application's own arguments, so a run-time use of the whole
object is an ordinary struct construction. `visitGetExpr` folds a member read
of that result (`foldAttributeMemberRead`): the read is aliased to a literal
of the field's type (`constantLiteralExpression`: string, bool, character,
float, a typed integer conversion, or an enumerator path) and recorded as a
constant, so the constructed object is never materialised for it. The
`std::meta::reflectable` constraint is `GenericConstraintKind::Reflectable`
(`isReflectableType`), bound by `@compiler_constraint("meta_reflectable")`
in the meta unit.

A direct-initialized class temporary consumed by a projection (the alias
above, `return {.x = 1}` consumers and the like) is a fresh SSA value that
MIR lowering now materialises before rooting a place at it
(`emitPlaceDependencies` treats `HirValueKind::DirectInitializer` like a
call); previously only call-like producers were emitted there.

Reflected field access checks its owner argument against the described owner
(`isSameOrDerivedClass`, following the public base chain) before aliasing,
and keeps a receiver with effects: when the metaobject expression is not a
binding, a literal, a projection of one or a default construction of a
compiler type (`isEffectFreeMetaReceiver`), the alias is the grouped comma
`(receiver, owner.field)` so the receiver runs once, before the owner, and the
projection stays the place (`semanticPlace` and `classifyExpression` follow a
comma to its right operand; MIR lowering already did).

Structural construction (`std::meta::construct`) is library recursion over
the field list with the compiler-private `meta_construct<T>(values...)` at
the base. Since a GTI call can only extend a parameter pack at its front, the
library prepends each produced value and the compiler selects a second
generated constructor, `ClassInfo::reversedDesignatedConstructor`, whose
parameters are a leading `bool` tag and the fields in reverse order while its
initializers stay in declaration order (`registerDesignatedConstructors`
generates both; the tag keeps the two signatures distinct for a single field
or a palindromic type sequence).
`analyzeMetaConstructCall` resolves the call through
`analyzeConstructorCall` with `selectingDesignatedConstructor` set, so the
generated constructor is the only candidate and ordinary argument, ownership
and rollback rules apply; the call then carries a `ResolvedConstructionInfo`
and HIR lowers it as a construction (a resolved construction's callee is never
lowered as a runtime value). A type-name call whose computed type rewrites to
a scalar (`std::meta::field_type<T, I>(value)`) is aliased to the
`Conversion` that scalar's keyword spells, and is deferred while the computed
type is still open.

Callable and location queries reuse the query shape table:
`metaMethodDeclarations` orders a class's `FunctionDecl` members (active
conditional branches included), `metaMethodFacts`/`metaConstructorFacts`
substitute the instance's arguments into parameter and return types, and
`metaDeclarationLine`/`metaDeclarationFile` read the declaration tokens and
the source graph. The computed types `method_list`, `operator_list`,
`parameter_list`, `parameter_type`, `return_type`, `constructor_list`,
`constructor_parameter_list`, `constructor_parameter_type`, `list_first` and
`list_rest` are rewritten like the field computed types.
`MetaTypePrimaryName` folds the primary's qualified name without arguments.
`MetaFieldHasDefault` folds whether the field's `VariableDecl` carries an
initializer, and `field<T, I>().default_value()` (`analyzeMetaFieldDefault`,
intrinsic `MetaFieldDefault`) aliases the call to a clone of that initializer
analysed under the field's type, exactly like `attribute<A>()` aliases to a
synthesized construction; the open form is deferred to concrete reanalysis.

A reflection client dispatches on an open parameter with `if constexpr`, so
the symbolic primary defers the branch (`recordConstexprBranch` with no
selection) and only concrete reanalysis analyzes the statements inside it.
Such statements have no primary semantic record; HIR verification
(`verifyHirProgramPlans`) collects the statements under a `CompileTimeBranch`
whose source the primary deferred and verifies a range iteration there on the
instance's own concrete types, structure, loans and strategy instead of
comparing it with a primary plan.

## Request Sessions

Function-signature syntax has a distinct `TypeRef` shape and resolves to
`SemanticType::FunctionSignature`, with the result followed by ordered parameter
types plus `signatureReceiver` (`ReadOnly`, `Mutable`, or `Consuming`). The AST
retains the written `mut`/`&&` token and source span. Generic substitution,
canonical equality, overload-signature comparison, type printing, and alias
resolution use that structural type. Signature components do not introduce
stored borrows into an enclosing generic argument. `GTI-S2093` rejects non-value
components and attempts to use a signature as storage. Native callback types
retain their separate ABI contract. Signatures have no value lifecycle traits.
The lowered C++ representation emits the signature as a type, and MIR rejects
bare signatures in executable values, places, and declared fields.
Exact function-generic deduction traverses signature components using the
ordinary structural inference path, including `void` result metadata and
paired reference layers. It requires exact receiver-qualifier agreement;
capability satisfaction is not a conversion between signature metadata types.
Generic arguments and defaults no longer reject
`void` categorically; concrete object-position validation and constraints own
those restrictions. Class-instance reanalysis also validates alias targets in
the concrete owner scope so a dependent signature parameter or array element
cannot become `void` unnoticed. Exact deduction also matches a final symbolic
`TypePack` against the ordered tail of a signature or nominal argument list.
Diagnostic and speculative inference share this matcher. Empty tails are
concrete empty packs, while symbolic forwarding retains the source pack's
identity. Repeated and explicitly supplied deductions must agree. A generic
type pack need not introduce a named value parameter when a formal metadata
pattern binds it; concrete instances still retain one flat generic argument
list for reanalysis, constraints, and HIR identity. This pack matcher does not
rank overloads or select patterned class definitions; the separate exact
pattern/receiver-family selector described below owns that decision.

Headerless signature-first patterned-primary syntax is retained separately in
`ClassDecl::genericArgumentPatterns()`. It is not flattened into AST generic
parameter declarations and is not an exact specialization. Registration derives
binders from bare unqualified type leaves; repeated names share an identity.
Pattern resolution publishes canonical semantic shapes and a type-argument
projection for each binder. Class identity retains its public argument list:
`Callback<int(bool)>` has one signature argument, not two flat arguments.
Semantic admission and concrete reanalysis require exact agreement after
substitution, including fixed components, repeated names, and empty pack tails.
HIR and lowered representation consume the resolved projections; they do not
declare binders or select among patterns. The lowered seal includes both shapes
and paths, and verification rejects invalid paths and forged projections.
A single-signature name can group disjoint receiver-qualified definitions in
the semantic catalog. The first registered ID owns the family inventory; each
definition retains a distinct ID, generic binders, members, and member aliases.
Selection uses the exact signature qualifier before pattern admission and
repeats after substitution. Registration rejects duplicate qualifiers, mixed
declaration kinds, ordinary-primary overlap, and foreign-package extensions.
Use-site admission preserves source-unit visibility. Member aliases are indexed
by their owning declaration rather than a shared family spelling, including
during alias dependency resolution. Exact concrete specialization keys use the
selected definition's ID and public arguments. HIR, MIR, backend representation,
and language queries consume that selection without selecting a family again.
There is no pattern ranking. `L-CALL-02` records the delivered source-defined
owning wrapper clients and their supported boundaries.

Class type packs reuse the generic parameter identity and `TypePack`
substitution representation. `TypeRef::packExpansion` distinguishes a type-list
expansion from a named value pack. Semantic resolution flattens concrete packs
into canonical class/signature arguments, including an empty list; HIR and
detached representation substitution preserve the same ordered identity.
Reference arguments in nominal metadata do not themselves create borrowed
state: lifecycle traits still derive from actual fields and bases. Concrete
storage and reference-referent validation remain frontend responsibilities.
There is no public-library-name recognition or backend-selected expansion rule.

Named method packs can bind the enclosing class's final generic pack by its
resolved identity. Declaration signatures retain symbolic `TypePack` metadata;
class substitution preserves that identity through symbolic base forwarding.
Method selection groups the owner's prescribed concrete list by source argument
expression and reuses exact argument and ownership checks, including forwarding
and `operator()`. It does not infer owner arguments from the call. Concrete
lexical pack scopes retain the declaring name/ID independently of a substituted
pack's canonical type identity. Signature-pattern forwarding does not rewrite
that type identity to impersonate the receiving declaration, and a nearer
non-pack generic shadows the enclosing pack normally. Concrete field reanalysis
also runs for zero-element owner packs, preserving the same canonical selected
field types as their constructor schedules. Concrete class reanalysis validates
member pack object positions even for uncalled
methods. Empty owner packs still trigger concrete method reanalysis. HIR
instance signatures flatten concrete packs independently of discovery path,
while body bindings retain source pack identity for cleanup and forwarding.
Each mutable-reference pack element has its own semantic entry loan, identified
by the formal pack binding and constant element index. Whole-pack forwarding
preserves the exact reference type/access rather than borrowing the grouped
binding. These elements cannot be named independently or escape through a pack
expression; the entry capabilities last for the invocation. HIR and MIR retain
the projected identities and end the loans on normal and failure exits.
Concrete by-value pack elements that carry reference fields likewise retain
separate `StoredValue` entry loans. Their identities include the formal binding,
constant element index and each resolved reference-field path/access mode.
Expanded call arguments preserve that element path, even when several elements
have the same type. Generic reanalysis forwards these exact dependencies rather
than treating the grouped binding as a referent or dropping its other origins.

One call to either public `SemanticVisitor::check` overload owns one complete
semantic request session. `SemanticVisitor::Impl::RequestSessionState` owns one
private `SemanticCatalog`, one published `SemanticModel`, diagnostics,
substitutions, contextual state, program effects, and the body-scoped
`ConstexprInterpreter`, `LoanFlowState`, and `ValueFlowState`. Both program and
standalone-expression analysis enter through the same `resetRequestState`
operation before publishing any result.

Registration mutates `SemanticCatalog` in the ordered prepasses above. It owns
namespace and visibility indexes, aliases, concepts, classes, enums,
inheritance/member tables, exact specializations, native declarations, generic
parameter identities, and one stable `CallableDeclaration` per source
function. `FunctionInfo` publication is one explicit projection from that
record. Overload resolution copies only a bounded `CallableCandidate` view for
type substitution, receiver/access, dispatch, borrow-result, and viability
state. The catalog is sealed before body visitation; body namespace value state
is a `ValueFlowState` copy, constexpr binding values remain model/body facts,
and a seal-shape assertion guards body and concrete-instance analysis against
catalog insertion or identity drift.

The selected target, source-graph pointer, tooling-occurrence mode, and metrics
sink are visitor configuration and remain outside the session. Reusing a
visitor therefore preserves configuration but cannot preserve names, compiler
IDs, AST pointers, flow facts, diagnostics, or return/expression context from a
previous check. Concrete-instance analysis still operates against the current
snapshot; it does not establish an unrelated public request.

The compiled units do not partition or copy session state. Every method is a
member of the same `SemanticVisitor::Impl`, and every unit includes only the
private declaration/state contract. Deduced-return helpers and templates stay
with that contract because their definitions must remain visible; algorithms
with a concrete signature compile out of line under their owning
responsibility. A static boundary keeps the private header as the sole direct
consumer of `semantic_analyzer.h` inside this implementation and rejects a
second facade or analyzer-state definition.

Borrow-summary publication builds one `FunctionId`-to-candidate index during
registration and updates the catalog declaration plus only the candidate views
for that resolved function. It does not rescan every namespace/member overload
table once per function. The index is request-local and contains no durable or
process-global identity.

Pre-body stored-field summaries compose unambiguous exact-result factory calls
and same-type construction/grouping wrappers field by field. Each result field
retains its source parameter or receiver projection and its own shared or
exclusive access; a mixed result is not flattened to its first parameter.
Recursive, ambiguous, and unresolved generic-call paths remain conservative.
This declaration inference only proposes a contract: selected call analysis and
return-body validation independently check every actual owner, field, access
permission, and permitted escape. The direct-initialization rule does not grant
new borrowing or escape authority.

Mutable field groups are source-only declaration containers. Member collection
recurses through the active group items with the surrounding access state,
registering every child as an ordinary mutable direct field in textual order.
The semantic model records no group symbol. Its retained origin is used only to
attach related information when an existing field/access restriction is
triggered by inherited group mutability. Mutable stored-reference fields are
supported under their ordinary exact binding and exclusive-loan rules.

Static assertions are analyzed as compile-time obligations rather than body
operations. The semantic model records `Passed`, `Failed`, or `Deferred` by AST
identity. Non-dependent exact-`bool` conditions use the existing bounded
constexpr evaluator immediately. A condition that refers to an enclosing
symbolic generic type or value parameter is deferred, then reanalyzed under the
canonical concrete class/function/method/constructor substitution. Concrete
instance tracking deduplicates evaluation and preserves the first requiring
source span for related diagnostics; independent assertions in generic source
still run during symbolic analysis.

Declaration registration order is separate from runtime evaluation order.
[Execution Section 4.2](../language/execution.md#42-evaluation-order) now fixes
strict left-to-right expressions, full-expression boundaries, and a lexical
dependency-first program-initialization walk. Semantics now selects the AST
roots of each full expression, including separate loop-condition/increment and
constructor-initializer groups. HIR maps those roots to concrete identities; it
does not rediscover endpoints from lowered statement kinds.

Semantics additionally owns one immutable `ProgramInitializationPlan`. It
visits configured prelude roots in their preserved order and then the entry
unit, follows explicit include edges in lexical dependency-first order with
first-visit deduplication, and excludes inactive compile-time branches. Namespace
globals and non-generic static fields then enter the plan in source order. Each
storage step has an explicit `DataOnly` or `Initializer` role: implicit zero
initialization and exactly materializable `constexpr` data are data-only, while
an executable source initializer owns an initializer step. Ordinary generic
class static fields remain rejected; they do not have a separate implemented
per-instance storage plan. The admitted collection-protocol static hooks are
functions, not generic program storage.

A use of later program storage is permitted only when the frontend records an
exact representable constant substitution for that expression. Reads, writes,
address or reference formation, borrows, and reference-boundary arguments are
storage accesses even when the declaration is `constexpr`. The analyzer closes
each executable initializer's effects over exact calls, constructors,
destructors, field/base initialization, and cleanup. An ordinary bodyless GTI
declaration, open generic cleanup shape, or other missing summary is unknown;
`GTI-S2068` rejects a later-storage access or unknown transitive effect before
HIR. This proof does not infer safety from a backend or from `findConstant()`.

Closed generic calls use request-local concrete effect queries keyed by the
declaration, concrete owner, and type/value substitutions. A reused isolated
semantic instance worker analyzes only initializer-reachable selected bodies;
discarded `if constexpr` arms do not contribute effects. Nongeneric bridge
calls retain their graph edges, and source constructor/destructor/field effects
use the same exact instance identity. Query expansion is bounded by 64 generic
ancestry steps and 16,384 analyzed distinct instances, with located `GTI-S2105`
diagnostics. Open substitutions, dynamic dispatch, and protocol/combinator paths
without exact selected generic arguments remain conservatively unknown.

Value-generic arithmetic needed before complete program HIR/MIR is retained as
an immutable declaration-ID-bound expression tree, never a borrowed AST pointer.
Substitution reuses the checked constant-operation kernel and exact-specialization
selection. Complete MIR cannot yet own this early type-formation query because
array/value arguments and class field layout shape the semantic instance itself;
the remaining L-CONST-01 staged/query-driven MIR evaluator remains the migration
goal, not a separate permanent runtime interpreter.

Missing nominal-field defaults use semantic-owned empty construction anchors,
retained across snapshots and deltas. Ordinary contextual construction analysis
publishes the exact selected operation and effects; HIR lowers that typed field
initializer, and MIR owns final-address construction, stage activation, ordering,
and rollback. Other omitted field categories do not gain an implicit initializer.
An open nominal field defers construction until concrete analysis; recursively
unresolved type arguments, value expressions, and extents are not an invitation
to check a constructor requirement outside its selected constructor's context.
Concrete selection still enforces every zero-argument operation and requirement.
Default-availability queries apply those requirements under the exact owner
substitution; a constrained-out zero-argument constructor is not queried as an
omitted default when a different constructor explicitly initializes the field.
Abstract owners remain constructible only in base-subobject position, never as
implicitly defaulted value fields.
New omitted-nominal defaults do not manufacture an extra argument-free overload
beside an owner's declared argument-taking construction API. Constructor-free
owners can use the structural default; declared zero-argument operations keep
their ordinary selection rules. The earlier generated overload for completely
written declaration-default state remains available unchanged.

`SemanticAnalysisSeal` binds the stable `Program` snapshot, full `TargetInfo`,
active-statement preorder, and exact ordered source-graph/prelude provenance.
HIR copies that seal and must match the same program and target before
lowering. Admitted executable families now use their ordered MIR stages and
frontend-selected loan endpoints; M-EXEC-01 is not a pending whole-program
cutover. A new composition must extend the applicable schedule and verifier
before acceptance. Existing conservative restrictions identify unsupported
semantic/representation profiles, not permission to fall back to an earlier
executable authority.

Ordinary call/construction exclusivity follows the implemented ordered-input
schedule: a completed mutation in an earlier operand does not conflict with a
borrow first produced by a later operand. The opposite order still conflicts,
including nested helper-produced and stored-carrier loans. Simultaneous
mutable parameter accesses and pre-existing lexical loans remain independently
checked. The primary diagnostic identifies the later invalidating operand and
relates the earlier borrow; there is no automatic operand-reordering fix.

## SemanticModel

`SemanticModel` is a set of snapshot-owned side tables keyed by AST identity or
compiler IDs. Important facts include:

- expression type, value category, access, traits, and constants;
- binding types, mutability, ownership/drop/copy/move and transfer/share
  traits, and source symbol;
- functions, classes, enums, aliases, concepts, constructors, and lifecycle;
- each exact class specialization's distinct class ID, primary class ID, and
  canonical type/value key, plus primary-application lookup by that key;
- exact selected calls, operators, conversions, constructors, contextual
  integer operands, intrinsic identity, dispatch mode, borrow origin, and any
  exact global/static borrow place;
- exact local defined-failure origins and call-like propagation channels,
  keyed by source expression and anchored by snapshot-local source-unit
  identity plus line/offsets;
- AST-selected full-expression roots for statements and constructor
  initializers;
- class bases, override roots, abstract/polymorphic state, and destruction;
- type-owned static function identity, separate from interface instance
  contracts: static methods have no receiver or virtual roots, and use ordinary
  access, default, generic, constraint and definition-closure rules. Interfaces
  remain abstract with polymorphic destruction even without instance signatures;
- array extents, switch constants, lambdas, target selections, moves, loans,
  unsafe operations, selected execution profile, and completion context;
- concrete implicit-return movement for an exact whole move-only local or
  by-value parameter, including its borrow transport access; the fact is
  instance-local because copyability may change after generic substitution;
- static-assertion pass/fail/deferred state and concrete generic evaluation;
- the semantic analysis seal, exact program-initialization plan, exact
  program-constant substitutions, and the selected hosted-entry plan.

`FunctionInfo` and `ConstructorInfo` retain the required parameter count and a
separate program-effect owner for each declared default expression. Selected
`ResolvedCallInfo`, `ResolvedConstructionInfo`, and constructor-initializer
records carry the exact omitted default-expression suffix. The analyzer checks
that suffix in declaration scope, validates its type and supported ownership
shape, rejects recursive expansion, and closes each used default's effects into
the caller. Consumers do not recover defaults by recounting AST arguments.

The `TargetInfo` supplied before analysis also carries the one GTI-owned scalar
`TargetDataLayout`. `Frontend` rejects an unsupported value before constructing
`SemanticVisitor`, so compile-time branch selection, HIR lowering, optimization,
and backend generation all observe the same normalized facts. The semantic
model does not query native C++ or LLVM for the source `sizeof`/`alignof`
contract. Those constant queries admit target scalars, integral enums, passive
unions, valid `[[c_abi]]` records and supported fixed arrays, not ordinary
class representation layout. Native record source-order offsets/size/alignment
and passive-union size/alignment are retained in `ClassTypeInfo`.
Separately, `runtimeObjectLayoutType` validates complete concrete object types
for the declaration-bound object-size/alignment operations behind
`std::layout_of<T>()`. That implemented runtime representation query admits
ordinary classes and other supported carriers; it is not a compile-time or
stable foreign-layout promise. A `[[c_opaque]] struct Name;` instead records one
incomplete nominal `ClassId` with no layout or lifecycle. Semantic type
validation confines that identity behind raw-pointer indirection at any
supported recursive pointer depth. Extern-C and native-record validation admit
those pointers while continuing to reject the handle by value or an operation
requiring its hidden representation.

Floating expression facts distinguish `SemanticType::Float` (binary32) from
`SemanticType::Double` (binary64). Semantics owns literal width, common numeric
promotion, implicit `float`-to-`double` widening, explicit-only narrowing, and
the selected width passed to constant evaluation. LLVM floating types never
enter `SemanticModel`.

Built-in numeric place updates additionally retain their selected arithmetic
domain as `ExpressionInfo::updateArithmeticType`. The destination and expression
result keep their own type. Semantics authors independent arithmetic and
checked-back-conversion failure origins at the written operator; HIR preserves
them and MIR emits separate primitive stages. Native C++ promotions are not an
authority for narrow increment/decrement or compound assignment.

For a binary operator's bounded integer-literal context, semantics recognizes
the complete literal expression, including one unary sign, checks its
mathematical value against the selected concrete operand type, and records a
signed operand fact when the sign expression itself adopts that type. HIR,
MIR lowering, and target representation consume the recorded expression
type/fact; they do not re-decide literal eligibility from native promotions.
Symbolic operands constrained by `std::numeric` (including composed concepts)
also supply that context, so equality does not reject literals that arithmetic
already accepts. Concrete integer instances range-check and retype the literal;
floating instances retain the ordinary integer literal and existing floating
operand conversion. No new MIR operation, implicit narrowing, or call-ranking
rule is introduced.

`SemanticVisitor::visitLayoutQueryExpr` is the sole authority for source
`sizeof(type)` and `alignof(type)`. It resolves aliases, recursively derives
positive concrete fixed-array size/alignment from `TargetDataLayout`, and
records an exact unsigned-64 constant on the source expression. It reports
`GTI-S2063` for unsupported representation categories, symbolic or zero
extents, and checked size overflow. Unknown types keep the ordinary
type-resolution diagnostic without a second layout error. No host or LLVM
layout query participates.

Native-record validation accepts only non-generic passive structs or unions with public
instance fields drawn from fixed-width and target-C scalars, `bool`, nested
valid native records, named native callbacks, recursive admitted raw pointers,
and positive concrete fixed arrays of admitted field elements. It rejects
methods, lifecycle declarations, bases, access sections, statics, empty and
recursive-by-value records, and
cleanup-owning or symbolic fields with `GTI-S2064`. The resulting immutable
layout is also the authority used by `extern "C"` signature validation and by
recursive unsafe-call classification when a record contains a raw pointer.

An `extern "C"` function retains two identities when it uses
`[[link_name("symbol")]]`: namespace lookup and tooling use the declared GTI
name, while `CallableDeclaration::externalSymbol` stores the validated portable
C identifier. Program-global duplicate-symbol, reserved-`main`, generated
header, and root-storage collision checks use the external symbol. HIR and MIR
copy that resolved fact; they do not reparse the attribute or derive the symbol
from the source name.

Native-union validation is a separate semantic family. It accepts a nonempty,
nongeneric, baseless public field-only declaration and computes maximum-field
size/alignment from target facts, nested valid unions, and admitted passive
records. `GTI-S2066` rejects behavior, hidden lifecycle, initializers,
references, ownership-bearing fields, and recursive by-value edges. Every
resolved member read/write is independently classified as an unsafe union
operation; emitted C++ layout is not semantic authority.

Payload-enum resolution assigns stable declaration-order variant identities,
resolves exact field types, folds copy/move/ownership/drop traits across every
alternative, and records construction and switch-pattern selections. A
concrete class field may own active cleanup, while stored borrowed state and
active recursive/non-class carriers remain rejected. Pattern bindings are
fresh immutable arm-local values. A switch over a named place observes it, so
a by-value binding is a copy and requires a copyable field; tag-only
alternatives admit move-only payloads without extraction. A switch whose
subject has `ValueCategory::Value` (a prvalue or an explicit `std::move`)
consumes it: semantics records the switch's hidden subject owner as a
`BindingInfo` (`findConsumedSwitchSubject`), marks its patterns `consuming`,
and requires each by-value field to be movable or copyable rather than
copyable. `auto&` and `mut auto&` patterns on a consumed subject are rejected
with `GTI-S2067`. The explicit move already recorded the moved place in the
ordinary flow state before any arm is analyzed. Semantics
records a switch as exhaustive only when it has `default` or every variant is
covered once with valid patterns. `GTI-S2067` owns payload shape, exact
construction, unsupported cleanup carriers, copied-pattern capability,
duplicate, and missing-variant failures. The bounded owning callable family
admits a direct enum type at ordinary source-defined free-function,
non-polymorphic method and ordinary-constructor boundaries and at exact typed
lambdas. Ordinary call analysis selects copy/move from the enum's folded
traits, so unavailable copies retain the normal ownership diagnostic before
HIR.

The same declaration-time analysis admits a direct owning-enum field in an
ordinary concrete class or struct, including a concrete generic owner, with no
polymorphism, native layout, stored-reference policy, or compiler-private
capability. Its
structurally folded class lifecycle then owns containment transport rather than
a payload-enum-specific call exception. Ordinary generic functions, methods and
constructors use that same direct transport contract. The shared check retains
the excluded polymorphic, native/runtime and operator boundaries, direct
captures, fixed arrays and direct result-carrier alternatives. A nominal type
argument alone is not a contained-ownership edge; its actual fields and
lifecycle decide containment, while recursively unsupported shapes remain
rejected. A top-level
checked-reference parameter does not contain or transfer the referent and
remains valid for exhaustive tag inspection.

AST pointers in these records remain valid only while the owning
`FrontendResult::program` lives.

## SemanticDatabase And Tooling

The embedded `SemanticDatabase` exposes snapshot-scoped `SymbolId`,
`SymbolRecord`, and `SemanticOccurrence`. Symbol records currently retain kind,
qualified name, source unit, exact name/declaration/definition spans, type,
traits, access, mutability, static/internal/default-library flags, and generated
state. Occurrences link a source span and role to the resolved symbol and may
retain selected call/construction facts. Declaration documentation comments are
normalized by the parser and retained on the corresponding `SymbolRecord`.

Occurrence recording is editor-tooling work and is gated by
`FrontendOptions::toolingOccurrences` (default enabled). The driver's
compilation and `check` paths disable it: only position queries read the
occurrence table, and not building it removes about a third of the
compiler's peak memory on large inputs. Symbol records are always produced,
because HIR and the C++ emitter resolve member identity through them.

`include/gti/language_queries.h` builds compiler-owned hover, completion,
definition, reference, rename, outline, and signature-help results over the
immutable frontend snapshot. Hover consumes the retained declaration
documentation.

`include/gti/documentation.h` owns a separate batch projection for reference
generation. `DocumentationQueries::standardLibraryIndex` accepts only a
source-, syntax-, and semantics-valid snapshot with tooling occurrences,
filters to public non-compiler-private standard-library symbols, reuses
`SignaturePrinter`, and returns presentation records made only of strings,
booleans, traits, and stable standard-library unit/offset data. The
deterministic JSON serializer omits physical checkout paths, AST addresses,
timestamps, and `SymbolId`. `SymbolId` remains snapshot-scoped and must not
be placed directly in a durable index or URL.

## Concrete Generic Reanalysis

Generic bodies are checked symbolically, then HIR requests concrete semantic
analysis for discovered function, constructor, destructor, and class-field
instances. Concrete substitutions revalidate ownership, value parameters,
packs, selected construction, capabilities, callables, and borrowed-return
origins. HIR is not a second type checker; it asks semantics for the concrete
facts it needs.

`HirLowerer` receives the immutable base `SemanticModel` and a narrow
`SemanticInstanceProvider`; it cannot start a fresh semantic request or mutate
the visitor facade. The provider adapts the existing request-local
delta/rebase bracket and reads the sealed catalog while producing an instance
model and diagnostics.

Default expressions do not supply missing generic inference evidence. Once
written arguments select a concrete instance, instance analysis rechecks the
selected declaration's defaults under that exact substitution. HIR keeps the
resulting instance model alive while lowering caller-side default values; it
does not type-check a symbolic expression itself.

For an ordinary named call or construction, semantic analysis may defer a
brace argument and every following argument until candidate parameter shapes
are known. It infers an eligible `uint64_t` value parameter from the exact
array extent or brace count, selects one exact candidate without list-overload
preference, then analyzes the deferred arguments once in source order under
the selected parameter types. `ResolvedCallInfo` and
`ResolvedConstructionInfo` retain the separated type and value arguments plus
the concrete fixed-array parameter types. Braces never acquire an independent
semantic type.

An explicit ordinary function or method generic suffix is classified before
candidate selection using its syntax, the target parameter kinds, and the
lexical constant catalog. Closed `uint64_t` expression trees contain literals,
in-scope `constexpr uint64_t` bindings, enclosing value parameters,
parentheses, and checked arithmetic; other written arguments occupy type
slots. Type slots precede value slots. Exact overload viability consumes those
ordered vectors, performs structural inference from written call arguments,
then applies declaration-owned defaults to unresolved trailing positions in
order. An ordered registration-side constant catalog folds earlier visible
namespace `constexpr uint64_t` bindings whose initializers are themselves
closed arithmetic before generic declarations, without making later or
invisible storage available. Default type records retain
resolved symbolic `SemanticType`s, and default value records retain normalized
parameter-identity expression trees; neither is source text. Concrete instance
reanalysis substitutes the complete result just like inferred fixed-array
extents. This is frontend authority:
native template deduction/defaults are not consulted and failed candidates
cannot leave partial substitutions in shared semantic state.

An ordinary constructor can add inference-only local type parameters inside
its owning class's generic scope. Candidate analysis infers them solely from
written arguments, checks inline and trailing constraints after substitution,
then records the declaration, concrete owner, ordered local type/value
arguments, concrete parameters, requirements, and instantiation site in
`ResolvedConstructionInfo`. Concrete constructor analysis revalidates those
requirements before HIR. The instance key includes all local generic
arguments, so equal native layouts do not merge semantic instances.

Each reanalysis runs on the one shared analyzer inside a detach/restore
bracket (`InstanceAnalysisScope`): the accumulated model is detached, the
instance is analyzed into an empty delta `SemanticModel` whose lookups fall
back to the detached base, and the bracket then restores the base model,
diagnostics, and identity counters. Instance analyses are strictly
sequential. The delta records only what the instance produces — reads reach
base facts through the fallback, loan tables deliberately restart rather
than fall back, and record mutators copy a base record into the delta before
updating it. This keeps per-instance cost proportional to the instance body
instead of the whole program, with observable identities and emitted output
unchanged for inherited declarations. New declarations found only in a selected
dependent branch receive IDs above a request-wide symbol identity ceiling. The
ceiling advances in sequential analysis order and resets with the request; it
is not restored with body-local counters. Thus distinct concrete-only locals
cannot collide even when instances have identical source names or different
types. These delta symbol records remain concrete and are handed to HIR without
mutating the immutable primary snapshot. Discarded arms are still unanalyzed.

### Trusted Prefix Preamble

Most of an editor snapshot's semantic work is the standard library:
registration of every loaded unit and, under `Reachable` analysis, the
library bodies the open file reaches. Neither depends on the open file, only
on which trusted units precede it. A frontend caller may pass a
`SemanticPreambleCache` (`include/gti/semantic_preamble.h`) through
`FrontendOptions::preambleCache`; the frontend then analyses a program's
*trusted prefix* once and resumes every later analysis of a program with the
same prefix from a copy of that state. `gti check` and builds do not set it,
and an analysis with the raw-pointer audit enabled never uses it.

- **Prefix.** The leading units of the compilation order that are
  compiler-trusted and not the entry unit; their declarations are a prefix of
  the program's declaration list. A `SemanticPreambleKey` names the analysis
  configuration, every prefix unit by identity, path, digests and exact
  parsed declarations, and the whole source graph by unit identity and
  include edges (registration publishes each declaration's visibility per
  consuming unit, so a program with other units cannot share the tables), so
  a changed or re-parsed library unit, or another include graph, selects
  another preamble (the cache keeps a few, least recently used first).
- **Building.** `SemanticVisitor::buildPreamble` analyses the prefix
  declarations as a program of their own with complete trusted bodies, then
  freezes the analyzer: its session state (catalog, model, flow state,
  analysed-body sets, identity counters) becomes the preamble, which also owns
  the prefix program so the syntax that state refers to stays alive. Program
  initialization and the hosted entry are not planned for a prefix. While a
  preamble is built, every body that bounded analysis would have deferred is
  analysed in place but *held back*: its body diagnostics and the function
  records it consulted are kept beside the body instead of being reported.
- **Resuming.** `SemanticVisitor::adoptPreamble` makes the next `check` copy
  the preamble's session state (the compiler-generated copy of
  `RequestSessionState`, with the catalog deep-copied and request-local
  budgets replaced), reopen the catalog, and run the registration passes over
  the declarations after the prefix only: the whole-catalog passes start at
  the adopted class, enum, alias, concept and function floors, and native
  record and union shape states persist in the session so later records can
  refer to earlier ones. The body phase then analyses only the remaining
  declarations, with a call journal, and finalization runs under the same
  `LateFinalizationScope` late demand uses, with the forwarding, call-use,
  candidate and occurrence counts recorded when the preamble was frozen, so
  every whole-program pass confines itself to what this analysis added;
  program initialization is planned for the whole program.
- **Reaching held-back bodies.** A held-back body is complete in the adopted
  model, but it is reported, and shown to HIR as analysed, only once this
  analysis reaches it: the functions the entry bodies demanded, the bodies a
  required-body request names, every body under complete analysis, and
  transitively the bodies those consulted. HIR's primary-body requests and
  the functions a concrete instance analysis consulted reach bodies the same
  way; an error among a reached body's diagnostics fails that request exactly
  as a fresh late analysis would have, so the repeated analysis reports it in
  the pass. Diagnostics for the open file are therefore those of bounded
  analysis, and a library body nothing reaches stays the library's own
  `gti check` concern.
- **Identity.** Registration of the prefix is literally the preamble's, so
  trusted declaration identities, the instance-reuse registration fingerprint
  and symbol identities below the prefix ceiling are the same in every
  resumed analysis; the rest of the program registers above them. The syntax
  the preamble synthesized (generated constructors, `constexpr for`
  expansions, expression aliases, implicit field-initializer anchors) is the
  same object in every resumed analysis, so the instance-reuse journal treats
  it as stable rather than snapshot-owned; the instance cache's generation
  context names the preamble's process-unique identity, so such entries are
  never served under another preamble of the same units.

`FrontendWorkMetrics` reports preamble builds and adoptions with their time,
the held-back bodies and how many were reached. The `trusted_body_analysis`
parity gate analyses every example and standard-library test fresh, resumed
from a preamble twice (built, then adopted) and completely over a preamble,
and requires the application-located diagnostics of the complete check, a
subset of its library diagnostics under bounded analysis and exactly its
diagnostics under complete analysis.

### Instance Reuse Across Snapshots

An editor snapshot re-derives the same library instances on every keystroke:
`std::vector<std::string>` members or formatting internals have unchanged
inputs whenever the standard library is unchanged. A frontend caller may pass
an `InstanceAnalysisCache` (`include/gti/instance_analysis_cache.h`) through
`FrontendOptions::instanceCache`; bounded (`Reachable`) analysis then reuses
its own earlier instance deltas instead of repeating them. Complete analysis
(`gti check`) and artifact builds do not set it, so their behaviour is
unchanged. The semantic analyzer gains no second analysis path: a reused
delta is exactly what `InstanceAnalysisScope` produced in an earlier
snapshot, and every request still flows through the same provider calls.

- **Eligibility.** The declaration (function, constructor, destructor or class
  field initializers) is compiler-trusted, and every class/function type and
  value argument names only compiler-trusted classes and enums (no lambda,
  function item, open parameter or symbolic value). Designated constructors are
  ineligible because their syntax is generated per snapshot.
- **Generation.** Entries belong to a generation keyed by the frontend cache
  context and analyzer configuration, every compiler-trusted unit by identity,
  path and digests together with the exact parsed declaration trees the parse
  cache shared (the generation retains them, so an AST address can never be
  reused by another allocation while an entry lives), and a fingerprint of the
  trusted declaration identities registration assigned: symbols up to the
  registration ceiling, functions, classes and their constructors and
  destructors, enums, concepts, type aliases, generic parameters,
  default-argument effect owners, and every exact class specialization
  wherever it is declared. Registration numbers declarations in program order,
  so an application unit ordered before a library unit can renumber library
  declarations; the fingerprint then selects another generation. Within a
  generation, keys use snapshot ids directly.
- **Journal.** While a reusable candidate is analysed, its delta journals what
  it read from the base: base symbol identities (by id or by declaration
  span, from bindings, places and resolved symbols), base lambda identities,
  and any base-owned synthesized syntax. Body-local identities are assigned in
  demand order, so an edit that adds a local before a library call renumbers
  them; on reuse each journaled identity must still denote the same
  declaration, otherwise the entry is rejected and the instance analysed
  afresh. A delta that read snapshot-owned synthesized syntax (a designated
  constructor, a constexpr-for expansion or expression alias of the base) is
  never stored. Implicit field-initializer anchors of trusted fields are
  instead shared through the cache: an anchor holds only its field's name
  token, so every snapshot presenting that field receives the same anchor and
  HIR's identity check against the base anchor still holds.
- **Identities.** A delta's concrete-only symbols keep the range they were
  first allocated. Reuse requires that range to lie above the current base and
  apart from every other delta of the request, then reserves it, so later
  instance analyses and late demand continue above it exactly as
  `instanceSymbolCeiling` already guarantees. A delta that allocated lambda,
  function, constructor or generic-parameter identities is not stored.
- **Consulted facts.** The functions whose facts the analysis read (the late
  demand probe) are stored with the entry and must denote the same
  declarations; any still pending in the current snapshot are late-demanded
  before reuse, exactly as the fresh analysis would have done before analysing
  again. A stored delta never consulted a non-trusted function.
- **Results.** Only diagnostic-free analyses are stored, so a reuse reports
  nothing a fresh analysis would have reported. Entries are published to the
  cache only once HIR is final; an analysis that is repeated with required
  bodies, or whose late demand failed, stores nothing. A reused delta is
  rebased onto the current base, adopting its request-wide facts (place
  snapshot, execution profile, program kind and embedding plan).

The cache is bounded by entries, estimated bytes and generations (LRU), and a
generation whose units have the same paths but other syntax supersedes the
earlier one. `FrontendWorkMetrics` reports reuse hits, misses (with rejected
entries), ineligible requests, stores and the time spent validating and
copying entries. The `instance_reuse` test analyses every example fresh and
then twice through one cache and requires identical diagnostics, HIR inventory
and MIR modulo a single program-wide alpha-renaming of body-local symbols,
keyed by exact HIR binding identity and source declaration identity/role.
A shared source declaration retains its complete sorted set of concrete HIR
bindings; distinct symbols may not claim the same binding. Concrete-only locals
still require their original one-symbol/one-binding bijection.
This includes ordinary locals and parameters in late-demanded bodies: reserving
a cached concrete range can shift their subsequent allocation too. The earlier
concrete-only comparison incorrectly treated such harmless shifts as changes.
Non-local base symbols, fields, all non-symbol
identities and every instruction/loan/cleanup fact remain exact; swapped,
aliased or cross-body references are not equivalent. Its fixtures cover edited
bodies, changed declarations, an
edited library unit and complete analysis.

A complete-range requirement is checked symbolically before its concrete
iterator and sentinel types exist. The primary analysis therefore uses private
range-derived carrier types for generated `begin`/`begin_mut`/`end`,
dereference, comparison, and increment operations, and records `size()` as the
exact `std::size_t` fact supplied by `sized_range`. These carrier types never
become public associated types. Concrete instance analysis selects ordinary
source members or the fixed-array strategy, and the instance delta suppresses
any inherited symbolic operator row that the concrete body replaces with a
built-in operation. HIR accepts a generic-primary range plan only as a shape
proof; every emitted concrete plan still contains exact protocol identities,
loan shape, and checked fixed-array operations.

A generic primary may contain an inferred local whose symbolic initializer is
valid under the active requirements but has no one declaration-wide concrete
type. The canonical example is `auto value = *first` under
`std::input_iterator<Iterator>` when no `std::dereferences_to` requirement
names the element type. The shared `SymbolRecord` remains `Unknown` and carries
`instanceDependentType`; each concrete HIR instance owns the exact inferred
binding type. `LoweredProgram` preserves that declaration marker while its
concrete bodies remain fully typed. Instance reanalysis never writes a
specialization's type into shared semantic state, so specialization order,
failed candidates, and the presence or absence of any instantiation cannot
change the generic declaration. Tooling presents the source declaration as
instance-dependent `auto` rather than selecting an arbitrary concrete type.

A lambda type separates physical closure shape from concrete identity. Its
ordinary type arguments hold the result, parameters, and captures; dedicated
identity fields retain the lexical declaration plus the enclosing
class/function type arguments and class value arguments. Concrete reanalysis
reuses the lexical declaration and substitutes both parts. This distinction is
required even for captureless lambdas with identical signatures: two enclosing
generic instances may give the same source body different compile-time meaning.
Function and constructor instance identity includes bounded inferred and
explicit `uint64_t` arguments. Lambda identity retains enclosing function value
arguments as a separate identity component rather than treating them as
capture or signature types.

Each lambda capture records its new binding symbol, original source symbol,
exact type and traits, initialization mode, and initializer expression. Bare
captures are immutable copy snapshots. The only owned initializer is
`[target = std::move(source)]`, where `source` resolves in the enclosing scope
to an available local or by-value parameter. Semantics invokes the ordinary
move intrinsic and place-state authority before introducing the new immutable
target binding, so later captures observe the source as moved. Explicit `&name`
and `mut &name` captures instead retain shared/exclusive checked dependencies.
The common stored-field path identity includes lambda captures as well as record
fields. Each dependency preserves its origin place, access and parent loan;
copy/move captures of borrowed carriers retain their nested dependency paths.
Multiple independent parents are represented explicitly, rather than collapsing
all captures onto one parent. Global/field capture spelling and general init
captures remain rejected; a named local reference may select a field first.

## Confined Callable Contracts

Checked reference erasure is a declaration-bound capability over the existing
borrow model. Erase/restore calls publish exact argument-origin and access facts;
restoration additionally records an unsafe exact-referent-type obligation.
Resolved restoration records the referent's exact checked field paths, types
and access modes. The checked opaque input is the conservative witness for
these nested dependencies. A body-derived `restoresReferenceResult` summary
propagates that proof through ordinary helpers only when every return restores
the same declared borrow argument, including reference-local aliases. Concrete
instances recompute their field paths. HIR preserves the summary and MIR
independently verifies the restore operation, witness and helper-return chain;
a reference-return signature alone never establishes it.
The erased referent is opaque and nonconstructible. A typed GTI invocation
pointer admits only exact safe stateless targets and records their function or
lambda identities before HIR. Its erased signature retains GTI failure and
ownership conventions but cannot encode a borrowed result origin yet. Neither
capability obtains semantics from a public `std::*` spelling.

Source-defined callable views compose these capabilities with ordinary reference
fields and captureless invocation thunks. A constructor's single-reference
helper initializer can retain the helper's checked argument-origin projection;
the actual selected helper call and result remain verified executable operations,
not a backend reinterpretation of the formal argument. An unrelated wrapper
uses the same rule. Current native reference-member preparation supports an
ordered prefix of exact no-failure reference helper initializers; extending that
preparation to arbitrary fallible helper calls is a separate lifecycle goal.

An exact checked-reference relay with no callable-use contract can forward a
borrow of callable state without acquiring ownership or invocation permission.
Other callable boundaries still require their existing confined/owned contract.
Generic local reference bindings may be invoked just like generic parameters
and fields; concrete reanalysis establishes the exact target. Captureless lambda
parameters may expand a surrounding lexical type pack, with the same concrete
binding and ownership checks as an ordinary function pack.

A direct by-value or checked-reference generic parameter may acquire a `Confined` callable
contract when its visible body invokes it or forwards it to another proven
confined parameter. Each call site records an exact parameter list, result
type, required receiver access, and selected concrete lambda or `operator()`
target. A call used as a `void` operation has a `void` result requirement. A
condition supplies exact `bool`; an explicitly typed initializer, assignment,
or enclosing return supplies an exact non-reference value type. A direct
generic-parameter or generic-field invocation used as a call argument can also
take its result requirement from the uniquely selected outer parameter. The
contextual-argument pass inspects declaration types without evaluating the
receiver, defers that argument and its suffix, then analyzes them once in
source order after selection. Named functions/methods, constructors, lexical
lambdas, function items, and concrete call operators share this rule. A
callback's eventual concrete result does not choose between outer overloads;
an ambiguous outer signature remains ambiguous. Unknown arguments do not
deduce an outer generic parameter. If the outer receiver is itself an open
generic callable, a directly nested callable argument instead retains an
explicit `inferResultFromInstantiation` requirement in the semantic contract.
Concrete reanalysis selects the inner result and checks the outer invocation;
it must resolve a non-void, non-reference, non-borrowing, non-lambda value before
HIR copies the exact signatures. This is not inference from the enclosing
return type or backend representation. Tooling labels unresolved declaration
requirements as deferred rather than presenting an unknown type as exact.
Raw pointers remain non-owning values under their ordinary language rules.
`std::string_view` may denote static literal storage or retain an owner
dependency; accepting the type does not erase an expression's tracked borrow
or permit its escape. The callable boundary does not reclassify either as
ownership. In an `auto` conditional initializer, direct callable arms may
also retain `inferResultFromInstantiation`. An unknown arm is never represented
as `void`. The symbolic binding stays instance-dependent until concrete
reanalysis validates both arm types and publishes one exact binding/result
type. Plain open-callable `auto` initializers, reference results, tracked
borrowed-state results and lambda-identity results remain rejected.

Reference callable parameters reuse ordinary reference binding and loan
analysis. The contract's symbolic callable type names the referent, while the
concrete formal in HIR/MIR retains its reference type and access mode. A
checked dereference preserves the parameter's invocation identity; field,
index and raw-pointer projections do not qualify as the whole callable.
MIR validates confinement, reference access and repeatable capability as well
as the actual receiver/argument loans. Borrowed invocation and forwarding do
not introduce a new environment, allocation, ownership transfer or drop.
The caller's object remains live across the call, including defined-failure
cleanup, and existing escape/transport checks still apply.

Confinement applies equally to lexical lambdas and nominal callable objects.
Once a generic parameter is invoked, it cannot appear within that function's
return type unless every reachable valued return is the exact final
`std::move` of that same parameter, and it cannot pass through a target
parameter without another proven `Confined` contract. The exact-return case
retains the confined invocation boundary while recording an independent owned
transport fact; it permits repeatable read or mutable invocation, but never a
once-callable requirement or forwarding edge. A callable object that is never
invoked through the generic parameter retains ordinary value-transport
semantics. Invalid escape and forwarding edges are rejected before HIR, so
later phases never receive a false confined fact.

After forwarding contracts reach their declaration-order-independent fixed
point, semantics first resolves every provisional lexical-lambda argument
boundary against the selected target parameter. If the target did not acquire
a `Confined` contract, the call is rejected before HIR rather than retaining a
false boundary fact. A forwarding edge whose target directly or transitively
requires `Once` must pass the source parameter as `std::move(source)`. The
ordinary path-sensitive value-state analysis then owns cardinality: sequential
or possibly repeated forwarding is rejected, while mutually exclusive
returning branches may each contain one explicit move. Semantics then audits
every resolved use of an invoked
parameter by parameter symbol identity. The callable expression of each
recorded direct invocation and the exact argument of each proven confined
forwarding edge are permitted; assignment, local copying, field storage,
capture, and any other ordinary value transport are rejected. Expression uses
carry their resolved symbol, while a lambda capture retains the canonical
declaration symbol it captured. Pending unproven forwarding and callable return
shapes keep their dedicated diagnostics, so this audit does not produce a
second error for the same invalid edge. Same-spelling shadowed locals have
distinct symbols and are not part of the callable contract.

`CallableBoundary` and per-argument boundary records replace the former
non-escaping booleans in semantics. Concrete generic reanalysis substitutes
symbolic result and parameter types before validating the selected target, so
a symbolic `T` requirement cannot leak into a concrete `int32_t` instance.
The earlier exact-return/one-field construction transport shapes remain
ordinary clients of the general owned entry rule below, not its exclusive
body syntax. An exact same-type return may coexist with a
`Confined` contract when that parameter has repeatable signatures, no
forwarding edges, and mutable access only when declared `mut`. A callable
argument at either transporting boundary must use explicit `std::move`, remain
exactly typed and movable. Checked borrowed state retains its ordinary
origin/escape/drop obligations through the transfer; unreviewed raw-pointer
capture state is not upgraded. Local closure environment movement and cleanup remain
represented independently and supply its concrete lifecycle evidence.

After confined forwarding reaches its fixed point, a source-defined function's
otherwise ordinary direct by-value type parameter, or nominal generic carrier
such as `Owner<T>`, receives an `Owned` value entry contract. This contract
describes the concrete parameter type, not a
prescribed return/constructor expression. Branches, local owners, normal
transfers, and cleanup therefore use ordinary concrete semantic analysis and
MIR lifecycle verification. It neither upgrades an existing confined contract
nor permits an implicit caller move or unchecked raw capture state. MIR requires
the exact initially available parameter place and, for lexical values, its
initially active cleanup obligation; calls retain their exact consuming input.
An ordinary `CallInput` may stage a nominal carrier after its explicit move.
Verification follows the exact single-use stage back to the ownership event;
the prepared lifetime and its handoff remain independently checked. This does
not admit a reference parameter, bodyless transport, implicit closure copy, or
unchecked raw stored state. A checked borrowed carrier transfers its precise
stored dependencies to the new field paths, not a reused source carrier identity.
Final by-value generic packs receive the same contract per concrete closure
element. Declaration contracts retain the source formal index; instantiated
contracts and call boundaries use flattened argument indices, so fixed prefixes
and non-callable elements cannot shift ownership onto a different argument.
Explicit whole-pack expansion preserves the existing copyable-element copy /
move-only-element move rule. Each copied element gets a fresh prepared lifetime;
moving any move-only element consumes the source pack. Ordinary call arguments
still require explicit closure transfer and preserve checked capture loans.
General native template shape composition and
additional erased allocation/failure profiles remain separate extensions.
The supported public `function`/`move_only_function` wrappers are implemented
in ordinary `<std/functional>` source with their exact signature families,
ownership, dispatch and cleanup evidence; this boundary is not a claim that
those clients are still missing.

Contextual namespace-function selection deduces ordinary fixed arguments and
the final value pack before comparing a confined invocation signature.
Substitution precedes `concreteCallArguments` expansion; forwarded elements
use the same value/read-only/mutable-reference matching as ordinary pack
invocation.
An effect-free function name may defer its own selection without postponing
later typed arguments. Effectful contextual producers still defer their suffix,
and concrete HIR/MIR retain source-order call-input stages. An open forwarding
pack cannot distinguish overload arities; a unique eligible item can remain
symbolic until the ordinary concrete invocation check. Failed contextual
selection owns its diagnostic instead of suggesting an unspellable explicit
function-item type.

Each exact callable signature also retains a `Read`, `Mutable`, or `Once`
invocation capability. An immutable by-value generic callable parameter
requires read-callable invocation. A `mut` by-value parameter permits mutable
invocation and accepts either a read-callable or mut-callable target; concrete
reanalysis records which one was selected. A lambda with an exclusive borrowed
capture is mut-callable and noncopyable; other lambdas are read-callable. Value
capture fields remain immutable whether initialized by copy or owned move.
A class `operator() ... mut`
is mut-callable. A direct `std::move(operation)()` on a by-value generic
parameter requires `Once`, consumes that parameter place, and may select an
exact read-callable, mut-callable, or trailing-`&&` once-callable target.
Ordinary move-state analysis proves at-most-once use across branches and loops;
confined forwarding into a once-callable parameter must likewise move the
source. A consuming-only target cannot satisfy a reusable read or mutable
client. `GTI-S2046` reports capability and confined-forwarding mismatches
before HIR lowering, while ordinary move diagnostics own repeated use.

Explicit consuming invocation retains a full-expression owner even when the
receiver requires active cleanup. An immediate nominal or stored-lambda field
invocation in a consuming method additionally records `retainedReceiverProjection` on the exact
resolved Move and a `ConsumeReceiver` ownership event. It requires a writable
owned field-only path rooted in `this`, consumes availability, and leaves
cleanup ownership with that receiver. Only transparent callee grouping is
ignored: a Move used as an argument, returned value, or member-access base is
still an actual extraction. The selected field-call target must return a value
without tracked borrowed state; reference-returning fallback overloads receive
`GTI-S2022` before HIR. Ordinary reference/pointer dereferences gain no owning
capability from this rule. Stored lambdas remain read-callable and retain their
complete resolved closure identity; consumption does not permit capture mutation.

### Namespace Function Items

A bare namespace function name in a value context selects one eligible
non-generic, body-bearing GTI declaration. With no required signature, multiple
eligible declarations are ambiguous. Existing confined callable contracts and
managed-thread task contracts can instead select one exact matching target.
Semantics constructs `SemanticType::FunctionItem` from the selected
`FunctionId`, result, and parameter list; neither HIR nor a backend sees an
overload set or must infer which declaration the source intended.

The expression records a function occurrence for hover and definition, the
exact target, and read-callable capability. Its zero-state type is trivial,
transfer-capable, and share-capable. Ordinary value rules allow storage,
copy-capture, generic fields, and exact results without closure cleanup.
Function-item calls expand concrete value/read-only/mutable-reference packs
under ordinary argument matching. A callee-producing expression still evaluates
before the arguments even though its selected function identity is static.

Parenthesized construction resumes deferred value arguments in source order
before constructor inference, just as direct braces analyze value arguments.
The deferred flags belong to the current analysis, including concrete generic
reanalysis; an expression entry inherited from another analysis does not prove
the argument was analyzed here. Contextual array tails retain their selected
constructor parameter context. Ordinary direct function calls and the separate
named-native-callback conversion retain their existing resolution paths.

## Owned Managed-Thread Tasks

Concrete callable-entry matching expands substituted parameter packs before
testing the exact zero-argument signature. An empty pack is eligible; a
nonempty or unresolved pack is not. The rule applies equally to user-defined
callable classes and capability-qualified library erasure.
The compiler-owned inline polymorphic slot derives its concurrency facts from
the erased interface's required capabilities, not its empty source shell.
MIR verifies those facts against the concrete interface independently.

Selection of the trusted private thread-spawn declaration extends the existing
`Owned` callable boundary with
`CallableOwnedTransportKind::ThreadTask`; it does not recognize
`std::thread` or a public method spelling. The accepted value is one fresh or
explicitly moved exact `void()` or context-required `Result()` lambda or
nominal callable, or one contextually selected namespace function item. It
must be movable, transfer-capable, free of tracked
borrowed state, and invocable with no arguments. A typed result must also be an
owned, non-reference, non-pointer, transfer-capable value matching the private
result-handle type exactly. Every lambda capture participates in the same
recursive transfer facts used elsewhere; references, raw pointers, borrowed
carriers, cleanup-based denials, and nominal opt-outs fail before HIR.

For a generic source wrapper such as the source-defined `thread::start`, the
callable parameter contract records one owned thread-task destination only
when the exact parameter move feeds the trusted spawn operation. The ordinary
path-sensitive move checker supplies at-most-once transport. `GTI-S2084`
reports an invalid task or unavailable execution/target capability and adds
related information for the first structural transfer denial where one
exists. Semantics also records the exact private handle operations and spawn
call. For a concrete lambda this is its exact `LambdaId`; for a nominal
callable it is the unique public, non-generic, exact `void operator()()`
or exact context-required result declaration selected for the consumed
receiver, preferring consuming over mutable over read-only when those distinct
receiver forms exist. For a function item it is the already selected namespace
function instance. The selected identity, invocation capability, and exact
result type are snapshot-local semantic facts. Later phases neither reopen
overload selection in C++, infer callable shape from a host type, nor repeat
transfer analysis. A direct fresh class construction is accepted by the same
owned-value rule as a fresh lambda; a named task still requires one explicit
move.

### Scoped-task type prerequisites

`SemanticVisitor::scopedThreadTaskShapeFor` is an internal preparation query,
not executable scoped-thread admission. It selects an exact safe `void()` task
entry and enumerates every supported stored reference as a resolved field path
with its exact reference access/type. It reuses structural capability analysis
with a confined boundary: a shared reference requires ordinary referent sharing,
and an exclusive reference requires ordinary referent transfer. Nominal nested
carriers and concrete generic substitution use the existing stored-dependency
path machinery. Ordinary transfer/share traits are unchanged. Owning erasure,
shared owners and synchronization storage retain their existing owned contracts;
the scoped query does not let those facilities hide borrowed dependencies.

The query preserves thread-affinity and unaudited-cleanup denials. Even an unsafe
positive assertion on the task cannot remove the separate requirements on its
checked referents. Missing dependency paths reject the shape rather than losing
the borrow. This result has no origin places, endpoint, or joining proof and
cannot authorize a spawn alone. The identity-bound private `thread_scope`
operation now resolves consumed concrete inputs into `ResolvedScopedThreadTask`
records: each exact field path is paired with its checked origin place and
access, and sibling accesses use ordinary place-overlap analysis. Read/read
sharing and disjoint projections are admitted; overlapping mutable access,
unconsumed tasks, affine referents and missing origins are rejected in semantics.
The concurrent profile and target thread capability are required. The call is
classified as a join failure-propagation boundary, not a recoverable conversion
of worker failures into its integer creation status.

This internal operation is not yet exposed through a public source wrapper.
Concrete pack-element origin retention, full invocation-loan endpoints,
verified MIR child accesses/join-all cleanup, and production adapters remain
implementation work. HIR preparation alone is not executable acceptance.
The public owned-thread boundary above is unchanged.

Composed borrowed locals may reborrow exact reference-formal entry permissions
without treating those permissions as competing loans. This applies to ordinary
constructor/function borrow mappings, not just scoped work. Only the exact
mapped reference entry is exempted when checking the new loan; unrelated local
children remain conflicts, and the composed dependencies continue to prohibit
parent mutation while the carrier is live. A primary origin from a mutable
reference formal retains that exact entry as its parent; independent fields
retain their own origins instead of being folded under that parent.

## Temporary Value Call Capabilities

Resolved ordinary method calls and class `operator()` calls retain a
`CallReceiverMode` alongside the exact selected target. The mode distinguishes
read-only and mutable source places, reusable values, consuming receivers, and
read-only or mutable borrows of a fresh temporary. Semantics alone classifies
freshness: ordinary construction, by-value GTI calls, and value-producing GTI
operators qualify for the bounded ephemeral-borrow contract. Receiver-only
materialization also accepts owned movable class values from conditional
expressions, lambda calls and copy/move construction; this does not broaden
ephemeral argument borrowing. Reusable
targets remain non-static with non-borrowing results, apart from the existing
nonescaping read-observation exception. A virtual target, including an
override or interface implementation, keeps `CallDispatch::Virtual`: dispatch
selects the body, while the receiver role, result restrictions and
full-expression lifetime are the selected member's contract, which every
override shares. Transparent grouping
is ignored, while an explicit move from named storage is not fresh. Concrete
generic reanalysis repeats these checks using substituted
type and lifecycle traits. Expression value category and access remain
unchanged, preventing the receiver-only capability from authorizing general
mutable-place operations.

`freshTemporaryReceiverValue` extends that receiver assessment, but not
`freshTemporaryValue`'s separate temporary-argument contract, to direct
ordinary GTI construction/calls producing a class with a nonempty proven stable
stored-dependency set. It reuses `storedBorrowDependencies`, the resolved producer
summary, and the existing stable-storage query. Native, virtual, unresolved,
or temporary-owner production fails closed. Multiple proven stable origins
do not themselves reject the immediate receiver. The selected method
cannot export a reference, tracked borrowed result, or untracked pointer-like
result. The stored dependency retains its producer-established access mode;
mutating the carrier shell never upgrades a shared external-owner loan.
Ordered call exclusivity retains that dependency through the complete
expression, including later arguments and operands. Unsupported immediate
receiver uses keep `GTI-S2081`, with the failed lifetime condition, selected
declaration, and a named-storage hint. LSP consumes these same semantic facts
and diagnostics without a separate receiver rule.

The existing passive shared-carrier `Value` receiver remains distinct: a
copyable carrier without active cleanup can observe multiple exact stable
read-only dependencies through an ordinary method with a non-borrowing result.
The stable-storage query also admits abstract caller-owned dependencies of
carrier parameters; `protectsStorage` describes storage accounting, not whether
such a dependency is valid.
It does not gain a mutable physical-shell loan. Receiver-argument exclusivity
and later-operand transient checks consume every stored owner dependency, not
only the producer's primary origin, so a conflicting second owner receives
`GTI-S2017` before MIR construction.

Ordinary methods and arithmetic/bitwise value operators can declare a
consuming receiver with trailing `&&`. Overload selection prefers this exact
capability for an owned movable value or explicit `std::move` receiver, never
for a named place or reference. Otherwise-exact reusable overloads remain
eligible. Consuming calls retain their full-expression owner and cannot return
a reference or tracked borrowed state; concrete generic instances repeat the
check. Virtual/interface signatures preserve the receiver qualifier.
`std::move(this)` is an explicit whole-receiver transfer inside a consuming
method: semantics records the exact receiver place and rejects overlapping
live loans, partial moves, and subsequent reads, writes, or implicit calls.
Implicit return movement does not infer this transfer from `return this;`.

`ResolvedCallInfo::ephemeralBorrowArguments` separately records the exact
written argument indices that use the bounded call-only read-borrow fallback.
Candidate selection first resolves the ordinary exact tier and admits this
fallback tier only when the first is empty. Each recorded argument must be a
direct ordinary construction or by-value GTI call producing a concrete,
self-contained class/struct value; the selected parameter is exact read-only
`T&`, and the selected GTI definition's result cannot retain the loan. This
metadata authorizes only the selected invocation and leaves general expression
value category, access, and reference-binding rules unchanged.

## Concepts And Requirement Contracts

Concept resolution keeps two related representations because unary facts and
multi-type relationships answer different questions. `GenericConstraintSet`
is the compact set of primitive and lifecycle facts attached to one type
parameter. `RegisteredConcept::StructuralPattern` retains an irreducible
relationship plus the ordered indices of the concept parameters to which it
applies. Source concept composition remaps both forms through each concept
application; public concept names never become semantic enum cases.

Source member requirements add one general `ReadMember` relationship carrying
the member name, receiver/result/parameter types, receiver access, and explicit
unsafe-call permission. Both direct boolean queries and named concept leaves use
the same semantic signature check. Composition
remaps the arguments and preserves the name; disjunction must not merge facts
for different methods or erase an unsafe obligation. A compiler-private member
is admitted only under the call-admission rule of the requirement's member
token: direct queries and trailing-clause leaves record that trust
(`AppliedConceptRequirement::trustedMemberAccess`), and named-concept leaves
never do, so public concept satisfaction cannot depend on a compiler-private
member. Symbolic bodies can call a guaranteed exact member under its receiver
and safety contract, but concrete instance analysis must resolve the real
method before HIR. Query results become
ordinary boolean HIR/MIR literals, never
synthetic runtime calls. Neither the constant evaluator nor the backend decides
whether a type supports cloning. `std::clone` and vector element duplication
remain ordinary source-defined generic bodies and verified-MIR calls.

A function or constructor's trailing clause is resolved once into
`AppliedConceptRequirement` records containing the selected `ConceptId`, AST
application, and exact semantic type arguments. Arguments may be visible
non-pack parameters, closed concrete types, or dependent compounds.
`resolveRequirements` first forms the complete application identities and
types, then validates the written arguments within that same clause's ordinary
guaranteed-fact scope. This makes conjunction order irrelevant without assuming
facts from only one disjunction alternative. Invalid and unknown arguments
still receive ordinary type diagnostics; concrete admission is rechecked.
Only dependent applications supply provisional admission facts; a closed
clause assertion cannot replace a closed argument's actual capabilities.
During symbolic body analysis, a
requirement scope contributes unary facts to ordinary constrained operations
and supplies synthetic exact operator or callable candidates for the bounded
structural relationships. Callable requirements carry unary/binary argument
and owned-result types plus read, repeatable mutable, or consuming-use
capability; iterator-aware predicate and operation relations derive the exact
checked-dereference referent privately. This is semantic proof, not text-based
operator guessing. The AST application is retained for source-facing
diagnostics and tooling occurrences.

Registration-time constraint diagnostics use the canonical semantic type
printer with a read-only lookup into declaration-owned generic parameter
records when completed model rows are not available yet. The reporter consumes
that spelling operation without owning proof or registration state; it does
not manufacture incomplete `FunctionInfo` rows to obtain a parameter name.

Unary structural annotations on class generic parameters reuse those same
applications. The parameter's resolved `conceptId` remains the concrete
class-argument admission contract; each symbolic type-parameter scope also
publishes its application in the ordinary requirement-scope stack. Constructors,
methods, field initializers and destructors therefore see the same guaranteed
member facts and unsafe-call obligations as a trailing clause. The paired type
scope owns the application's endpoint, including isolated instance analysis.
Concrete parameters do not receive assumed applications: type resolution checks
their actual member signatures before HIR can select an instance.

Call or construction viability substitutes concrete type arguments into the same records and
checks both unary and structural facts. Concrete generic reanalysis then
resolves the real operator declarations in the instantiated body, so HIR sees
the selected `FunctionId` rather than a compiler capability or unresolved C++
operator. Requirement failure filters a candidate but does not rank viable
candidates or change the exact-overload identity rule.

The current structural checks are deliberately identity- and shape-based:
iterator movement/read/write relationships, exact domain-value operators, and
exact public callable signatures. A callable result must be the requested
owned non-borrowed type. Each readable argument is accepted either by exact
value or exact read-only reference, and receiver implication follows the
existing `Read` -> repeatable `Mutable` -> `Once` use-site lattice. Concrete
reanalysis still selects one exact lambda or `operator()` target and retains
its identity and capability in HIR/MIR. Adding a relationship requires one new
irreducible semantic question and a source-defined public client; it must not
be implemented as a public-name case or backend expression probe.

### Protocol Concepts And Conformance Blocks

A concept with a body ([ADR 028](../decisions/028-protocol-concepts.md))
resolves to `RegisteredConcept::protocol` state:
- its associated names, in declaration order;
- its operations' signatures, in the concept parameters' space.

While the operations are typed, each associated name maps to a projection of
the parameters. The concept has no atomic or structural facts, so an inline
constraint cannot express it and is rejected. The concept's name and
associated names are also recorded in the `SemanticModel`, so the type
printer spells a projection as `C<A, B>::name`.

**Parser desugar.** The parser rewrites each `for concept` block into a
namespace whose generated `__gti_conformance_<digest>` name no source can
spell. The digest is derived from the concept application's tokens, so it is
stable across compilations and distinct per block. The namespace holds:
- the `ConformanceDecl` itself;
- the block's functions, as ordinary generic functions over the block's
  parameters.

Every function also carries a requires clause naming the block's own
application, under a generated `requires` keyword. Each existing declaration
and lowering pass therefore sees ordinary namespace functions. HIR enqueues their
instances from ordinary `ResolvedCallInfo` records, MIR lowers them like any
generic function, and the C++ backend names them by the existing instance
identity, which includes the generated namespace.

**Registration** happens in three stages before the catalog is sealed:
1. `registerConformances` runs right after concept resolution. It resolves
   the concept from the block's enclosing namespace, then checks each
   argument's pattern shape, that every block parameter appears, ADR 020
   package coherence over the concept and every pattern's top-level type, and
   overlap with earlier blocks. Overlap unifies two blocks' patterns
   position by position: a union-find over both blocks' parameters, where
   each class may hold one closed type. Patterns are flat, so parameters only
   ever bind closed types or each other, and no occurs check is needed. It
   records each `using` type in the block's parameter space. A block with
   missing or extra associated types still registers, so its operations are
   still checked and its uses do not report a second error. This stage needs
   only the identities of the block's written types, not their validity.
2. `bindConformanceOperations` runs after namespace symbols exist. It selects
   each operation's function from the generated namespace and compares both
   signatures in the block's parameter space, with projections of the block's
   own pattern resolved to its types.
3. `validateConformanceTypes` runs with the type-alias constraint check,
   after every block is registered and each class's members and lifecycle
   are recorded. It validates each block's patterns and `using` types once,
   whether or not the block registered. A concrete type's constraints and a
   projection through a block declared later are therefore decided as in any
   other declaration. The restated requires clause on each block function is
   resolved for its body but not validated again, so a pattern problem
   reports once at the block rather than once per operation.

`SemanticCatalog::conformances` is copied into instance-analysis snapshots and
included in the instance-reuse fingerprint.

**Selection.** `matchConformance` canonicalizes concrete arguments and
returns the block whose patterns they match, position by position, with the
block's parameters bound from the class arguments. A repeated parameter binds
on its first occurrence and must agree at the others. Satisfaction of an application of a concept with a
body first uses the ordinary forwarded proof from active requirement scopes.
Otherwise it requires a matching block whose parameter constraints hold for
the binding.

**Projections.** A dependent `C<T, U>::name` is an opaque `TypeParameter`
with no parameter identity. It carries `projectionConcept`,
`projectionMember` and the concept's arguments as its arguments.
`substituteType` substitutes the arguments, and
`selectExactClassSpecialization`, which ends every type resolution and
substitution, reduces the projection once its arguments are concrete. Reduction
is depth-limited, so a cyclic block cannot recurse without bound.

Inside a block's function, a bare associated name and a projection of the
block's own pattern resolve to the block's `using` type, mapped onto the
function's copies of the block parameters.

**Operation calls.** A concrete `C<X>::operation(...)` resolves its callee to
the selected block function's symbol, binds the block parameters as explicit
type arguments, and then follows the ordinary single-overload call path.
Through `requires C<T>`, a generic body checks arguments against the declared
signature and records a symbolic call; each concrete instance resolves the
real function.

## Transfer And Share Facts

`SemanticTypeTraits` is the single authority for the independent
`transferCapable` and `shareCapable` facts. Scalars and enums are positive;
arrays, ordinary classes, concrete generics, and read-only callable
environments compose their component facts. References,
stored borrowed state, string views, and raw pointers are negative in the
initial profile. Compiler-owned unique-owner and storage types recurse through
their element type by identity rather than wrapper spelling.

Nominal classes use a cycle-aware greatest fixed point, so an owning edge such
as `unique_ptr<Node>` does not make `Node` fail merely because the query is
recursive. A destructor with a source cleanup body suppresses both automatic
facts; an explicitly defaulted destructor remains structural. The
class-owned policies `Denied`, `Required`, and `UnsafeAsserted` represent safe
opt-outs, interface requirements, and explicit unsafe positive assertions;
they are retained in `ClassTypeInfo`, while each concrete expression, binding,
lambda, and HIR class instance carries the effective facts. Generic parameter
queries consume the compiler-bound `transferable`/`shareable` constraint bits.

`GTI-S2059` owns unknown, misplaced, duplicate, and conflicting declaration
policies and failed interface implementation proofs. The implementation type
name is the primary span for a failed proof and the declaring interface
attribute is related information. The backend never recognizes capability
attributes, public concept names, or standard-library wrapper names.

`TargetInfo::executionProfile` is resolved by the direct/project driver before
frontend entry and copied into `SemanticModel`. During variable analysis, the
concurrent profile requires every namespace global and class static field to
be immutable and to have a share-capable resolved type. The check consumes the
same recursive `SemanticTypeTraits` facts as generic constraints, so aliases,
concrete generic instances, raw pointers, explicit nominal
policy, and internal linkage do not create alternate paths. `GTI-S2060` uses
the binding name as its primary span and, for nominal state, relates the first
explicit opt-out, base, stored reference, or structural field that prevents
sharing. The profile-independent active-cleanup check described below precedes
this policy and suppresses a duplicate `GTI-S2060`. The single-threaded profile
bypasses only the concurrent check. No backend or public wrapper spelling
participates.

Ordinary static members of generic classes remain rejected by `GTI-S2039`,
apart from exact registered collection-protocol hooks. Implemented qualified
generic type-member paths do not establish a general static callable/storage
contract. The concurrent-policy path suppresses a redundant `GTI-S2060` only
under that existing rejection. Lifting the generic-static restriction must remove the
suppression and apply the same immutable/share-capable rule to each concrete
generic static identity.

## Place And Ownership-State Authority

M-OWN-01 adopted one value-owned `PlaceKey` and an exhaustive equal,
directional-prefix, disjoint, or may-alias relation in
[`place-and-ownership-state.md`](../plans/place-and-ownership-state.md).
Semantics owns source place formation, call-origin substitution, ownership
events, source control-flow state, and diagnostics. This preserves ADR 001 and
keeps ownership diagnostics available before concrete-instance lowering. HIR
must carry the accepted concrete key/event; MIR later verifies the same finite
ownership-state transfer over executable CFG joins and backedges.

M-OWN-02 implements the directly owned fixed-array slice. The semantic model
records value-owned `PlaceKey` and `OwnershipEvent` facts for reads, moves, and
reinitializations. Resolved fields use `SymbolId`; an in-range evaluated
constant array index is an exact projection; and each dynamic index evaluation
gets a selection identity but remains may-alias with every element. The shared
relation drives projected move state, loan overlap, branch joins, and loop
backedge checks. A private `SemanticPlace` remains only as analysis-time source
recovery and is converted to the shared value before it becomes durable
semantic data. Raw addresses and opaque results still receive no guessed
checked-loan provenance; optional raw auditing retains separate origin
evidence without manufacturing checked references or lifetime guarantees.

A `SemanticPlace` root points at the `Symbol` stored in the current lexical
scope stack, and an assignment resolves its target place before analysing the
assigned value. Expression-level flow joins (`&&`, `||`, `?:`) and the
`mergedValueStates` scratch restore operand states through
`restoreScopeState`, which assigns each surviving binding in place instead of
replacing the scope maps. A nested logical or conditional value therefore
cannot free or re-key the root that the enclosing assignment's loan-conflict
check, reinitialisation and ownership event still read.

Plain member assignment records the same exact `Reinitialize` event as local
and indexed assignment, including the field's previous availability. Checked
arrow access retains the owner/dereference/field key; restoring the field can
therefore make the whole owner movable again in both semantics and MIR.

A resolved non-static instance field always forms a receiver-rooted `PlaceKey`
with a field projection, whether the source spells `field` or `this.field`.
Borrow-return validation consumes that resolved identity, so lexical locals and
parameters that shadow a field remain symbol-rooted and independent. HIR
canonicalizes the unqualified field value to the same implicit-receiver member
access used by the explicit spelling; MIR therefore retains a `This` root for
direct reference returns, stored-reference construction, and calls made on the
field.

For a by-value direct stored-reference parameter, the semantic entry loan also
retains the exact resolved `SymbolId` of the class's designated reference
field. HIR and MIR carry that fact directionally so MIR verification can bind a
returned runtime place or carrier value to the same formal dependency; later
phases do not infer the field from its type or emitted representation.

Stored carriers generalize this to a set of reference-bearing field
paths and checked owner places. `SemanticLoanInfo.additionalDependencies`
retains the remaining places, access modes, storage protection, and entry
field paths alongside the original primary dependency. Every constructor
binds each borrowed field from an exact parameter; resolved construction/call
records preserve the complete supplying-parameter set. Every owner participates
in conflicts, argument exclusivity, escape validation, and the common carrier
endpoint. Copies alias the whole set only when the carrier's state is copyable
and every retained dependency is shared; moves transfer its shell and complete
dependency set. Shared projections retain the full set conservatively. This
rule applies equally to unrelated source-defined carriers and source-library
views.

Shared and exclusive fields may coexist without a cardinality limit. Each
exclusive origin retains its own access mode and suspended-parent relationship;
only conflicting overlapping places are rejected. Declaration analysis requires
exact constructor parameter-to-field bindings. Field-wise return summaries
identify the result field, receiver or parameter, source field path and access
mode; body analysis validates that inferred summary against actual returned
origins. Selected reference accessors do not inherit an unrelated first field's
permission. Forming a reference selection in a switch arm records a use of its
parent authority and that parent's ancestors, so the carrier's dependency
stays live while the arm forms its selections even when it never reads a
selected binding. Likewise, an arm that binds payloads of an observed subject
reached through a reference (a reference local, or a reference-returning call
that names no place) records a use of that reference's authority and its
ancestors at arm entry, because its extractions read through it; an arm over
a named owned place records none. An observed payload subject must have the
stable borrow storage a reference initializer needs (`GTI-S2017` otherwise).
Concrete generic reanalysis substitutes these same facts.
Lifecycle traits reject copying any exclusive path, and transfer preserves all
parents until the final child/carrier endpoint. Custom destruction,
when present, extends the dependency endpoint through cleanup; it does not
change the origin or access mode. Globals/captures, dependency reseating,
dynamic origins and ambiguous return provenance remain outside this bounded
graph.

A reference returned from an ordinary receiver's owned field or owned storage
protects the receiver's physical storage separately from its external stored
dependencies. Exact stored-reference-field returns instead preserve the
selected external referent. Field-origin summaries distinguish these cases,
including single-path carriers; a type merely containing borrowed state is
not permission to replace its shell origin with its resource dependency.
An exact field-origin path can extend beneath a stored external reference.
An unambiguous nonvirtual accessor relay composes that path, and independent
body analysis checks its actual return. Composition respects the callee's
checked contract: an accessor into receiver-owned storage exposes the receiver
as owner, not an implementation-private field or allocation path. The same
physical-storage classification governs mutable reference initialization;
a read-only external dependency does not make a separately owned field
read-only. Returning through a reference alias
uses the selected stored dependency, not the alias's separate physical-shell
loan. A projected referent retains its exact suffix instead of becoming an
alias of the whole external owner. Dynamic, ambiguous and recursive declaration
provenance remain conservative.
Synthesized range dereference retains the selected physical range-storage loan
before composing field mappings. Its transitive resource dependency remains
separate; borrowing an element cannot be substituted with borrowing the resource
that supplied the container's allocation.
Moving a cleanup-bound carrier updates flow availability and transfers the
active loan, but retains the declaration's historical loan identity so earlier
reference uses still lower against the original shell and dependency.

A direct by-value return of a nested copyable shared view records the exact
receiver field prefix and the result-relative reference path. Copying that
view creates independent shell storage while retaining its external referent.
This is distinct from returning a reference to the field, which also protects
the enclosing shell. Concrete field identities, not public type names, select
the rule.

Addressable namespace bindings and non-generic static data members use the
same symbol-rooted place whether their source spelling is qualified or
unqualified. Function-local borrows from that storage therefore participate in
the ordinary loan-conflict analysis and lower to exact HIR/MIR symbol places.
Function borrow-summary resolution uses a separate declaration registry for
namespace storage because summaries precede lexical body analysis. It reuses
the eventual tooling `SymbolId` without publishing the variable into ordinary
lookup, so this support does not make namespace variables forward-declarable.
When every return in a free/static function resolves to one identical global
or static root and projection path, the summary records `Global` plus that
exact place. Calls substitute the same place directly instead of deriving an
owner from a receiver or argument.
This does not introduce program-wide move-state inference: consuming global or
static storage with `std::move` remains rejected because its initialization
state is not locally provable.

## Loan Flow

Global and local loan-flow places use the same analysis-local domain. HIR
qualifies those places with its concrete snapshot/body identity; qualifying
only global dependencies during semantic conflict checks would incorrectly
make every local carrier potentially alias a global owner.

Retained borrows receive stable semantic loan identities. A move transfers a
carrier; a read-only alias adds another carrier to the same identity, while a
mutable stored carrier cannot be copied. Uses are
recorded by loan rather than by one preferred variable, so endpoint planning
considers every alias across straight-line statements and the supported
conditional, loop, switch, and break shapes. Shared early endings are enabled
for semantically read-only loans. An exclusive stored carrier instead keeps
one origin suspended across its whole moved lifetime; a returned carrier
re-establishes that exact dependency in the caller rather than inventing a new
root.

A value whose class type has no borrowed state can still carry one owner
dependency at a direct text field (`L-OWN-02`, phase 1). Constructor and
designated-construction selection mark the call `dependentTextPublication`
and record a `BorrowedFieldOrigin` from the dynamic argument to the field.
They do not record a complete dependency set: like a dynamic view, the value
uses the retained-loan model. Its expression traits carry the borrowed state,
and its binding's `StoredValue` loan designates the field and names the
argument's owner. A copy adds a carrier to that loan, a move transfers it, and
a read of the designated field yields an owner-dependent view. Code outside the
creating body reads text fields as ownerless, so `noteTextDependentValue`
records each dependent value. A local initializer, a field read, a grouping, a
move or a discarded statement consumes it. `finalizeTextDependentValues`
reports every other use as `GTI-S2045`, so an unsupported context fails closed.
Publication also requires `constructionKeepsPublishedText`. The constructor
body must be empty, because it could read the field back through `this` as
ownerless text. `readsConstructedObject` must also find no read of the object
in another initializer or field default; the language rejects such reads
today, and any expression form the check does not inspect counts as a read.

A function in a field-contract scope (`textFieldContractScope`) carries a
field-level contract, `FunctionInfo::ownerlessTextFields`, beside
`ownerlessTextParameters`. Such a scope is a non-generic, non-virtual GTI
function or method body, including a method of a generic class, outside
lambdas, constructors and destructors. Inside it:
- A text-field read through a formal, `this` or an implicit member of a
  publishable class is an owner-dependent view.
- A sink that already requires ownerless text records (formal or receiver,
  field) through `requireOwnerlessTextParameter`. So does a return that the
  function's field-sourced result does not name; the read is then re-recorded
  as ownerless.
- Each other use of such a formal is noted like a dependent value. A field
  read consumes it. `recordTextFieldCall` forwards a formal that is passed to
  an in-scope callee's formal or receiver of the same class.
- In a generic class method's open analysis, a read of a type-parameter
  field of the read-only receiver or of a contract formal
  (`conditionalTextField`) records the requirement on that field directly
  (`noteConditionalTextFieldRead`). Only a return among
  `conditionalRelayReturns`, which hold when every return yields the same
  such field of the receiver, is exempt. The requirement binds only instances whose field is
  text: `analyzeFunctionInstance` keeps the open requirements whose field is
  a direct text field of the instance, and HIR and the contract proof carry
  those.
- Relays of a generic class method are decided for the receiver's concrete
  class. A call computes `dynamicTextReturnBorrowSummary` and
  `textReferenceReturnSummary` on its concrete callee, and so does the
  instance analysis, so an instance whose field is not text keeps no borrow
  summary. Only the reference relay is also recorded on the open declaration
  (conditionally, for a type-parameter referent): a call reads it where its
  result is a reference to text, and it lets `recordTextFieldCall` relay a
  dependent receiver before the concrete callee exists.
- `finalizeTextFieldContracts` turns each unconsumed use into requirements on
  every text field, then closes forwarding to a fixpoint.

A return of one such field publishes a field-sourced result in the shared
summary instead of a requirement: `returnBorrowOrigin` is Argument or
Receiver and `returnBorrowFields` holds one `BorrowedFieldOrigin` whose
`sourceFields` is that field, the shape of a returned `T&` field.
- `predeclaredTextFieldSource` proposes it before bodies, beside
  `storedTextViewRead`. It follows a helper or method that returns a field of
  a formal it is applied to, so `return first(t);` composes.
- `dynamicTextReturnBorrowSummary` publishes it only when every dependent
  return names the same field. Beside a whole-origin source or another field,
  the read stays an ownerless alternative under a requirement, as before.
- At a call (`fieldSourcedTextResult`), the result is dependent when the origin
  argument or receiver is a dependent value designating the field, or a
  contract formal of the caller; `storedBorrowDependencies` maps the
  designated retained loan onto the result. Otherwise it is ownerless.
- `ownerlessTextForwardingField` treats such a call on a caller's contract
  formal as a read of that formal's field, so sinks, relays and returns
  compose through it.

`dynamicTextReturnBorrowSummary` summarizes recursion as a least fixpoint over
each set of mutually recursive functions. `TextRelaySources` records what the
returns read: their direct origins, their contract-formal fields, and whether
any provenance is unknown. These facts only accumulate.
- A recursive call (`recursiveRelay`) reads the summary computed so far
  through its arguments and receiver. It is ownerless while that summary has no
  dependent return, and otherwise maps as a completed summary does.
- The first function of a set to be evaluated repeats its returns, with one
  evaluation of each other member per pass, until no summary of the set grows.
  A summary that read an unfinished one stays provisional until then. One query
  caches its complete summaries until it returns, keyed by declaration, so a
  generic instance's call through its open signature reads the instance.
- A recursion that relays one origin or one field at every depth relays it. A
  recursion that permutes formals reaches two origins or fields and relays
  neither.
- A call to a function outside the set keeps the existing rules. A completed
  ownerless summary still reads as unknown provenance there, so a relay beside
  an ownerless call is not proposed.
- Body analysis validates every return against the published summary, so a
  recursive call is checked under the contract it assumes.

A payload construction publishes a dynamic text argument the same way:
`ResolvedPayloadConstructionInfo` carries `borrowFields` (result field = the
payload's symbol) and `dependentTextPublication`, and no complete dependency
set is recorded for it.
- `textFieldOwnerDependency` gives the argument's sole named owner. A second
  dependent payload, a temporary owner, or an enum with borrowed state or open
  type arguments keeps the storage error.
- The text-dependent value helpers (`textDependentType`,
  `isTextDependentValue`, `textDependentFields`, `storedBorrowDependencies`,
  local retention) accept a payload enum as they accept a class. The
  retained StoredValue loan designates the payload symbol without a variant
  condition: a dependent value cannot be reassigned, so its alternative is
  fixed.
- A non-consuming `switch` subject consumes the value for confinement.
  `dependentTextPayloadLoan` finds the subject's loan when it designates an
  arm binding's text payload; a by-value binding then carries that loan, and
  an `auto&` binding is `GTI-S2045`.

Phase 2b publishes through payload-enum fields at a path `[field, payload]`:
- `ownerlessTextConstructorInputs` also recognizes, from declarations, an
  initializer `field(E::v(formal))` or `field(E<T>::v(std::move(formal)))`
  whose enum, resolved in the class's namespace, is the field's type. Each
  text payload a formal fills is a `ConstructorTextInput` with its payload
  symbol. While the constructor analyzes that initializer,
  `constructorTextPayloadArguments` names the formal's argument: the payload
  construction neither publishes nor rejects it and records an empty complete
  dependency set. At a call, `textFieldInitializerDependencies` designates the
  argument's owner at the path.
- `textDependentPaths` gives a dependent value's designated paths;
  `textDependentFields` keeps the direct ones. A read of an enum field
  through which a path goes is itself a dependent value, which a `switch`
  observes: `dependentTextPayloadLoan` matches the subject's field prefix
  before the payload. Copying the field out of its value is `GTI-S2045`, and
  so is any relay of a path into a callee.
- `OwnerlessTextField::payload` names a payload requirement. In a
  field-contract scope, a `switch` binding a text payload of a contract
  formal's or the read-only receiver's enum field requires it
  (`contractPayloadBinding`); in a generic class, a type-parameter payload
  requires it conditionally. A whole use requires every text payload too
  (`textPayloadFields`), and `contractTextPath` filters open requirements for
  an instance. A relay is decided against each designated path.
- `payloadReferenceRelayArm` recognizes a reference relay of a payload: an
  arm of `switch (this.field)` that is exactly `return binding;` for a
  read-only `auto&` binding. That binding requires nothing.
  `composedTextReference` lets a method that returns a unique zero-argument
  read-only accessor of `this` relay what the accessor relays, and
  `noteReturnedFormalReference` then notes no whole use.
- An overloaded operator records its receiver's and operands' relays like a
  call (`recordTextFieldOperator`). Its `ResolvedOperatorInfo` carries the
  concrete `returnedTextReference`, and a dereference classifies its result as
  a relayed call does (`operatorTextReferenceRelay`).
- A generic class method's type-parameter formal is opaque to its open
  analysis. An instance requires every text field and payload of a
  publishable argument type there, and a call relays and forwards nothing
  into it, so a container cannot store a dependent value.

A temporary needs no further model. Its construction publishes as above, and
its expression is a dependent value that a field read, a relay or a receiver
consumes, so `fieldSourcedTextResult` classifies a relay of it as it does a
named value's. Only the fresh-temporary receiver mode is extended:
`receiverModeForObject` admits a dependent temporary for a
`receiverTextFieldRelay`, a read-only method whose summary returns one direct
text field of its receiver. That result holds the owner's text and borrows
nothing of the temporary, so `ephemeralBorrowResultSupported` does not apply
to it.

A function may return a fresh value that holds one of its by-value text
formals in a direct text field (phase 2a). Its `publishedTextFields` lists
each such (formal, field) relay; it is a callee fact, not a return-borrow
summary, so an ownerless call keeps no result borrow.
- The relay is decided where the ownerless requirement used to be recorded.
  `relayReturnedTextFormal` runs at a construction of the function's own
  return type, and at a call of another relaying function. It applies when
  the expression is the function's `contextualReturnValue`, the argument is
  exactly the formal, the class is a publication owner, and the constructor
  stores that formal in one field. Any other use of the formal there keeps
  `requireOwnerlessTextParameter`.
- A generic function decides the relay in its open analysis, which records
  its conditional formal requirements today; an instance re-derives it after
  clearing its contract sets. A call reads the callee's relays from the
  body-analyzed declaration and checks the field against the concrete result
  type with `directTextField`. An open call composes the same way: a
  type-parameter argument at a type-parameter field (`directTextField` with
  `conditional`) relays or requires there and publishes nothing. Otherwise a
  generic wrapper whose callee is analyzed later would declare fewer relays
  than MIR derives for its instance.
- `publishTextCallResult` gives a call whose relayed argument is dynamic the
  constructor's call-site facts: `dependentTextPublication`, `borrowFields`
  and an Argument origin. The result is a `textDependentValueInfo` value, and
  `textDependentFields`, `storedBorrowDependencies` and confinement treat it
  as a publishing construction. A temporary owner or a second owner is
  `GTI-S2045`, as for constructors.
- A callee whose body is in `bodiesInProgress` (recursion) may relay formals
  it has not recorded yet into a text field of its result. When the result has
  one, possibly open, the call is marked `provisionalTextRelays`; a formal
  argument there is required ownerless and a dynamic one is `GTI-S2045`. A
  result with no such field is an ordinary call. An instance analysis puts no
  body in progress, so a generic instance relays through its own recursive
  call, while callers are checked against the open declaration's requirement.

A dependent argument or receiver records a `TextDependentRelay` with its
designated fields, while its scope is live. The request's own finalization
decides it, once every body that feeds the contracts is analyzed: it consumes
the relay when the analyzed callee requires none of those fields, and
otherwise reports `GTI-S2045` related to the callee. A callee that returns a
reference to text records a relay only as a receiver relay: its
`returnedTextReference` names the receiver's direct text field that each of
its returns designates. `textReferenceReturnSummary` computes it before
bodies, from `predeclaredTextFieldSource` over the returns, in a
field-contract method. At a call, `textReferenceRelay` treats the result as
a read of that field: it is a dependent text place when the receiver
designates the field or is a contract formal, `storedBorrowDependencies`
gives a copy the receiver's owner dependency, and a contract receiver's
relay forwards the requirement on the field. Any other reference may
designate the dependent field without a loan for the view copied from it,
so it takes no relay, and a contract formal passed to such a callee is not a
forward but a whole use, which requires each of its text fields. So is a
returned reference that a call borrowed from a contract formal or the
receiver (`noteReturnedFormalReference`): MIR sees that call's loan rooted
at the whole formal. An instance analysis
records no field use, forward or relay. Its finalization leaves relays pending
and skips the contract pass, because looking up the functions a contract names
would record them as functions the instance consulted, and demand their
bodies.

For a bounded exclusive reborrow, semantics records a distinct child loan and
its mutable parent. The accepted source place has one stable symbol or receiver
root and named-field, fixed-array index, or checked-dereference projections.
Prefix-overlap of those projection paths is a conflict; divergent paths remain
conservative unless both sides name known, different fields or constant
in-range array indices. A whole root therefore conflicts with its descendants
while sibling fields and exact sibling elements can remain independent. A
mutable parent is suspended while any mutable or read-only child is active and
fully reactivates only after its final active child reaches a frontend-selected
endpoint. Known-disjoint children may coexist, and direct access through a
disjoint parent projection remains valid. The same relation composes into
nested chains; read-only-to-mutable upgrades are rejected. Dynamic indexed,
raw, and opaque sources do not receive precise conflict treatment in this
slice.

Semantics chooses all proven endpoints and reports invalidation conflicts. HIR
and MIR preserve those choices; they do not recompute liveness from emitted
C++ references.

An unretained global-origin call loan is a temporary and receives the ordinary
full-expression endpoint. Retaining a mutable result creates a symbol-rooted
lexical loan that is deliberately not shortened by local last-use planning;
this makes two aliases from repeated singleton accessors conflict until an
explicit nested scope ends the first. The concurrent profile rejects the
underlying mutable global/static binding before this single-threaded loan rule
can authorize unsynchronized process-wide mutation.

## Defined Integer Arithmetic

The public `<std/numeric>` wrapping, saturating, and checked-result functions
are ordinary, exact scalar overloads. Each delegates to one compiler-trusted
prelude operation selected by declaration identity, never by a user function's
spelling. Semantic analysis requires two operands of the same concrete
fixed-width integer type and records the selected operation and mode in
`ResolvedCallInfo`. Wrapping and saturation return that integer type;
`checked_add/sub/mul` return the declared
`expected<T, std::arithmetic_errc>`.

The scalar constant evaluator dispatches those nine identities to the compiled
checked-integer engine. Private `llvm::APInt` computation implements the
mathematical boundary, while `ConstantInteger`, the checked-result record, and
their GTI-owned domain remain semantic representations. Wrapping, saturation,
and checked-result state therefore agree at compile time without exposing LLVM
types or inheriting host integer overflow. Ordinary operators retain their
distinct checked-failure meaning.

## Defined-Failure Meaning

Under [Execution §4.10](../language/execution.md#410-defined-runtime-failure),
semantics owns the bounded local category/detail set of each resolved checked
detector and whether a call-like operation may propagate an existing failure.
Only a language-required constant context or an existing direct-literal rule
may instead issue a frontend diagnostic. Optimizer proof that an ordinary
well-formed expression will fail does not make it ill-formed; its runtime
failure remains observable. HIR binds remaining outcomes to concrete source
anchors and MIR chooses executable failure/cleanup edges. A propagating
call preserves the original record byte-for-byte and does not acquire or select
the callee's possible origin categories. Neither later phase nor the backend
may infer a category from operator spelling or a native helper.

The first semantic identity slice is implemented in `gti/failure.h` and
`SemanticModel::findDefinedFailure`. It gives checked integer arithmetic,
numeric conversion, fixed-array and string-view indexing, unique-owner access,
expected observers, private storage operations, allocation, ordinary GTI
calls, concrete construction, resolved operators, and confined callables one
compiler-owned vocabulary. An expression may retain more than one distinct
origin; outcomes within each origin are sorted and deduplicated. Propagation is
recorded independently for direct, virtual, constructor, and callable edges,
without copying a callee category set or changing the origin anchor. Exact
`nullptr_t` contextual construction is reclassified after constructor
selection so the late contextual step cannot lose its constructor channel.

The failure-metadata builder interns exact frontend origin records into
artifact-local sites, including the two hosted-plan operations below. The
generated hosted startup invokes the source-graph initialization plan through
the merged verified `Module/0` body inside the hosted containment boundary; a
no-entry artifact uses the separately verified initialization guard. Hosted
containment, same-thread callback firewalls, managed-task handoff/re-raise,
and double-cleanup-failure records are implemented for their admitted families.
Remaining trusted host origins, broader function-effect refinement, embedding
activation and foreign-thread callback containment require their own evidence;
generated integrity helpers are not a second source-body authority.

For an owned-argument entry, `HostedProgramEntryPlan` records the exact source
entry, canonical vector/string constructors, append target, source unit, and
`main` anchor. It owns exactly the two adapter-local operations that no source
expression spells:
`hosted_runtime_contract_failure/negative_argument_count` and
`numeric_conversion_out_of_range/hosted_argument_count`. Allocation detectors
remain on the exact vector constructor, string constructor, and append callee
records; the adapter does not re-site them. The stable
`allocation_failure/hosted_arguments` detail is reserved but currently has no
producer. HIR copies this plan as data. A backend may not rediscover any target,
origin, or anchor. `MirLowerer::lowerHostedStartup` builds the executable
hosted-startup body from that retained plan; this is an implemented verified
MIR schedule, not a future backend-to-MIR cutover.

M-LIFE-01 adds the v1 restriction on cleanup-owning namespace globals and
static fields after the existing more-specific unique-owner, storage, and
borrowed-state checks. The recursive semantic trait follows arrays, aliases,
bases, fields, captures, and concrete generic substitutions,
and treats declared cleanup as an active obligation. `GTI-S2061` reports the
outer declaration plus the first available declared-cleanup/base/field cause.
HIR and MIR can therefore rely on the absence of source global/static drop
obligations; this does not define global shutdown.

## Bounded Aggregate Constant Evaluation

Semantics owns eligibility and results of the `L-CONST-01` bounded constant
queries; the staged verified-MIR profile owns execution for its admitted
bodies. Recursive constant nodes retain exact scoped-enum, array, class, generic type, and value-
argument identity; declaration spelling and emitted C++ types are never used as
substitutes for those identities. One execution context carries concrete
generic substitutions, automatic values, and an optional read-only receiver
through constexpr calls and constructors while enforcing the existing shared
4096-step and 64-call-depth limits.

Only fully initialized structural aggregates enter this representation.
Semantics rejects bases, polymorphism, stored references, union state, active
cleanup, mutable receiver execution, and unsupported field types before a
constant is accepted. HIR and MIR retain only facts needed by runtime or
program-storage materialization. The C++ backend may serialize an accepted
constant from its exact fields, but it cannot invoke source constructors or
host constant evaluation to recover language meaning.

Literal-backed text inspection dispatches on resolved `StringViewSize` and
`StringViewEmpty` intrinsic identities and the typed string-view index
operation. Its byte/length kernels live in `constant_evaluator`, independent of
AST or library spelling; indexing preserves unsigned code units and rejects
negative and end-or-later offsets, matching the verified-MIR runtime operation.
The semantic interpreter supplies operands and consumes source-located failures.
Read-only operator expressions normalize their already-resolved receiver and
arguments into the same constant-call boundary as named methods. This shares
generic substitution, argument observation, and completed-definition checks;
operator bodies require staged MIR and never fall back to AST execution.
Within MIR, an exact static read-only nominal call operator uses the ordinary
function target and receiver. Recorded read and non-consuming mutable
capabilities are admitted; consuming invocation, lambda execution and raw
operations remain excluded. This does not add a second overload resolver or
special stdlib path.
Completed constexpr free-function closures, including concrete type/value
generic calls, now execute through
staged ordinary HIR, exact failure metadata, and verified unoptimized MIR before
whole-program analysis completes. `semantic_constexpr.cpp` owns this bridge;
the frontend lends it the request's source graph and source manager so failure
metadata uses real source bytes, not synthesized locations. All staged
identities die with that synchronous request. Definition availability is
recorded after body and scope analysis, not at declaration registration.
For a fully selected body this includes loan-flow validation. A generic primary
with deferred branches is only available for concrete reanalysis: its
availability is not an executable flow proof.

A value- or type-dependent `if constexpr` in a symbolic primary now records an
explicit deferred selection. Neither arm contributes primary semantics. The
body-local flow state records that its CFG is incomplete; name/type checking and
independent assertions continue, but missing-return, move availability,
receiver-restoration and loan-endpoint proofs cannot use that incomplete CFG.
Concrete analysis selects the arm and reruns all those checks with fresh body
state. HIR rejects any still-unselected branch rather than emitting an empty
branch. Unused-mut advice is withheld for a deferred-flow body's locals since
uninspected arms may contain writes.

Required constants encountered during concrete reanalysis of any instance kind
(function, constructor, destructor, or class field initializers and static
assertions) stage against its canonical base model, never its partially
analyzed delta. Nested workers keep separate semantic state and share only a
constant-request work account and the list of instances whose reanalysis is
open along the staging chain. Nested staging counts toward the depth bound,
and its executed work consumes the same 4096-step budget as the enclosing
request. Recursive semantic-staging cycles therefore fail with a resource
diagnostic instead of unbounded reentry. An assertion's constant may need the
instance that owns it, as when a class assertion or a destructor reflects on
the class's fields. A worker then reanalyzes that open instance without
evaluating its assertions: the enclosing reanalysis owns their outcome, and an
assertion yields no value. Other required constants, such as `if constexpr`
conditions, are still evaluated, so a genuine cycle still fails as above.

Concrete reanalysis resets body scopes, so early HIR discovery uses a lazy,
isolated semantic provider rather than reentering the unfinished caller. Its
request-local snapshot copies the sealed catalog and the canonical semantic
model, preserves declaration IDs, and rebuilds owned callable indexes. Ordinary
instance analysis still produces each typed delta; there is no second generic
resolver. Non-generic closures do not allocate this snapshot. Instance deltas
expire after the synchronous lowering operation. The snapshot is reused while
the declaration order, whether a concrete reanalysis is open, and the
available constexpr definitions and constructors are unchanged, and completing
the primary analysis drops it. Its key never includes the model's address,
which moves between the analyzer and each reanalysis scope. The copy drops the
side channels an enclosing analysis lent the canonical model, a late-demand
probe's demand sink and a batch's call journal, which do not outlive that
analysis. Tests retain enclosing locals and live loans across this boundary.
Concrete semantic diagnostics are authoritative and cannot fall back to legacy
execution.

Unsupported closures/profile operations still use the existing AST driver;
work already executed consumes the shared limit, and arithmetic/resource
failures are authoritative. Direct semantic-only callers without source text
also retain the early kernel. Fixed arrays and passive structural record
parameters, local copies, indexed/field mutation, and returned values now use
ordinary MIR places. Class catalogs retain exact concrete field identities;
runtime destruction or borrowed fields cannot be hidden behind passive
constant storage. Completed explicitly constexpr constructors with explicit
field initialization and read-only constexpr methods now execute in that same
profile, retaining exact receiver identity and constructor failure provenance.
Direct constructor and read-only/static method roots use the same request-local
staging service, including exact generic owner substitutions. Declaration-default
schedules and returned-view loans remain staging/profile work. The bridge
does not introduce a new executable representation or weaken whole-program
publication seals. Request metrics count actual completed MIR evaluations so
tests can distinguish this path from compatibility fallback.

`convertConstantToType` is shared by both execution mechanisms in the constant
service. It preserves exact aggregate/enum identity, checked numeric domains,
binary-float formats, and the char/uint8 code-unit conversions. Neither engine
may introduce a separate cast policy.

Constexpr declaration validation checks potential type eligibility before
instantiation. Concrete reanalysis retains signature-shape and ordinary
ownership checks but does not reject a generic runtime-only type substitution
merely because the declaration is constexpr. Required constant execution still
checks exact types and operations independently. Contextually signed integer
literals retain their whole checked mathematical value, avoiding a fictitious
positive intermediate outside the target domain. MIR execution shares the
exact single-use signed-minimum literal query with native emission; ordinary
negation and its overflow behaviour are not relaxed.

Read-only and mutable reference signatures are potentially constexpr-capable when the
referent is supported. Ordinary semantic loans and return-origin checks retain
authority; the staged MIR evaluator alone executes reference calls and produces
the observed referent value, not a persistent reference constant. Failed staging
does not fall back to an AST interpretation of newly admitted reference rules.
Mutable and non-consuming receiver methods/operators and `void` helpers use
that same boundary. Constant execution retains mutable referent origins across
selected calls; semantic access, loan endpoints and return-origin rules are
unchanged. Constructor-body edits require the canonical explicit field-write
schedule. Unsupported executed operations in a mutation-capable staged closure
are authoritative failures, not an invitation to retry a value-copy AST
interpretation. The failure is mapped back through HIR to the executing source
expression, including helpers in another unit.
An AST compatibility caller cannot independently stage a mutable callee: its
snapshots do not retain writable origins. The enclosing body must run as MIR.
Existing reflection expansion
and metadata resolution precede this service and are not reimplemented here.
Constructor reference parameters preserve those same origins. Selected
declaration defaults, explicit nested source constructions and generated default
constructors now stage their canonical field initialization schedules. Stored
references and initialization targets without that explicit MIR schedule remain
outside constant execution.

Ordinary scoped enums use the canonical MIR integral-enum catalog and
`EnumConstant` operation. The executor reconstructs a nominal `ConstantEnum`
with the semantic underlying domain, validates external constant arguments
against that domain, and compares enum switch labels through the shared
constant comparison kernel. No native enum representation or public library
name supplies authority. These passive values use the same checked locations,
aggregate copies, constructor/method calls and shared budgets as scalar
builders. Payload-enum execution remains a separate
profile; it is not admitted by the integral-enum catalog.

## Boundaries

- Resolve names, types, visibility, overloads, access, conversions, ownership,
  and dispatch here—not in HIR, MIR, C++ emission, or the LSP.
- Publish declarations through source-unit visibility maps; the combined AST
  does not imply global visibility.
- Keep constant evaluation backend-neutral in `constant_evaluator.h` and record
  results in semantics before lowering.
- Bind compiler-private intrinsics and native linkage by trusted declaration
  identity. Call-site spelling must not grant behavior.
- Preserve the selected function/class/constructor IDs and source provenance in
  every downstream representation.

Language rules belong in [`docs/language/`](../language/index.md); this document
only describes their implementation ownership.
