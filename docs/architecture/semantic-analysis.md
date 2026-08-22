# Semantic Analysis

Status: Current implementation.

Semantic analysis is GTI's authoritative language layer. It resolves meaning
over the syntax-preserving AST and records facts in `SemanticModel`; HIR, MIR,
backends, and language queries consume those facts rather than repeating
lookup or type inference.

`include/gti/semantic_analyzer.h` exposes snapshot-owned semantic records,
`SemanticModel`, and the narrow `SemanticVisitor` analysis/query facade.
Declaration registration, lookup, overload selection, flow/lifecycle analysis,
generic reanalysis, and AST visitor algorithms compile once in
`src/compiler/semantic_analyzer.cpp`. Snapshot/database operations and shared
place, trait, extent, and intrinsic classifications compile in
`src/compiler/semantic_model.cpp`; canonical type rendering compiles in
`src/compiler/semantic_type_printer.cpp`. Consumers cannot depend on mutable
analysis state or visitor internals.

## Analysis Order

`SemanticVisitor::check(const Program&)` is staged because later facts depend
on declaration-wide registration. It currently:

1. registers namespaces, aliases, type aliases, enums, concepts, and primary
   classes;
2. resolves concepts and aliases, then registers canonical exact class
   specializations before resolving inheritance;
3. registers function generic parameters and namespace symbols;
4. collects root native-storage symbols, class members, and summary-only
   namespace-global storage identities;
5. resolves inherited members, stored-reference contracts, and function borrow
   summaries;
6. derives transfer/share facts and validates inherited interface capability
   requirements;
7. records class types and lifecycle facts;
8. analyzes declaration bodies in lexical scopes;
9. finalizes callable forwarding and argument contracts; and
10. finalizes the program-initialization plan and semantic occurrences.

This ordering prevents declaration-order dependence. A new declaration kind may
need registration, source-unit publication, tooling-symbol creation, body
analysis, and finalization—not only a visitor method.

Mutable field groups are source-only declaration containers. Member collection
recurses through the active group items with the surrounding access state,
registering every child as an ordinary mutable direct field in textual order.
The semantic model records no group symbol. Its retained origin is used only to
attach related information when an existing field restriction, such as the
read-only stored-reference rule, is triggered by inherited group mutability.

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
an executable source initializer owns an initializer step. Generic-class
static storage remains concrete-instance HIR state and is deliberately outside
this program-wide plan.

A use of later program storage is permitted only when the frontend records an
exact representable constant substitution for that expression. Reads, writes,
address or reference formation, borrows, and reference-boundary arguments are
storage accesses even when the declaration is `constexpr`. The analyzer closes
each executable initializer's effects over exact calls, constructors,
destructors, field/base initialization, and cleanup. An ordinary bodyless GTI
declaration, open generic cleanup shape, or other missing summary is unknown;
`GTI-S2068` rejects a later-storage access or unknown transitive effect before
HIR. This proof does not infer safety from a backend or from `findConstant()`.

`SemanticAnalysisSeal` binds the stable `Program` snapshot, full `TargetInfo`,
active-statement preorder, and exact ordered source-graph/prelude provenance.
HIR copies that seal and must match the same program and target before
lowering. The general ordered-expression work is still incomplete: active
ordered child roles and some transient-loan endpoints remain M-EXEC-01 work,
and the analyzer therefore retains conservative borrow restrictions for
families whose production schedule is not yet authoritative.

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
- array extents, switch constants, lambdas, target selections, moves, loans,
  unsafe operations, selected execution profile, and completion context;
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
model does not query native C++ or LLVM layout. Ordinary class layout remains
absent; a valid `[[c_abi]] struct` is the one bounded aggregate category whose
source-order field offsets, size, and ABI alignment semantics compute and
retain in `ClassTypeInfo`. A `[[c_opaque]] struct Name;` instead records one
incomplete nominal `ClassId` with no layout or lifecycle. Semantic type
validation confines that identity to a one-level raw-pointer pointee; extern-C
and native-record validation admit the pointer while continuing to reject the
handle by value.

Floating expression facts distinguish `SemanticType::Float` (binary32) from
`SemanticType::Double` (binary64). Semantics owns literal width, common numeric
promotion, implicit `float`-to-`double` widening, explicit-only narrowing, and
the selected width passed to constant evaluation. LLVM floating types never
enter `SemanticModel`.

