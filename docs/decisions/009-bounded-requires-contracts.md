# 009: `requires` Names Exact Concept Contracts Without Ranking

Status: Accepted

Implementation revision (2026-10-23): unevaluated expression/construction
requirements and constexpr Boolean-call leaves are now supported. This
explicitly supersedes the expression-probing exclusion below: ordinary GTI
semantics checks the expression in an isolated query, never the native backend.
Direct operator/call/construction shapes supply bounded provisional generic
facts; arbitrary compound expressions and opaque constexpr predicates are
concrete validity filters, not inferred inner proofs. Exact-owned non-borrowed
results remain the first-slice boundary. The historical exact member signature
shape retains its stricter matching contract. No overload ranking, subsumption,
specialization or SFINAE is added. See [the current
contract](../language/concepts.md#unevaluated-expression-requirements).

General `std::meta` predicates now expose classification, exact type/enum
relationships and lifecycle/transfer facts. Public concepts compose these and
source expression requirements; obsolete duplicate private concept declarations
are removed. The compiler retains the facts source cannot safely establish.
Strict comparison-signature and borrowed-result structural contracts remain
private until corresponding general metadata/lifetime predicates exist.

Implementation revision (2026-10-11): the original nullary read-only leaf below
has been generalized to exact argument-bearing read-only or mutable instance
methods, including checked reference parameters, dependent compound types and
explicit unsafe requirements. The result remains exact and owned; no expression
execution, overload ranking or conditional operator availability is implied.
See [the current concept contract](../language/concepts.md#exact-member-signature-requirements).

Revision (2026-10-14): [ADR 028](028-protocol-concepts.md) adds concepts with a
body, whose associated types and operations are supplied by conformance
blocks. The exclusion of associated types below now applies only to concepts
without a body.

## Context

Unary concepts can describe properties of one generic type but cannot state
that an iterator compares with a distinct sentinel or dereferences to the exact
accumulator element type. Foundational standard-library algorithms need those
relationships. Delegating them to C++ template substitution would make invalid
GTI depend on backend lookup and diagnostics, while adopting the complete C++20
constraints model would also introduce expression requirements, subsumption,
overload ordering, and specialization machinery that GTI does not need.

## Decision

GTI supports source concepts with one or more type parameters and a trailing
function clause limited to a Boolean tree of concept applications and bounded
exact member signature requirements:

```gti
requires Concept<A> && (Relation<B, A> || Alternate<B, A>)
```

`&&`/`and` binds more tightly than `||`/`or`; parentheses group the tree.
Concrete viability uses ordinary Boolean meaning. Symbolic body analysis unions
facts across conjunction and intersects facts across disjunction, including
the existing callable-capability implication lattice. It therefore exposes
only facts guaranteed by every viable alternative and never combines partial
facts from different branches. Inline unary constraints retain the resolved
concept identity for concrete checks even when that intersection is empty.

Application arguments name visible generic type parameters. Concepts compose
compiler-owned unary facts and exact structural relationships through private,
trusted `gti_internal` declarations; public `std` and user concept names remain
ordinary GTI source.

Requirements are validity predicates only. They supply exact facts for
symbolic body checking and are substituted and revalidated for concrete calls.
They do not distinguish overload signatures, rank candidates, provide SFINAE,
select specializations, or admit general requires-expressions.

The explicit cloning client adds a source-defined exact member signature leaf:
`requires (T& x) { { x.snapshot() } -> R; }`. It admits one safe public
read-only nongeneric nullary instance method with exact owned result `R`.
This is structural signature inspection, not arbitrary expression probing or
execution. The same semantic predicate serves boolean queries and concept
contracts, and symbolic generic bodies gain only the guaranteed named method.
Concrete instances still resolve ordinary calls before HIR/MIR. Primitive
copy construction remains a separate capability; source-defined library policy
can combine it with a member requirement, as `std::clone` does. No clone name
or public type becomes a compiler capability.

The bounded clause applies to generic free functions and non-polymorphic
methods. It is rejected on operators and on virtual, pure, overriding, or
interface methods. Conditional operator availability needs an equally precise
backend representation, and polymorphic requirements need contract-equivalence
rules; neither is implied by this decision.

## Alternatives

- Keep concepts unary and implement each algorithm with type switches:
  rejected because GTI has no safe generic type switch and the relationship is
  part of the algorithm's static contract.
- Probe arbitrary expressions as C++ does: rejected because it creates a
  second compile-time expression language and risks backend-dependent
  validity.
- Adopt C++20 concepts, subsumption, and constrained overload ordering:
  rejected because it conflicts with GTI's exact unique-overload rule and adds
  complexity beyond the demonstrated iterator/sentinel client.
- Make public iterator or algorithm names compiler magic: rejected because
  public policy must remain source-defined and reusable by third-party code.

## Consequences

The familiar `requires` spelling can express relational contracts needed by
`std::accumulate` while overload selection remains governed by
[ADR 003](003-exact-overload-resolution.md). Semantic analysis must preserve
ordered concept arguments, check only compiler-owned exact capabilities, and
resolve real operations again for concrete generic instances. The first
`accumulates_into<I, T>` contract intentionally requires dereference to exact
`T`; heterogeneous accumulation needs a new reviewed relationship rather than
implicit conversion or native expression probing.

The original exclusions are revised by the implementation revisions above.
Specialization, subsumption, constraint-level negation and constraint-based
ranking remain excluded.
