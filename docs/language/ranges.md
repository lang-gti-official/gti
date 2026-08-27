# Ranges and iterators

Status: current through the bounded L-RANGE-03 single-owner view release and
the iterator/sentinel algorithm cluster

GTI range iteration is structural. The compiler does not recognize
`std::range`, `std::array`, `std::vector`, or any other public library name.
The implemented range-loan layer supports two compiler-verified strategies:
direct indexed traversal of a fixed array and the exact member protocol below
for a nominal class. Both strategies support read-only iteration and bounded
mutable element iteration.
Iterators and sentinels may be self-contained values, or an imported
standard-library iterator may use the confined one-owner contract to retain a
read-only checked storage borrow or the compiler-generated mutable range
capability.

```gti
for (T value : range) {
  use(value);
}
```

The compiler evaluates the range expression exactly once. For a read-only
loop, an addressable lvalue is retained by one read-only hidden owner
reference. For a mutable-reference loop, an addressable lvalue must provide a
mutable place and is retained by one exclusive hidden owner reference. An
owned prvalue is transferred into hidden loop storage and can supply either
access mode. A second range carrier borrows that owner. Nominal read-only
ranges then use the equivalent compiler-private core sequence (`auto&` is
otherwise confined to a range declaration):

```gti
auto owner = range;            // read-only reference for an lvalue, owned value for a prvalue
auto& stable_range = owner;
mut auto iterator = stable_range.begin();
auto sentinel = stable_range.end();
for (; iterator != sentinel; ++iterator) {
  T value = *iterator;
  use(value);
}
```

A mutable-reference declaration selects a distinct mutable endpoint instead
of overloading the read-only operation:

```gti
mut auto owner = range;        // mutable reference for an lvalue, owned value for a prvalue
mut auto& stable_range = owner;
mut auto iterator = stable_range.begin_mut();
auto sentinel = stable_range.end();
for (; iterator != sentinel; ++iterator) {
  mut T& value = *iterator;
  mutate(value);
}
```

`begin_mut()` is ordinary source-level protocol, not special C++ lookup. Its
iterator must retain the exclusive range dependency and return a mutable
checked reference from mutable dereference. Keeping the mutable endpoint
separate lets ordinary read-only calls such as `range.begin()` and
`range.end()` remain usable without accidentally requesting an exclusive
loan.

The names above are explanatory. Real lowering uses reserved generated
bindings that are absent from completion, semantic tokens, and user lookup.
Diagnostics produced by those operations map to the source `:` rather than a
synthetic identifier span.
For a fixed `T[N]`, the same plan uses an exact `uint64_t` index from zero to
`N` and checked array element selection. It does not synthesize pointers,
require public `begin`/`end` members, decay the array, or ask the C++ backend to
perform native range lookup. `continue` targets the core increment block and
`break` targets the range cleanup block.