For a binary operator's bounded integer-literal context, semantics recognizes
the complete literal expression, including one unary sign, checks its
mathematical value against the selected concrete operand type, and records a
signed operand fact when the sign expression itself adopts that type. HIR,
MIR lowering, and target representation consume the recorded expression
type/fact; they do not re-decide literal eligibility from native promotions.

`SemanticVisitor::visitLayoutQueryExpr` is the sole authority for source
`sizeof(type)` and `alignof(type)`. It resolves aliases, recursively derives
positive concrete fixed-array size/alignment from `TargetDataLayout`, and
records an exact unsigned-64 constant on the source expression. It reports
`GTI-S2063` for unsupported representation categories, symbolic or zero
extents, and checked size overflow. Unknown types keep the ordinary
type-resolution diagnostic without a second layout error. No host or LLVM
layout query participates.

Native-record validation accepts only non-generic passive structs with public
instance fields drawn from the fixed-width scalar, nested valid native-record,
and one-level raw-pointer families. It rejects methods, lifecycle declarations,
bases, access sections, statics, empty and recursive-by-value records, and
cleanup-owning or symbolic fields with `GTI-S2064`. The resulting immutable
layout is also the authority used by `extern "C"` signature validation and by
recursive unsafe-call classification when a record contains a raw pointer.

Native-union validation is a separate semantic family. It accepts a nonempty,
nongeneric, baseless public field-only declaration and computes maximum-field
size/alignment from target facts, nested valid unions, and admitted passive
records. `GTI-S2066` rejects behavior, hidden lifecycle, initializers,
references, ownership-bearing fields, and recursive by-value edges. Every
resolved member read/write is independently classified as an unsafe union
operation; emitted C++ layout is not semantic authority.

Payload-enum resolution assigns stable declaration-order variant identities,
resolves exact passive field types, and records construction and switch-pattern
selections. Pattern bindings are fresh immutable arm-local symbols. Semantics
records a switch as exhaustive only when it has `default` or every variant is
covered once with valid patterns. `GTI-S2067` owns payload shape, exact
construction, pattern, duplicate, and missing-variant failures. This first
family deliberately excludes ownership-bearing payloads until variant-aware
move/borrow and partial drop state exist.

AST pointers in these records remain valid only while the owning
`FrontendResult::program` lives.

## SemanticDatabase And Tooling

The embedded `SemanticDatabase` exposes snapshot-scoped `SymbolId`,
`SymbolRecord`, and `SemanticOccurrence`. Symbol records currently retain kind,
qualified name, source unit, exact name/declaration/definition spans, type,
traits, access, mutability, static/internal/default-library flags, and generated
state. Occurrences link a source span and role to the resolved symbol and may
retain selected call/construction facts.

Occurrence recording is editor-tooling work and is gated by
`FrontendOptions::toolingOccurrences` (default enabled). The driver's
compilation and `check` paths disable it: only position queries read the
occurrence table, and not building it removes about a third of the
compiler's peak memory on large inputs. Symbol records are always produced,
because HIR and the C++ emitter resolve member identity through them.

`include/gti/language_queries.h` builds compiler-owned hover, completion, and
definition results over the immutable frontend snapshot. `SymbolId` is not
stable across analyses and must not be placed directly in a project index.
Documentation comments, a durable project symbol identity, references, and
rename are not implemented semantic facilities yet.

## Concrete Generic Reanalysis

Generic bodies are checked symbolically, then HIR requests concrete semantic
analysis for discovered function, constructor, destructor, and class-field
instances. Concrete substitutions revalidate ownership, value parameters,
packs, selected construction, capabilities, callables, and borrowed-return
origins. HIR is not a second type checker; it asks semantics for the concrete
facts it needs.

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
unchanged from the previous whole-model-copy design.

A lambda type separates physical closure shape from concrete identity. Its
ordinary type arguments hold the result, parameters, and captures; dedicated
identity fields retain the lexical declaration plus the enclosing
class/function type arguments and class value arguments. Concrete reanalysis
reuses the lexical declaration and substitutes both parts. This distinction is
required even for captureless lambdas with identical signatures: two enclosing
generic instances may give the same source body different compile-time meaning.
Function and constructor instance identity now includes the bounded inferred
`uint64_t` extent arguments used by contextual fixed-array parameters. Lambda
identity retains enclosing function value arguments as a separate identity
component rather than treating them as capture or signature types.

