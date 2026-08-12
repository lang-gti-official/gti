# Typed HIR

Status: Implemented concrete-instance and typed-value foundation.

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
- module initialization;
- concrete class, function, constructor, destructor, and lambda instances;
- enums and resolved class/base/lifecycle metadata;
- executable `HirBody` values, bindings, statements, and root statements;
- source-expression to concrete-value mappings used by the transitional
  optimizer/backend.

IDs are stable only inside one `HirProgram`; zero means no identity.

The execution profile is immutable program metadata, not a body operation.
HIR preserves it for later concurrency boundaries but does not repeat the
global/static validity check owned by semantics or infer runtime capabilities.

## Instance Discovery

Lowering seeds non-generic declarations and then drains growing worklists.
Processing a field, call, operator, constructor, destructor, lambda, return, or
parameter type can discover another concrete instance. A fixed pass over the
initial declarations is therefore incorrect.

`HirInstanceIndex` (`include/gti/hir_instance_index.h`, compiled in
`src/compiler/hir.cpp`) answers "has this instance already been discovered?"
in constant time. It is a lookup structure only: the ordered instance vectors
in `HirProgram` assign identity and are the sole thing iterated, so instance
numbering and emitted output do not depend on it.

Concrete class instances retain substituted bases, fields, kind,
abstract/polymorphic state, transfer/share facts and nominal policies, virtual
roots, and structured base/field initializers. Function instances retain exact
linkage, external symbol where
applicable, resolved dispatch identities, and callable/borrow summaries. A
program-entry function additionally retains its semantic entry kind. The
owned-argument form turns the exact resolved source-defined append
`FunctionId` into a concrete `HirFunctionInstanceId` for the canonical
vector/string specialization; HIR does not rediscover that operation from
`std::vector` or method spelling. This target is a program-root reachability
edge even though the user `main` body contains no source call to it.

M-FAIL-01 must additionally materialize a compiler-generated hosted-startup
HIR operation/body for that owned-argument entry. It carries the semantic
entry record's three local origins—negative native count, checked GTI count
conversion, and owned argument allocation—plus the canonical source `main`
declaration anchor. The operation has no source `Expr`, but participates in
failure-metadata interning like every other local detector; it is not implicit
backend adapter policy. D-EXEC-01 additionally fixes validation and count
conversion before the ordered program-initialization plan, followed by argument
construction and final left-to-right parameter transfer.

## Executable Values

HIR bodies retain source operand vectors and attach semantic type/category,
access, ownership, selected call/operator/constructor, intrinsic, dispatch,
unsafe, move, and borrow facts to explicit values and statements. Generated
range operations and constructor initialization use the same resolved call
records as ordinary source. This current child order is useful provenance, but
it is not yet a complete executable schedule: calls do not carry an explicit
receiver/parameter materialization plan, full-expression identities are
missing, conditional values list both arms, general temporary obligations are
absent, and module/static initializer bodies are not one ordered program plan.

Under the accepted D-EXEC-01 contract, HIR must retain named concrete child
roles in semantic order, destination-materialization intent, a snapshot-local
`FullExpressionId`, and the semantic program-initialization step identity.
M-LIFE-01 then assigns concrete temporary/drop obligations and M-EXEC-01
linearizes them into MIR. HIR does not select native statements, repeat borrow
validity, or infer that vector position alone is sufficient for every
expression kind.

`HirValueKind::LayoutQuery` preserves layout-query provenance while carrying
the semantic model's exact `uint64_t` constant and numeric literal value.
Lowering neither recomputes the target layout nor turns the operation into a
backend query. This distinct HIR kind lets structural tests prove that the
frontend-owned result survived the syntax/semantic boundary.

Defined wrapping and saturating arithmetic remains an ordinary
`HirValueKind::Call` whose resolved intrinsic identity distinguishes add,
subtract, multiply, and the two modes. Constant public-wrapper calls
additionally carry the exact `ConstantInteger` chosen by semantics. HIR does
not replace these calls with native overflow behavior or reinterpret them as
the checked built-in operators.

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
read/move/reinitialization facts; complete initialize/drop obligations and
lifetime epochs remain M-LIFE-01 work.

HIR retains syntax provenance needed for diagnostics and transitional C++
emission. `HirProgram::sourceValueIds` may map one source expression to several
concrete generic values; a source-level optimization replacement is valid only
when all concrete instances agree.

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

After HIR, a backend-independent failure-metadata builder consumes those local
origins together with `SourceGraph`, `SourceManager`, the direct/project logical
root, and the canonical pre-optimization site-table rules. It assigns
artifact-local `FailureSiteId` values, maps detector HIR values to them, and
constructs the immutable artifact descriptor. MIR and every backend consume
that compiler-owned metadata; HIR/MIR never calculate the final artifact digest
or retain absolute paths. This pipeline does not exist yet. Current HIR retains
enough AST provenance for the transitional backend but owns neither the general
failure vocabulary nor the metadata product.

## Boundary

HIR owns concrete identity and typed executable structure. It does not decide
source validity, repeat overload resolution, own body-local CFG repair, define
object ABI/layout, or choose a backend representation. New syntax reaches HIR
only after semantics owns its meaning. New body-local dataflow generally
belongs in MIR.

Current gaps and future instance/backend work are tracked in
[`docs/plans/compiler-roadmap-status.md`](../plans/compiler-roadmap-status.md)
and [`docs/plans/optimization.md`](../plans/optimization.md).
