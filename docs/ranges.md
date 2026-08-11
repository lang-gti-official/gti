# Ranges and iterators

Status: implemented groundwork subset

GTI range iteration is structural. The compiler does not recognize
`std::range`, `std::array`, `std::vector`, or any other public library type.
The current subset accepts an ordinary class when its exact member operations
satisfy the protocol. Iterators and sentinels may be self-contained values, or
an imported standard-library iterator may use the confined one-owner contract
to retain one read-only checked storage borrow. Fixed-array iteration, owned
temporary ranges, mutable owner-tied iterators, and dedicated iteration loans
remain governed by the staged
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
results carry the owner dependency through semantics, HIR, and MIR. A free
function or static factory may now return a cursor or view derived from one
eligible read-only parameter. The single dependency survives ordinary calls,
concrete generic carrier relays, explicit moves, returns, and drops. An
iterator explicitly created before an ordinary source `while`, body-first
`do`/`while`, or classic `for` stays active across backedges and ends at the
loop's unified exit. This permits owner invalidation after that loop while
still rejecting it in a body that may iterate again. It supports read-only
owner-tied iterators and small factory-built views without a public
compiler-owned cursor or raw pointer. The source-defined `std::string`
exercises this path: its
iterator retains a trusted read-only borrow of the private checked storage
while the compiler remains unaware of the public container and iterator names.
The initial source-defined `std::vector<T>` uses the same path for read-only
iteration over a stable vector lvalue. Its iterator carries an index plus the
storage borrow; it does not expose an address or unchecked cursor. An active
iterator therefore blocks reserve, push, clear, movement, and other mutable
receiver operations under the existing retained-loan rules.
This is ordinary retained-carrier flow, not a dedicated range-level or
per-element loan. Mutable/exclusive reborrows, multiple or nested owner
dependencies, global/captured/storage escape, dependency-changing assignment,
precise shared-alias endings, and those iteration-specific scopes remain later
lifetime layers. Fixed arrays also do not yet expose
`begin()` and `end()`; the structural protocol and range-for syntax do not need
to change when they do. Generic non-escaping `void` operations, exact `bool`
predicates, and proven forwarding through other non-escaping callable
parameters are now available for algorithm callbacks. Generic range
capabilities are still missing. A public `std::for_each` over arbitrary
structural ranges should wait for those capabilities instead of teaching the
compiler a public container or algorithm name.