Each lambda capture records its new binding symbol, original source symbol,
exact type and traits, initialization mode, and initializer expression. Bare
captures are immutable copy snapshots. The only owned initializer is
`[target = std::move(source)]`, where `source` resolves in the enclosing scope
to an available local or by-value parameter. Semantics invokes the ordinary
move intrinsic and place-state authority before introducing the new immutable
target binding, so later captures observe the source as moved. Reference,
stored-borrowed-state, global/field, and general init captures remain rejected.

## Confined Callable Contracts

A direct by-value generic parameter may acquire a `Confined` callable
contract when its visible body invokes it or forwards it to another proven
confined parameter. Each call site records an exact parameter list, result
type, required receiver access, and selected concrete lambda or `operator()`
target. A call used as a `void` operation has a `void` result requirement. A
condition supplies exact `bool`; an explicitly typed initializer, assignment,
or enclosing return supplies an exact non-reference value type. Raw pointers
remain non-owning values under their ordinary language rules, and the current
`std::string_view` denotes static literal storage; the callable boundary does
not reclassify either as ownership. `auto` cannot infer a result through an
otherwise unknown generic callable, and reference, tracked borrowed-state, and
lambda-identity results remain rejected.

Confinement applies equally to lexical lambdas and nominal callable objects.
Once a generic parameter is invoked, it cannot appear within that function's
return type or pass through a target parameter without another proven
`Confined` contract. A callable object that is never invoked through the
generic parameter retains ordinary value-transport semantics. Invalid escape
and forwarding edges are rejected before HIR, so later phases never receive a
false confined fact.

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
`Owned` is produced only for a free function's direct immutable by-value type
parameter whose one-statement body moves that exact parameter into the
same-type result, or
returns a construction whose sole exact generic field is initialized by the
constructor's matching parameter move. A lambda argument at that boundary must
use explicit `std::move`, remain exactly typed and movable, and contain no
reference, tracked-borrow, or raw-pointer capture state. Arbitrary storage,
wrappers, inferred closure results, and type erasure do not acquire this fact.
Local closure environment movement and cleanup remain represented
independently and supply its concrete lifecycle evidence.

Each exact callable signature also retains a `Read`, `Mutable`, or `Once`
invocation capability. An immutable by-value generic callable parameter
requires read-callable invocation. A `mut` by-value parameter permits mutable
invocation and accepts either a read-callable or mut-callable target; concrete
reanalysis records which one was selected. Lambdas are read-callable because
their capture fields remain immutable, whether initialized by copy or owned
move. A class `operator() ... mut`
is mut-callable. A direct `std::move(operation)()` on a by-value generic
parameter requires `Once`, consumes that parameter place, and may select an
exact read-callable, mut-callable, or trailing-`&&` once-callable target.
Ordinary move-state analysis proves at-most-once use across branches and loops;
confined forwarding into a once-callable parameter must likewise move the
source. A consuming-only target cannot satisfy a reusable read or mutable
client. `GTI-S2046` reports capability and confined-forwarding mismatches
before HIR lowering, while ordinary move diagnostics own repeated use.

After exact target selection, an explicit consuming invocation also queries
the concrete receiver's recursive active-cleanup property. Cleanup-owning
direct calls use the ordinary operator boundary; cleanup discovered while
reanalyzing a concrete confined generic instance remains a confined-callable
error. Both stop before HIR because the language has no accepted cleanup
schedule for that moved receiver across the enclosing full expression; this is
a semantic/lowering restriction, not a backend fallback.

## Concepts And Requirement Contracts

Concept resolution keeps two related representations because unary facts and
multi-type relationships answer different questions. `GenericConstraintSet`
is the compact set of primitive and lifecycle facts attached to one type
parameter. `RegisteredConcept::StructuralPattern` retains an irreducible
relationship plus the ordered indices of the concept parameters to which it
applies. Source concept composition remaps both forms through each concept
application; public concept names never become semantic enum cases.

