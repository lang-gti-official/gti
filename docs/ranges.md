# Ranges and iterators

Status: implemented groundwork subset

GTI range iteration is structural. The compiler does not recognize
`std::range`, `std::array`, `std::vector`, or any other public library type.
The current subset accepts an ordinary class when its exact member operations
satisfy the protocol and its iterator and sentinel are self-contained values.
Container-owned iterators, fixed-array iteration, owned temporary ranges, and
iteration loans remain governed by the staged
[`iterator-range-proposal.md`](iterator-range-proposal.md).

```gti
for (T value : range) {
  use(value);
}
```

The compiler resolves this source as the equivalent compiler-private core
operation sequence (`auto&` is otherwise confined to a range declaration):

```gti
auto& stable_range = range;
mut auto iterator = stable_range.begin();
auto sentinel = stable_range.end();
for (; iterator != sentinel; ++iterator) {
  T value = *iterator;
  use(value);
}
```

The names above are explanatory. Real lowering uses reserved generated
bindings that are absent from completion, semantic tokens, and user lookup.
Diagnostics produced by those operations map to the source `:` rather than a
synthetic identifier span.
`continue` targets the core `for` increment block, so it always invokes the
selected prefix increment before testing the sentinel again.

## Structural protocol

A range must provide accessible, exact zero-argument `begin()` and `end()`
members. `begin()` returns an iterator value. `end()` may return the same type
or a distinct sentinel type.

The iterator must provide:

- `operator!=(Sentinel&)`, with one read-only reference parameter that exactly
  accepts the sentinel;
- `operator*()`, returning a checked reference;
- `void operator++() mut`, the prefix increment operation.

These are ordinary GTI methods. Access control, generic substitution, receiver
mutability, exact overload matching, virtual dispatch, result ownership, and
borrow checks all run through the normal frontend. C++ argument-dependent
lookup and native range-for semantics are never consulted.

A by-value loop declaration such as `for (T value : range)` copies the
dereferenced element and therefore rejects a move-only lvalue. `for (T& value :
range)` and `for (auto& value : range)` bind a read-only non-null reference.
Leading `mut` requests writable access and succeeds only when `operator*`
returns a writable reference:

```gti
for (mut auto& value : mutable_range) {
  value = replacement;
}
```

The range expression must currently be a stable addressable value. Iterating a
temporary is rejected instead of relying on backend-specific lifetime
extension.

## Current boundary

This layer establishes syntax, protocol resolution, explicit HIR provenance,
MIR control flow, and replaceable-backend call targets. It is enough for
self-contained iterators and sentinels, including generic virtual operator
contracts.

Source-defined container iterators may now retain one read-only owner reference
through GTI's confined stored-reference class contract. Constructor and method
results carry the owner dependency through semantics, HIR, and MIR, and
retaining the iterator conservatively prevents invalidating the owner for the
rest of the function. This supports read-only owner-tied iterators without a
public compiler-owned cursor or raw pointer. The source-defined `std::string`
now exercises this path: its iterator retains a trusted read-only borrow of the
private checked storage while the compiler remains unaware of the public
container and iterator names. Mutable stored borrows and precise last-use loan
ending remain later lifetime layers. Fixed arrays also do not yet expose
`begin()` and `end()`; the structural protocol and range-for syntax do not need
to change when they do. Generic non-escaping `void` operations are now
available for algorithm callbacks, but generic range capabilities and
predicate results are still missing. A public `std::for_each` over arbitrary
structural ranges should wait for those capabilities instead of teaching the
compiler a public container or algorithm name.
