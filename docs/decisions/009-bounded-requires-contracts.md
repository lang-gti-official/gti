# 009: `requires` Names Exact Concept Contracts Without Ranking

Status: Accepted

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
function clause limited to a conjunction of concept applications:

```gti
requires Concept<A> && Relation<B, A>
```

Application arguments name visible generic type parameters. Concepts compose
compiler-owned unary facts and exact structural relationships through private,
trusted `gti_internal` declarations; public `std` and user concept names remain
ordinary GTI source.

Requirements are validity predicates only. They supply exact facts for
symbolic body checking and are substituted and revalidated for concrete calls.
They do not distinguish overload signatures, rank candidates, provide SFINAE,
select specializations, or admit general requires-expressions.

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

Future disjunction, general expression requirements, associated types,
specialization, subsumption, or constraint-based ranking are separate language
decisions. This ADR does not authorize them.
