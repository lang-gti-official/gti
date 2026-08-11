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

- module initialization;
- concrete class, function, constructor, destructor, and lambda instances;
- enums and resolved class/base/lifecycle metadata;
- executable `HirBody` values, bindings, statements, and root statements;
- source-expression to concrete-value mappings used by the transitional
  optimizer/backend.

IDs are stable only inside one `HirProgram`; zero means no identity.

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
abstract/polymorphic state, virtual roots, and structured base/field
initializers. Function instances retain exact linkage, external symbol where
applicable, resolved dispatch identities, and callable/borrow summaries. A
program-entry function additionally retains its semantic entry kind. The
owned-argument form turns the exact resolved source-defined append
`FunctionId` into a concrete `HirFunctionInstanceId` for the canonical
vector/string specialization; HIR does not rediscover that operation from
`std::vector` or method spelling. This target is a program-root reachability
edge even though the user `main` body contains no source call to it.

## Executable Values

HIR bodies preserve source evaluation order and attach semantic type/category,
access, ownership, selected call/operator/constructor, intrinsic, dispatch,
unsafe, move, and borrow facts to explicit values and statements. Generated
range operations and constructor initialization use the same resolved call
records as ordinary source.

For exclusive reborrows, HIR copies the semantic child-loan identity, mutable
parent identity, stable source place, access mode, and selected child endpoint
set into each concrete body. It does not rediscover place conflicts, decide
whether sibling children are disjoint, or decide when a suspended parent has
no active children remaining; those are resolved semantic facts carried for
MIR lowering and verification.

HIR retains syntax provenance needed for diagnostics and transitional C++
emission. `HirProgram::sourceValueIds` may map one source expression to several
concrete generic values; a source-level optimization replacement is valid only
when all concrete instances agree.

## Boundary

HIR owns concrete identity and typed executable structure. It does not decide
source validity, repeat overload resolution, own body-local CFG repair, define
object ABI/layout, or choose a backend representation. New syntax reaches HIR
only after semantics owns its meaning. New body-local dataflow generally
belongs in MIR.

Current gaps and future instance/backend work are tracked in
[`docs/plans/compiler-roadmap-status.md`](../plans/compiler-roadmap-status.md)
and [`docs/plans/optimization.md`](../plans/optimization.md).
