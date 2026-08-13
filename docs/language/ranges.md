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
[`docs/plans/iterators-and-ranges.md`](../plans/iterators-and-ranges.md).

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

The hidden steps follow the ordinary full-expression order in
[Execution Section 4.2.3](execution.md#423-full-expressions-and-cleanup). The
range expression executes once, followed by `begin()` and `end()` in that
order. Each comparison completes before the body; the element binding is live
only for its iteration and is cleaned before increment; increment completes
before the next comparison. Iterator, sentinel, and any future hidden owning
range value are cleaned in reverse initialization order on every loop exit.

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

The range expression must currently be a stable addressable value. The language
execution contract defines how a future permitted temporary is transferred to
a hidden owner lasting for the complete range statement, but the current
frontend rejects that form until its lifecycle and ordered MIR lowering are
implemented; it never relies on backend-specific lifetime extension.

## Generic iterator contracts

`<std/iterator>` exposes the implemented operator subset to generic functions
without making an iterator inherit a category base or teaching the compiler a
public library name:

- `std::input_iterator<I>` proves read-only checked dereference and mutable
  prefix increment; ordinary parameter ownership separately checks whether a
  particular iterator expression can be transferred into an algorithm;
- `std::sentinel_for<S, I>` proves exact public read-only
  `bool I::operator!=(S&)`.

`<std/numeric>` additionally defines `std::accumulates_into<I, T>`, whose first
version proves that read-only dereferencing `I` returns a checked reference to
exact `T`.
These source concepts are implemented over private declaration-identity-bound
structural capabilities and are consumed through trailing `requires` clauses.
They are rechecked for every concrete generic instance; C++ iterator traits,
concepts, and overload resolution are not consulted.

The first ordinary library clients are the default and operation-based
`std::accumulate` overloads, homogeneous `std::inner_product`, and unary
`std::transform_reduce`. They accept exact input iterator relationships while
keeping the accumulator and operation results numeric and homogeneous. The
operation overloads use confined callables whose exact result is supplied by
the accumulator assignment or a typed intermediate. GTI's current
`transform_reduce` is deterministically sequential and invokes its transform
and reduction operations once per element from left to right; unlike C++, it
does not permit reordering. These overloads currently invoke operation objects
through read-only access and require the element, intermediate, and accumulator
types to be the same exact `T`. This establishes a
bounded iterator/sentinel algorithm surface, not a complete range concept,
heterogeneous accumulation, iterator category hierarchy, or C++20 ranges
model.

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
per-element loan. Multiple read-only aliases share one loan and one aggregate
path-aware endpoint plan. Bounded local exclusive reborrows are available for
stable root, named-field, and checked-dereference places, but mutable/exclusive
owner dependencies, multiple or nested owner dependencies,
global/captured/storage escape, dependency-changing assignment, and
iteration-specific loans remain later lifetime layers. Fixed arrays also do
not yet expose
`begin()` and `end()`; the structural protocol and range-for syntax do not need
to change when they do. Confined generic `void` operations, exact `bool`
predicates, exact context-supplied owned value results, and proven forwarding
through other confined callable parameters are available for algorithm
callbacks. Result inference through `auto`, borrowed results, and owned escape
remain unavailable. The bounded input-iterator, sentinel, and exact
accumulation capabilities are implemented, but complete-range,
readable/writable element, sized, and multi-pass capabilities are still
missing. A public `std::for_each` over arbitrary structural ranges should wait
for those capabilities instead of teaching the compiler a public container or
algorithm name.
