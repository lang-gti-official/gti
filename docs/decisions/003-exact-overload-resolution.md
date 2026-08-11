# 003: Overloads Use One Exact Match

Status: Accepted for the current language; ergonomic widening remains a
pre-1.0 review item.

## Context

C++ overload ranking combines conversions, templates, specialization, ADL,
and context into a large, fragile selection system. GTI needs predictable
generic and ownership behavior and frontend-owned diagnostics.

## Decision

After explicit generic substitution, an overload is viable when its parameter
types match exactly, apart from the bounded documented raw-pointer
compatibilities. Selection requires one unique candidate. Return types do not
distinguish overloads; no ADL, conversion ranking, SFINAE, or
concrete-over-generic preference participates.

## Alternatives

- Delegate overloads to C++: rejected because the result would depend on
  backend spelling and implicit C++ conversions.
- Reproduce C++ ranking: rejected as unnecessary complexity and a source of
  surprising ownership conversions.

## Consequences

Callers use explicit conversions, and semantic analysis records the selected
callable identity for HIR/MIR/backend/tooling. Before 1.0, GTI should still
evaluate whether a very small specified set of safe widening or polymorphic
reference conversions improves usability without recreating ranking.