A function's trailing clause is resolved once into
`AppliedConceptRequirement` records containing the selected `ConceptId`, AST
application, and exact semantic type-parameter arguments. During symbolic body
analysis, a requirement scope contributes unary facts to ordinary constrained
operations and supplies synthetic exact operator candidates for the bounded
iterator relationships. This is semantic proof, not text-based operator
guessing. The AST application is retained for source-facing diagnostics and
tooling occurrences.

Call viability substitutes concrete type arguments into the same records and
checks both unary and structural facts. Concrete generic reanalysis then
resolves the real operator declarations in the instantiated body, so HIR sees
the selected `FunctionId` rather than a compiler capability or unresolved C++
operator. Requirement failure filters a candidate but does not rank viable
candidates or change the exact-overload identity rule.

The current structural checks are deliberately identity- and shape-based:
public read-only checked dereference and mutable prefix increment for an input
iterator, exact read-only iterator/sentinel inequality, and a read-only
dereference referent exactly equal to the accumulator type. Adding a
relationship requires one new irreducible semantic question and a
source-defined public client; it must not be implemented as a public-name case
or backend expression probe.

## Transfer And Share Facts

`SemanticTypeTraits` is the single authority for the independent
`transferCapable` and `shareCapable` facts. Scalars and enums are positive;
arrays, expected alternatives, ordinary classes, concrete generics, and
read-only callable environments compose their component facts. References,
stored borrowed state, string views, and raw pointers are negative in the
initial profile. Compiler-owned unique-owner and storage types recurse through
their element type by identity rather than wrapper spelling.

Nominal classes use a cycle-aware greatest fixed point, so an owning edge such
as `unique_ptr<Node>` does not make `Node` fail merely because the query is
recursive. A declared destructor suppresses both automatic facts. The
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

Static members of generic classes remain unconditionally rejected by
`GTI-S2039` because qualified generic member paths are not represented. The
concurrent-policy path suppresses a redundant `GTI-S2060` only under that
existing rejection. Lifting the generic-static restriction must remove the
suppression and apply the same immutable/share-capable rule to each concrete
generic static identity.

## Place And Ownership-State Authority

M-OWN-01 adopted one value-owned `PlaceKey` and an exhaustive equal,
directional-prefix, disjoint, or may-alias relation in
[`place-and-ownership-state.md`](../plans/place-and-ownership-state.md).
Semantics owns source place formation, call-origin substitution, ownership
events, source control-flow state, and diagnostics. This preserves ADR 001 and
keeps ownership diagnostics available to semantics-only LSP analysis. HIR must
carry the accepted concrete key/event; MIR later verifies the same finite
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
provenance.

A resolved non-static instance field always forms a receiver-rooted `PlaceKey`
with a field projection, whether the source spells `field` or `this.field`.
Borrow-return validation consumes that resolved identity, so lexical locals and
parameters that shadow a field remain symbol-rooted and independent. HIR
canonicalizes the unqualified field value to the same implicit-receiver member
access used by the explicit spelling; MIR therefore retains a `This` root for
direct reference returns, stored-reference construction, and calls made on the
field.

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

Retained borrows receive stable semantic loan identities. A move transfers a
carrier; a read-only alias adds another carrier to the same identity. Uses are
recorded by loan rather than by one preferred variable, so endpoint planning
considers every alias across straight-line statements and the supported
conditional, loop, switch, and break shapes. Shared early endings are enabled
for semantically read-only loans.

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

The failure-metadata builder now interns exact frontend origin records into
artifact-local sites, including the two hosted-plan operations below. This is
not yet a general executable hosted-startup body: generated startup control
flow, remaining trusted host origins, broader function-effect refinement,
failure successors, cleanup unwinding, and containment remain M-FAIL-01 /
M-EXEC-01 work. Existing transitional backend helpers are not evidence that
those pieces exist.

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
origin, or anchor, and generated executable hosted-startup HIR/MIR remains a
later cutover stage.

M-LIFE-01 adds the v1 restriction on cleanup-owning namespace globals and
static fields after the existing more-specific unique-owner, storage, and
borrowed-state checks. The recursive semantic trait follows arrays, expected
payloads, aliases, bases, fields, captures, and concrete generic substitutions,
and treats declared cleanup as an active obligation. `GTI-S2061` reports the
outer declaration plus the first available declared-cleanup/base/field cause.
HIR and MIR can therefore rely on the absence of source global/static drop
obligations; this does not define global shutdown.

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
