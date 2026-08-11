# Semantic Analysis

Status: Current implementation.

Semantic analysis is GTI's authoritative language layer. It resolves meaning
over the syntax-preserving AST and records facts in `SemanticModel`; HIR, MIR,
backends, and language queries consume those facts rather than repeating
lookup or type inference.

## Analysis Order

`SemanticVisitor::check(const Program&)` is staged because later facts depend
on declaration-wide registration. It currently:

1. registers namespaces, aliases, type aliases, enums, concepts, and classes;
2. resolves concepts, aliases, and inheritance;
3. registers function generic parameters and namespace symbols;
4. collects root native-storage symbols and class members;
5. resolves inherited members, stored-reference contracts, and function borrow
   summaries;
6. records class types and lifecycle facts;
7. analyzes declaration bodies in lexical scopes; and
8. finalizes callable forwarding/arguments and semantic occurrences.

This ordering prevents declaration-order dependence. A new declaration kind may
need registration, source-unit publication, tooling-symbol creation, body
analysis, and finalization—not only a visitor method.

## SemanticModel

`SemanticModel` is a set of snapshot-owned side tables keyed by AST identity or
compiler IDs. Important facts include:

- expression type, value category, access, traits, and constants;
- binding types, mutability, ownership/drop/copy/move traits, and source symbol;
- functions, classes, enums, aliases, concepts, constructors, and lifecycle;
- exact selected calls, operators, conversions, constructors, intrinsic
  identity, dispatch mode, and borrow origin;
- class bases, override roots, abstract/polymorphic state, and destruction;
- array extents, switch constants, lambdas, target selections, moves, loans,
  unsafe operations, and completion context.

AST pointers in these records remain valid only while the owning
`FrontendResult::program` lives.

## SemanticDatabase And Tooling

The embedded `SemanticDatabase` exposes snapshot-scoped `SymbolId`,
`SymbolRecord`, and `SemanticOccurrence`. Symbol records currently retain kind,
qualified name, source unit, exact name/declaration/definition spans, type,
traits, access, mutability, static/internal/default-library flags, and generated
state. Occurrences link a source span and role to the resolved symbol and may
retain selected call/construction facts.

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

## Loan Flow

Retained borrows receive stable semantic loan identities. A move transfers a
carrier; a read-only alias adds another carrier to the same identity. Uses are
recorded by loan rather than by one preferred variable, so endpoint planning
considers every alias across straight-line statements and the supported
conditional, loop, switch, and break shapes. Shared early endings are enabled
for semantically read-only loans.

For a bounded exclusive reborrow, semantics records a distinct child loan and
its mutable parent. The accepted source place has one stable symbol or receiver
root and only named-field or checked-dereference projections. Prefix-overlap
of those projection paths is a conflict; divergent paths remain conservative
unless both sides name known, different fields. A whole root therefore
conflicts with its descendants while sibling fields can remain independent. A
mutable parent is suspended while any mutable or read-only child is active and
fully reactivates only after its final active child reaches a frontend-selected
endpoint. Known-disjoint sibling-field children may coexist, and direct access
through a disjoint parent projection remains valid. The same relation composes
into nested chains; read-only-to-mutable upgrades are rejected. Indexed, raw,
and opaque sources do not receive precise conflict treatment in this slice.

Semantics chooses all proven endpoints and reports invalidation conflicts. HIR
and MIR preserve those choices; they do not recompute liveness from emitted
C++ references.

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
