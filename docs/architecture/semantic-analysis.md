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
6. derives transfer/share facts and validates inherited interface capability
   requirements;
7. records class types and lifecycle facts;
8. analyzes declaration bodies in lexical scopes; and
9. finalizes callable forwarding/arguments and semantic occurrences.

This ordering prevents declaration-order dependence. A new declaration kind may
need registration, source-unit publication, tooling-symbol creation, body
analysis, and finalization—not only a visitor method.

Declaration registration order is separate from runtime evaluation order.
[Execution Section 4.2](../language/execution.md#42-evaluation-order) now fixes
strict left-to-right expressions, full-expression boundaries, and a lexical
dependency-first program-initialization walk. Semantics will own the active
ordered child roles, full-expression/transient-loan endpoints, and one
`ProgramInitializationPlan` derived from `SourceGraph` plus source spans. It
must also reject a program-wide initializer unless safe GTI call/effect facts
prove that it cannot access a later initialization step.

Those general facts are not represented today. The analyzer already selects
short-circuit branches and several loan endpoints, but it conservatively
rejects an overlapping transient borrow and mutation in either call-argument
order. The combined AST's dependency parsing order is not a substitute for the
program-initialization plan. M-LIFE-01/M-EXEC-01 must add the downstream facts,
and the semantic restriction may be narrowed only when the matching production
backend family consumes them.

## SemanticModel

`SemanticModel` is a set of snapshot-owned side tables keyed by AST identity or
compiler IDs. Important facts include:

- expression type, value category, access, traits, and constants;
- binding types, mutability, ownership/drop/copy/move and transfer/share
  traits, and source symbol;
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

This general record is not implemented yet. Existing checked-integer and
constant-evaluation enums preserve part of the vocabulary, while indexing,
owners, expected observers, storage, allocation, and host operations use
separate semantic/intrinsic facts that M-FAIL-01 must normalize without moving
source validity out of semantics.

The selected owned-argument program-entry record must also state that its
compiler-generated hosted-startup operation has exactly three local origins:
`hosted_runtime_contract_failure/negative_argument_count`,
`numeric_conversion_out_of_range/hosted_argument_count`, and
`allocation_failure/hosted_arguments`. These facts and the source `main`
declaration anchor are semantic program-entry metadata even though no source
expression spells the native adapter operation; a backend may not synthesize
their meaning.

The intended v1 restriction on cleanup-owning namespace globals and static
fields is not fully enforced today: declared-cleanup value types can pass the
existing unique-owner/storage/borrowed-state checks. M-LIFE-01 must add one
recursive semantic global-admissibility trait covering arrays, fields, aliases,
and concrete generic instances before failure cleanup relies on the absence of
global drop obligations.

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
