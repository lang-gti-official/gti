# Typed HIR

Status: Implemented concrete-instance, typed-value, normal-exit lifecycle, and
defined-failure identity foundation.

HIR is GTI's backend-independent graph of concrete program instances and
executable typed values. It sits after semantic analysis and before body-local
control-flow lowering. It is not a pretty AST and it is not an independent
type system.

## Inputs And Outputs

`HirLowerer` in `include/gti/hir.h` consumes the checked `Program`, target, and
`SemanticVisitor`/`SemanticModel`. It produces `HirProgram` plus diagnostics.
Concrete generic validity is obtained through semantic instance reanalysis.

`HirProgram` owns:

- the selected execution profile copied from the pre-semantics target facts;
- the semantic analysis seal, exact program-initialization plan, hosted-entry
  plan, and merged module initialization body;
- concrete class, function, constructor, destructor, and lambda instances;
- exact native callback adapter identities linking a callback type to one
  concrete free-function instance;
- enums and resolved class/base/lifecycle metadata;
- executable `HirBody` values, bindings, statements, and root statements;
- snapshot-local full-expression identities plus typed lexical/value drop
  obligations for values that require structural cleanup;
- source-expression to concrete-value mappings used by the transitional
  optimizer/backend.

IDs are stable only inside one `HirProgram`; zero means no identity.

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

For an owned-argument entry, `HirHostedProgramEntryPlan` maps the exact semantic
entry, append function, vector constructor, and string constructor to concrete
HIR instance identities. It also copies the `main` anchor and exactly two
adapter-local failure operations: native negative-count validation and checked
GTI count conversion. Allocation failure remains owned and sited by the exact
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

Before a class type enters the worklist, lowering asks the semantic model for
its exact-specialization identity recursively. A matching primary application
therefore enqueues the specialization's distinct `ClassId`; a nonmatching
application enqueues the primary plus its concrete arguments. HIR does not
compare specialization syntax, rank candidates, or defer selection to C++.

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
place/ownership event used elsewhere. Callable parameters and call values
use an explicit boundary enum rather than a generic "non-escaping" boolean. The
`Confined` boundary retains invocation/forwarding requirements. The bounded
`Owned` boundary retains either an exact same-type generic return or an exact
generic owner/field destination, after semantics has required an explicit
source move. Concrete lowering drops symbolic owned contracts from non-lambda
instantiations, substitutes the exact closure and destination types, and
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

## Executable Values

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
resolved class `operator()` call, or ordinary constructor a `HirCallPlan`.
Eligibility is deliberately narrow: the
target is concrete and non-intrinsic; argument cardinality exactly matches the
selected parameters after caller-side default expansion; no pack expansion is
present; and every
parameter is either a supported scalar/reference form or an exact class value
without borrowed state. The ordinary function family excludes operators other
than `operator()`, lambdas, unresolved deferred callables, and construction. A
concretely selected `operator()` uses the same exact target even when its
generic source call still has a deferred-callable base record. Its receiver is
a read or mutable borrow for a place, a value for a reusable value receiver, or
a `MoveValue` for an explicitly moved receiver or exact trailing-`&&` target.
This preserves a once-callable requirement even when exact overload selection
legitimately falls back to a read or mutable target. Ordinary constructors
require an exact constructor target and at least one parameter;
generated/default zero-argument construction and copy/move special construction
remain unscheduled. The plan
names a function receiver, when present, once and records each argument in
source order with its exact concrete parameter type. Constructors have no
receiver. An input role is value, class-copy value, class-move value, read
borrow, or mutable borrow. A class place is eligible only when it can be copied;
a class value is eligible only when it can be moved. The legacy operand vector
remains source provenance and compatibility-backend input; MIR consumes the
plan as schedule authority for these slices.

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

This is not yet a complete executable schedule. Borrowed-state class values,
whole-pack forwarding and general pack expressions beyond the bounded fold,
unresolved callable calls, operators other than `operator()`, special/default
construction, conditionals, target-place formation, result destinations, and
module/static initializer bodies remain outside the bounded plan.

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

For exclusive reborrows, HIR copies the semantic child-loan identity, mutable
parent identity, stable source place, access mode, and selected child endpoint
set into each concrete body. It does not rediscover place conflicts, decide
whether sibling children are disjoint, or decide when a suspended parent has
no active children remaining; those are resolved semantic facts carried for
MIR lowering and verification.

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

HIR retains syntax provenance needed for diagnostics and transitional C++
emission. `HirProgram::sourceValueIds` may map one source expression to several
concrete generic values; a source-level optimization replacement is valid only
when all concrete instances agree.

Function and constructor instance keys retain type arguments and `uint64_t`
value arguments separately. Contextual braces have already become a concrete
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
belongs in MIR.

Current gaps and future instance/backend work are tracked in
[`docs/plans/compiler-roadmap-status.md`](../plans/compiler-roadmap-status.md)
and [`docs/plans/optimization.md`](../plans/optimization.md).
