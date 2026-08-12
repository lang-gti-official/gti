# 003: Overloads Use One Exact Match

Status: Accepted for GTI 1.0. D-LANG-01 retained exact call matching as an
intentional safety/simplicity rule.

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
callable identity for HIR/MIR/backend/tooling. The bounded integer conversions
available to value-assignment and built-in numeric-expression contexts do not
participate in calls. Any additive call conversion requires a new
proposal proving unique selection without conversion ranking.
