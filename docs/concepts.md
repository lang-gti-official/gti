# Concepts and compiler capabilities

GTI concepts describe the operations and lifecycle properties that a generic
type must provide. They are a frontend validity mechanism. They do not emit C++
concepts, participate in overload ranking, or introduce a separate compile-time
metaprogramming language.

## Source syntax

A concept is a namespace-scoped unary declaration whose non-empty right-hand
side combines existing concepts:

```gti
concept sortable<T> =
    std::totally_ordered<T> && std::movable<T>;

namespace math {
concept signed_value<T> =
    std::signed_numeric<T> and std::copyable<T>;
}
```

The declaration parameter must be used as every concept argument. A generic
parameter names one concept with the existing compact GTI syntax:

```gti
T lower<sortable T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}
```

Concept names follow ordinary namespace and source-unit visibility. They can be
declared after use within a visible source unit. Definitions may use `&&` or
`and`, but not disjunction. Cycles are rejected.

This first layer deliberately excludes expression requirements, multiple type
parameters, value parameters, negation, `requires` clauses, specialization,
and constraint-based overload ranking.

## Ownership boundary

The compiler must understand facts that cannot be derived from GTI source, such
as whether a primitive is integral or whether a nominal type has the exact
copy constructor and assignment contract. It must not understand public library
names or library policy.

The boundary has three layers:

1. `GenericConstraintKind` contains irreducible semantic facts only.
2. Trusted concepts in `gti_internal` bind one fact each.
3. Public `std` concepts and user concepts compose those declarations in GTI.

For example, `std::totally_ordered` is not a compiler enum value. The prelude
defines it as equality plus the irreducible relational-operator fact. Likewise,
the prelude defines `std::numeric` by composing primitive numeric
classification, total ordering, copyability, and default initialization.

This keeps aliases and implication policy editable in the standard library and
prevents the compiler from accumulating knowledge of `std` APIs.

## Trusted bindings

The standard prelude uses reserved syntax to connect an internal declaration to
one compiler fact:

```gti
namespace gti_internal {
@compiler_constraint("relationally_ordered")
concept relationally_ordered_capability<T>;
}
```

`@compiler_constraint` is accepted only in the implicit prelude and only inside
`gti_internal`. User source and optional standard-library units cannot forge a
binding or directly use a compiler-bound concept as a generic constraint or
concept requirement. Binding names and bound concept declarations are private
compiler/prelude integration details, not a public language API.

## Frontend and backend contract

Semantic analysis resolves each selected concept by declaration identity and
flattens its conjunction to a `GenericConstraintSet`. Symbolic generic bodies
are checked against that set, and concrete instantiations are checked again
against the substituted type. Diagnostics retain the source concept name while
identifying the first irreducible capability that failed.

HIR and MIR receive only validated generic instances and explicit operations
such as constrained default construction. Concept declarations emit no C++.
The C++ backend may use templates as representation, but C++ lookup, concepts,
SFINAE, and implicit conversion do not decide GTI validity.

## Extension rule

Add a new source concept when existing facts can express the contract. Add a
new compiler capability only when semantic analysis must answer a genuinely new
irreducible question. Do not add a capability for a public class, algorithm,
container state, naming convention, or convenience alias.
