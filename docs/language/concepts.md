# Concepts and bounded requirements

GTI concepts describe primitive, lifecycle, and exact structural properties
that generic type arguments must provide. They are a frontend validity
mechanism. They do not emit C++ concepts, rank overloads, or introduce a
separate compile-time metaprogramming language.

## Concept declarations

A namespace-scoped source concept declares one or more type parameters and
combines existing concepts with conjunction:

```gti
concept sortable<T> =
    std::totally_ordered<T> && std::movable<T>;

concept readable_as<Iterator, T> =
    std::input_iterator<Iterator> &&
    std::accumulates_into<Iterator, T>;
```

Every application argument is an identifier naming one of the declaration's
own type parameters. An application must supply the referenced concept's exact
arity, but may map, reorder, or reuse the enclosing parameters. Concept names
follow ordinary namespace and direct-source-unit visibility. They may be
declared after use within a visible source unit. Definitions may use `&&` or
`and`; cycles and disjunction are rejected.

An inline generic-parameter constraint remains the compact unary form:

```gti
T lower<sortable T>(T left, T right) {
  if (left < right) { return left; }
  return right;
}
```

The named concept must have one type parameter and must reduce to unary
capabilities. Relationships between two or more generic parameters use the
trailing clause below.

## Trailing `requires`

A generic free function or non-polymorphic method may append a bounded
requirements clause after its parameter list and receiver `mut` qualifier,
when present:

```gti
T consume<Iterator, Sentinel, T>(mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  // body
}
```

The implemented clause has exactly this form:

```text
requires concept-application (&& concept-application)*
```

`and` is an equivalent conjunction spelling. Every argument must name a
non-pack type parameter visible on that declaration, and every application
must have the declared arity. Arbitrary types, packs, values, expressions,
nested requirements, boolean predicates, `||`/`or`, and negation are not
accepted in this clause.

The active requirements supply facts while the symbolic generic body is
checked. At a concrete call, the requirements are substituted and checked
again against the inferred or explicit types. A failed requirement removes the
candidate from viability and produces a source-facing constraint diagnostic
when no call is viable. Requirements do not:

- distinguish otherwise identical overload declarations;
- rank viable overloads or prefer a more constrained candidate;
- provide SFINAE-style substitution recovery;
- select a specialization; or
- delegate validity to native C++ constraint evaluation.

The bounded first version does not permit a trailing clause on an operator or
on a virtual, pure, overriding, or interface method. Conditional operators
would require the backend protocol to encode the same condition, while a
polymorphic contract cannot be strengthened safely by an override. Both need a
separate design rather than silently weakening the declared requirement.

GTI therefore retains the exact unique-match rule from
[ADR 003](../decisions/003-exact-overload-resolution.md).

## Ownership boundary

The compiler must evaluate facts that current source concept composition cannot
itself express, such as whether a primitive is integral or whether an iterator
declaration exposes an exact dereference operation. It must not understand
public library names or library policy.

The boundary has three layers:

1. compiler-owned capability records contain irreducible semantic facts;
2. trusted declarations in `gti_internal` bind one fact or exact structural
   relationship by declaration identity; and
3. public `std` concepts and user concepts compose those declarations in GTI.

For example, `std::totally_ordered` is not a compiler enum value. The prelude
defines it as equality plus the irreducible relational-operator fact. The
iterator units likewise define public concepts over private structural facts:

```gti
namespace std {
concept input_iterator<I> = gti_internal::input_iterator_capability<I>;

concept sentinel_for<Sentinel, Iterator> =
    gti_internal::sentinel_for_capability<Sentinel, Iterator>;
}
```

The current structural capability set is intentionally small:

- `input_iterator<I>` requires public read-only `operator*()` returning a
  checked reference and public `void operator++() mut`; transferring an
  iterator into an algorithm is checked separately by ordinary parameter
  ownership rules;
- `sentinel_for<S, I>` requires an exact public read-only
  `bool I::operator!=(S&)`; and
- `accumulates_into<I, T>` requires public read-only `I::operator*()` to return
  a checked reference whose referent is exactly `T`.

The last rule is a deliberate first version. It supports homogeneous numeric
accumulation but not an iterator element type that merely converts to, or can
otherwise be added into, a different accumulator type. A later heterogeneous
contract requires a separately specified exact operation relationship; native
C++ conversion or expression probing must not fill that gap implicitly.

## Trusted bindings

The standard prelude uses reserved syntax to connect an internal declaration to
one compiler fact:

```gti
namespace gti_internal {
@compiler_constraint("sentinel_for")
concept sentinel_for_capability<S, I>;
}
```

`@compiler_constraint` is accepted only in the implicit prelude and only inside
`gti_internal`. Compiler-trusted standard-library units may compose these
declarations into public concepts. Application source cannot forge a binding,
name the private capability, or expose it through a public signature. Binding
names and bound declarations are compiler/standard-library integration details,
not a public language API.

## Frontend and backend contract

Semantic analysis resolves each concept by declaration identity. Unary facts
remain compact capability sets; structural facts retain their ordered type-
parameter relationships. During symbolic body checking, an active requirement
can prove only the exact operator shapes represented by those facts. Concrete
generic reanalysis substitutes the actual types, validates their declarations,
and records the selected operator function identities used by HIR.

Concept declarations and trailing clauses emit no C++. HIR and MIR receive
only validated concrete instances and resolved operations. The C++ backend may
use templates as a transitional representation, but C++ lookup, concepts,
SFINAE, implicit conversion, and overload ranking do not decide GTI validity.

## Standard-library client

`<std/numeric>` uses the bounded model to implement `std::accumulate` in
ordinary GTI source:

```gti
T accumulate<Iterator, Sentinel, std::numeric T>(
    mut Iterator first, Sentinel last, mut T init)
  requires std::input_iterator<Iterator> &&
           std::sentinel_for<Sentinel, Iterator> &&
           std::accumulates_into<Iterator, T> {
  for (; first != last; ++first) {
    init = T(std::move(init) + *first);
  }

  return init;
}
```

The explicit `T(...)` preserves GTI's checked numeric-conversion and
assignment rules for narrow integer types. The compiler recognizes none of the
public names `std::accumulate`, `std::input_iterator`, `std::sentinel_for`, or
`std::accumulates_into`.

## Deliberate exclusions and extension rule

The implemented language does not provide general C++ requires-expressions,
arbitrary expression requirements, disjunction, negation, associated types,
value constraints, specialization, constraint subsumption, constraint-based
overload ordering, conditionally available operators, constrained polymorphic
contracts, or unrestricted compile-time reflection.

Add a new source concept when existing facts can express the contract. Add a
new private compiler capability only when semantic analysis must answer a
genuinely new irreducible question for a demonstrated language or library
client. Do not add a capability for a public class, algorithm, container state,
naming convention, or convenience alias.