The hidden steps follow the ordinary full-expression order in
[Execution Section 4.2.3](execution.md#423-full-expressions-and-cleanup). The
range expression executes once, followed by `begin()` and `end()` in that
order for a read-only nominal range, or `begin_mut()` and `end()` for a mutable
one. Each comparison completes before the body; the
element binding and its read-only or mutable child loan are live only for that
iteration and end before increment; increment completes before the next
comparison. The sentinel, iterator, range carrier, and owner are cleaned in
reverse initialization order on normal completion, `break`, `return`, and
every defined failure path. An owned temporary is destroyed exactly once after
its last active child loan; emitted C++ lifetime extension is not language
authority.

## Read-only structural protocol

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
selected element and therefore rejects a move-only lvalue. `for (T& value :
range)` and `for (auto& value : range)` bind a loop-bounded, non-escaping,
read-only non-null reference. Assigning through that reference or storing,
capturing, or returning it beyond the iteration is ill-formed.

Read-only loans are shared: nested or otherwise simultaneous read-only
traversals may retain the same local, field, or owned range. Each traversal
still owns a distinct range plan and loop-bounded element loan. A mutable
traversal remains exclusive and therefore cannot overlap either another
traversal or an ordinary read of the same owner.

## Mutable structural protocol

`for (mut T& value : range)` and `for (mut auto& value : range)` request
mutable iteration. A fixed array needs no public members, but an lvalue array
must itself be mutable. A nominal class must provide all of the read-only
sentinel/comparison/increment relationships above plus:

- exact zero-argument `begin_mut()` on a mutable receiver;
- an iterator that carries the returned mutable owner dependency; and
- mutable `operator*() mut` returning exact `mut T&`.

The compiler holds one exclusive range loan for the complete traversal and
creates one mutable child element loan per iteration:

```gti
for (mut auto& value : mutable_range) {
  value = replacement;
}
```

See [`examples/65-mutable-ranges.gti`](../../examples/65-mutable-ranges.gti)
for fixed-array and `std::vector` mutation in one runnable program.

The element child ends before `continue`, increment, `break`, `return`, normal
cleanup, or failure cleanup. It cannot be returned, stored, captured, or
otherwise escape the iteration. The parent excludes every overlapping read,
write, move, drop, or second traversal of the owner while the loop is active;
the child is the sole authorized mutation path. This conservatively rejects
container operations such as `reserve`, `push_back`, and `clear` even when a
particular execution might not relocate storage. It also rejects moving the
owner during iteration. Pointer ranges and other non-array/non-nominal forms
remain unsupported. `std::span<T>` is a nominal source-defined range and uses
the same protocol and owner-loan rules. Moving a named range
into owned loop storage consumes that owner, so later reuse is rejected by the
ordinary ownership rules.

`GTI-S2083` diagnoses an immutable owner or a nominal range without the exact
mutable endpoint. Ordinary retained-loan diagnostics diagnose overlapping
access and invalidation attempts.

## Single-owner spans and text views

`std::span<T>` is an ordinary source-defined, read-only range over one checked
prefix-storage owner. `std::vector<T>::as_span()` and
`std::string::as_span()` construct a complete span without exposing an address
or pointer-length pair. A span stores an offset and count alongside the trusted
read-only storage dependency and provides checked indexing, `front()`,
`back()`, `first()`, `last()`, `subspan()`, and structural iteration. Empty
spans remain valid. Copy and move construction preserve the same shared owner
dependency; assignment is unavailable because the current lifetime model does
not reseat borrowed-state carriers.

`std::string::view()` returns a counted `std::string_view` over the string's
live character prefix. Literal-backed string views remain independent trivial
values. A view produced from dynamic storage is instead tagged at the
expression and binding level with one read-only owner dependency. That
dependency passes through local copies, direct receiver factories, exact
single-parameter relay functions, moves, returns, and drops. It ends at the
proven last use or enclosing scope end of the final retained local view, after
which the owner may be mutated, moved, or dropped. While it is live, every
overlapping owner mutation or movement is rejected. Empty dynamic views follow
the same rules and carry a zero length without requiring an element address.

The bounded release deliberately excludes mutable views, multiple owners,
nested borrowed-state storage, reassignment, and field/global/static storage.
A dynamic text view cannot be formed from temporary storage or escape a local
owner. A `std::string_view` parameter is conservatively treated as potentially
dynamic inside the callee; it may be inspected or returned through one exact
read-only parameter summary, but it cannot be retained in another storage
object. These restrictions keep source validity in semantics and preserve the
same owner and transfer facts in HIR and verified MIR. The C++ backend only
represents the already-verified counted view and does not infer its lifetime.

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
does not permit reordering. The implemented predicate and reduction algorithms
retain each callable by value in one confined mutable local. They may invoke it
zero or more times, so both read-callable and mut-callable targets are valid,
while a consuming once-callable is not. Numeric operation state can affect the
current reduction but is not returned to the caller, and the element,
intermediate, and accumulator types remain the same exact `T`. This establishes
a bounded iterator/sentinel algorithm surface, not a complete range concept,
heterogeneous accumulation, iterator category hierarchy, or C++20 ranges
model.

`<std/algorithm>` additionally defines the ordinary iterator/sentinel
`std::find`, `std::count`, and `std::for_each` cluster. All three accept an
`Iterator` and a potentially distinct `Sentinel` under exact
`input_iterator`/`sentinel_for` requirements. `find` and `count` also require
exact `dereferences_to<Iterator, T>` and `equality_comparable<T>` facts. They do
not use implicit conversions, C++ traits, SFINAE, subsumption, partial
specialization, or public-name recognition.

`find` returns the final iterator by moving the exact `first` parameter. For a
borrow-carrying concrete iterator, that declaration-linked move selects
`first`, rather than an unrelated sentinel or other read-only carrier, as the
return owner. Mixed return paths rooted in different parameters remain
ambiguous and are rejected. `count` returns an independent `ptrdiff_t`.

`for_each` invokes its by-value callable zero or more times with the exact
checked-reference type produced by dereference, then returns that same callable
through one final `std::move`. Its invocation contract remains confined and
repeatable; exact-return transport is a separate ownership fact. A mutable
generic callable parameter can select either read-only or mutable
`operator()` for the exact `void(T&)` signature, but not a once-callable or a
value-returning/mismatched overload. A named callable must be passed with
`std::move`, must be movable, and cannot store references, tracked borrowed
state, or raw pointers. The returned value preserves the exact concrete
callable type without erasure.

## Current boundary

This layer establishes syntax, protocol resolution, explicit HIR provenance,
MIR control flow, and replaceable-backend call targets. It is enough for
self-contained iterators and sentinels, including generic virtual operator
contracts.

Source-defined container iterators may retain one read-only or mutable owner
reference through GTI's confined stored-reference class contract. Constructor
and method results carry the owner dependency through semantics, HIR, and MIR.
A free function or static factory may now return a cursor or view derived from
one eligible read-only parameter. The single dependency survives ordinary calls,
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
The initial source-defined `std::vector<T>` uses the same iterator path. Its
read-only iterator carries an index plus a read-only storage borrow. Its
`begin_mut()` endpoint returns a separate mutable iterator carrying an index
plus the exclusive storage borrow, and mutable dereference uses checked
storage access. Neither iterator exposes an address or unchecked cursor. An
active mutable range therefore blocks reserve, push, clear, movement, and
other overlapping owner operations under the retained-loan rules.

Each range statement additionally has a compiler-owned semantic/HIR/MIR plan
with exact owner, range, iterator, sentinel, element, operation, access, loan,
parent/child, loop-edge, and cleanup identities. Multiple non-overlapping
loops retain separate plans. Fixed arrays use the indexed plan directly and
need no public `begin()` or `end()`. The MIR verifier rejects an access-mode
widening, a mutable child detached from its exclusive parent, an escaping
element loan, an inconsistent array strategy, a mismatched owner/drop, or a
broken condition/body/increment/cleanup cycle before backend admission.

Multiple-origin or nested stored views, pointer decay, and mutable spans or
text views remain later lifetime layers. Mutable iteration currently uses
conservative whole-owner invalidation exclusion rather than a public
container-specific invalidation trait. Confined generic `void` operations,
exact `bool`
predicates, exact context-supplied non-reference value results without tracked
borrowed state or lambda identity, and proven forwarding through other
confined callable parameters are available for algorithm callbacks. Result
inference through `auto` and borrowed callable results remain unavailable. One
bounded exact-return transport may be combined with repeated direct invocation
when every reachable return moves that same callable parameter; arbitrary
storage, wrappers, and erased callable escape remain unavailable. The bounded
input-iterator, sentinel, exact `dereferences_to`, lifecycle
`copy_constructible`, and source-composed `forward_iterator` capabilities are
implemented. `std::string(first, last)` uses the input family in an ordinary
constructor-local generic declaration and copies the selected character range
without retaining either iterator. Read-only string and vector cursors may be
copy-constructed: each copy has an independent position while retaining the
same shared owner loan. They remain non-assignable, and the compiler-confined
mutable vector iterator remains non-copyable.

`std::binary_search(first, last, value)` is the first forward-iterator client.
It makes one counting pass, then copies and advances cursors to select
midpoints. A possibly distinct `last` remains an immutable sentinel; no
iterator subtraction or numeric iterator operation is required. The algorithm
performs logarithmic comparisons and linear iterator increments, and requires
the input to already be sorted according to `operator<`. Complete-range,
readable/writable element, sized-range, random-access, and public invalidation
capabilities are still missing. The implemented iterator/sentinel
`std::for_each` does not imply a range-first overload; a public range-first form
over arbitrary structural ranges should wait for those capabilities instead of
teaching the compiler a public container or algorithm name.
