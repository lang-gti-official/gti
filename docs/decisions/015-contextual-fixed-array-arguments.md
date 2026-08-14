# ADR 015: Contextual Fixed-Array Arguments

Status: Accepted

## Context

GTI needs concise construction of containers and other APIs from a finite set
of values. C++ offers familiar braces, but `std::initializer_list` also adds a
special library-backed type, list-preferred overload resolution, lifetime
rules, narrowing rules, and interactions with constructors and CTAD. Those
rules would weaken GTI's exact-overload model and make a public library name
part of compiler semantics.

Fixed arrays already provide an owned, inline, exact element type and extent.
They are therefore sufficient to give braces a safe contextual meaning without
introducing another view or ownership category.

## Decision

A brace argument in an ordinary named function, method, or constructor call is
a contextual fixed-array value. The selected parameter must be one exact
by-value `T[N]` type. The elements are initialized under `T`, the written count
must equal `N`, and the value obeys ordinary fixed-array ownership and cleanup.
`{}` value-initializes the selected array extent.

A direct immutable `uint64_t` value parameter on a function or constructor may
be inferred only when it is the complete extent of a by-value fixed-array
parameter. A brace argument supplies its written element count; a named array
supplies its exact extent. Type parameters are still inferred from ordinary
arguments, not from an otherwise untyped brace list. Constructor-local generic
parameters use the familiar `Name<uint64_t N>(T values[N])` declaration shape.

Brace arguments do not create a `std::initializer_list` object, infer a common
element type, trigger CTAD, prefer one overload family, permit implicit
conversions, or participate in constraint ranking. Candidate selection remains
exact. If the available contextual array shapes do not identify one candidate,
the call is invalid or ambiguous.

The backend materializes the selected fixed-array value explicitly. Native C++
list initialization and overload resolution are never semantic authority.

## Consequences

- The feature is generic: any ordinary API can accept `T[N]`; `std::vector` is
  only the first standard-library client.
- APIs get compact braces without adding a non-owning list view or its lifetime
  hazards.
- Different extents are distinct types and inferred values participate in
  concrete HIR instance identity.
- Explicit function/constructor value arguments, arbitrary value expressions,
  value packs, untyped standalone braces, and contextual braces through generic
  `operator()` remain outside this bounded slice.
- Container implementations still choose whether they copy or move elements.
  The current vector client copies from its by-value array and therefore
  requires copyable elements when instantiated through this constructor.

## Rejected Alternatives

- **Adopt `std::initializer_list`:** rejected because it imports special
  overload preference, a hidden view lifetime, and compiler knowledge of a
  public library type.
- **Infer a common type from every brace list:** rejected because it adds a new
  conversion-ranking and deduction system rather than using exact context.
- **Make braces vector-specific:** rejected because the source construct has a
  complete general meaning as a fixed-array value.
- **Lower raw braces to C++ and accept what compiles:** rejected because native
  overload resolution would become a second language authority.
