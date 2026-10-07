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

A declaration with trailing default arguments is arity-viable when the caller
supplies at least its required parameter count and no more than its total
parameter count. Only supplied arguments participate in exact type matching.
Defaults do not distinguish overload identities and do not rank viable
candidates; two otherwise-exact candidates remain ambiguous. Omitted defaults
also do not contribute to generic inference.

One bounded fallback preserves exact matching for a fresh, self-contained
class or struct value passed directly to an exact read-only `T&` parameter.
Selection first resolves all otherwise-exact candidates that require no
ephemeral argument borrow. If that tier is non-empty, every candidate requiring
the fallback is discarded. Only when the first tier is empty are candidates
whose sole mismatch is one or more eligible call-only temporary borrows
considered, and selection still requires one unique candidate within that
tier. The number of temporary borrows, generic status, defaults, and return
type do not rank candidates. Consequently an exact by-value overload wins over
an exact read-only-reference fallback without importing conversion ranking.

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

Receiver capabilities are an explicit, separate selection axis. Named places
never consume implicitly. Owned movable temporaries and explicitly moved
receivers prefer an exact consuming instance member, with reusable overloads
as fallbacks. Mutable named receivers prefer mutable over read-only members.
This applies to ordinary methods and the permitted value-producing operators
as well as call operators. The compiler records the selected receiver mode;
the C++ backend does not perform receiver-overload resolution. This permits
builder chains to reuse owned intermediate storage without copying every
intermediate or weakening named-value availability and borrow checks.
