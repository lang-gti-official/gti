# ADR 020: Exact Qualified Class Specializations

Status: Accepted

## Context

GTI's `std::hash<T>` needs a source-defined customization for exact types such
as `std::hash<int>` or `std::hash<AppType>`. Ordinary overloads cannot replace a
class definition, while importing C++ partial specialization, SFINAE,
subsumption, and ranking would conflict with GTI's exact-selection model and
make the C++ backend an accidental language authority.

The spelling must also work across packages. Allowing every package to define
every concrete application would make two dependencies able to publish the
same specialization independently.

## Decision

1. **A qualified concrete target is the specialization marker.** A package-
   scope declaration such as `class std::hash<Widget> { ... };` is an exact
   specialization. It uses no new keyword, must match a generic class or
   struct primary's kind, supplies every concrete type/value argument, and is
   a complete definition. An unqualified `class Name<T>` remains an ordinary
   generic declaration, so the two forms are syntactically unambiguous.

2. **Semantics selects one canonical identity before HIR.** Aliases and value
   arguments are canonicalized, canonical duplicate keys are rejected, and
   registration precedes body analysis so source order is irrelevant. A
   matching application resolves to a distinct specialization `ClassId`; a
   nonmatching application retains the primary and its concrete arguments.
   Generic substitution repeats the same semantic lookup. There is no partial
   matching, ordering, constraint selection, or concrete-over-generic overload
   preference.

3. **Coherence follows primary-or-argument ownership.** The declaring package
   must own the primary or at least one top-level nominal class, struct, or enum
   argument. A nested nominal does not grant ownership. This lets an
   application define `std::hash<AppType>` but not `std::hash<int>`, while the
   standard-library package can define primitive specializations of its own
   primary.

4. **Downstream phases preserve rather than rediscover selection.** HIR queues
   the selected class identity. Lowered representation records retain the
   primary and canonical arguments. The C++ backend emits the corresponding
   explicit full specialization, and compiler language queries use the same
   identity for hover, outline, definition, references, and tokens.

## Alternatives

- A `specialize` keyword was rejected because the qualified concrete target is
  already unambiguous and a new declaration category word adds no meaning.
- C++-style partial specialization and ranking were rejected as unnecessary
  for the `std::hash<T>` client and incompatible with exact selection.
- Unrestricted foreign-primary/foreign-argument specialization was rejected
  because package graphs need one coherent definition of each exact key.
- Backend-only explicit specialization was rejected because semantic member
  lookup, ownership traits, diagnostics, HIR, and tooling must all observe the
  selected class before C++ emission.

## Consequences

- Libraries can expose a generic customization surface and applications can
  provide exact implementations for their own nominal types without a new
  keyword.
- The feature does not supply primitive hash implementations, hashability
  concepts, partial specialization, or associative containers; those remain
  separate standard-library work.
- Moving a specialization between packages or replacing a top-level nominal
  argument can change coherence, so `GTI-S2078` reports the ownership rule
  without a mechanical fix-it.
