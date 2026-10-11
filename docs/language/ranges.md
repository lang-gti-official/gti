# Ranges and iterators

## Mutable contiguous views

`<std/span>` provides `std::mutable_span<T>`, an exclusive, move-only view of
initialized elements. `vector<T>::as_mutable_span() mut` and
`string::as_mutable_span() mut` create checked views of their current size, not
their spare capacity. This slice uses the source-defined prefix-owner
representation; construction from fixed arrays, arbitrary pointers and native
memory is not supplied.

The familiar operations are `size`, `empty`, checked `at`/`operator[]`,
`front`, `back`, `first(count)`, `last(count)` and `subspan(offset, count)`.
Empty end subspans are valid; element access, front/back on empty views, and
invalid subranges raise defined bounds failure. Subviews retain the original
storage and their parent loan; they do not allocate or copy elements.

A mutable receiver returns mutable element references and exclusive subviews.
Read-only access returns read-only elements. `as_span()` creates a checked
shared view without transferring ownership. While a shared child or any copy
of it remains live, the exclusive parent cannot be used for mutation or moved.
When children end, parent access resumes. Owner mutation/reallocation,
movement and destruction cannot invalidate a live view.

`begin_mut()` supports mutable range-for; `begin()`/`end()` support read-only
iteration. A retained iterator protects both the physical carrier and its
stored owner dependency. No public type name grants borrowing permission;
ordinary user-defined single-owner carriers follow the same rules.

See [composable I/O](io.md#composable-buffered-io) for `std::byte_buffer` and
byte-slice transports.

## Range protocol

Status: current through the bounded L-RANGE-03 single-owner view release,
M-OWN-04 named Expected-payload range loans, and the L-RANGE-04
iterator/sentinel plus complete-range algorithm cluster

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

- `operator!=(Sentinel&)` or `operator!=(Sentinel)`, with one exact read-only
  reference parameter or copyable by-value parameter accepting the sentinel;
- `operator*()`, returning a checked reference;
- `void operator++() mut`, the prefix increment operation.

These are ordinary GTI methods. Access control, generic substitution, receiver
mutability, exact overload matching, virtual dispatch, result ownership, and
borrow checks all run through the normal frontend. C++ argument-dependent
lookup and native range-for semantics are never consulted.

The comparison must be unambiguous for a named sentinel. Public library end
sentinels containing only a position or an empty end marker are passed by
value, so explicit loops may compare `iterator != owner.end()` directly.
Reference-taking application sentinels remain supported, but still require a
stable named argument; this is not an exception to temporary-reference
binding. The structural requirement promises the named-argument operation,
not that every conforming sentinel accepts temporary arguments.

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

## Named Expected-payload ranges

A read-only range expression may be the exact `value()` extraction from an
available named local or by-value parameter of type `expected<T, E>`:

```gti
int count(expected<std::string, int32_t> result) {
  mut int32_t total = 0;
  for (auto& byte : result.value()) {
    total += 1;
  }
  return total;
}
```

`T` must independently satisfy the ordinary read-only structural range
contract. This is not recognition of `expected`, `string`, or another public
library spelling: the frontend admits only the exact compiler-trusted checked
value extraction and records it in the existing range plan. The extraction's
wrong-state failure occurs before the loop starts. On success, the range loan
is rooted in and conservatively protects the complete named `expected`.
Nested read-only traversals of the same payload are permitted; `continue`,
`break`, `return`, normal exhaustion, and defined failure end element loans
before their parent range loans.

The receiver cannot be a field, global/static, returned reference, or opaque
call result for this retained form. The loop cannot request mutable elements,
iterate `error()`, move or replace the `expected`, or let an element reference
escape. A fresh rvalue such as `make_result().value()` may still be transferred
into the ordinary owned range-temporary path when its payload is itself a
valid range; that case does not create a retained named-owner projection.

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

See [`examples/065-mutable-ranges.gti`](../../examples/065-mutable-ranges.gti)
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

## Complete-range concepts

`<std/iterator>` exposes bounded structural concepts for generic code that
wants the complete owner rather than an iterator/sentinel pair:

- `std::input_range<Range>` proves the exact public read-only `begin()` and
  `end()` protocol plus input iteration and sentinel comparison;
- `std::range_of<Range, T>` additionally proves that read-only dereference
  returns exactly `T&`;
- `std::mutable_range_of<Range, T>` proves the exact mutable `begin_mut()`
  endpoint and `mut T&` dereference; and
- `std::sized_range<Range>` proves one exact public read-only `size()` returning
  `std::size_t`.

Fixed arrays satisfy these concepts through their checked indexed range plan;
they do not acquire members or decay to pointers. A nominal range satisfies
them only through one exact accessible non-static overload for each required
member and iterator operation. Return conversions, native lookup, ambiguous
overloads, private/compiler-private members, and generic endpoint methods do
not satisfy the first bounded contract.

The constrained generic body may iterate `Range&`, `mut Range&`, or call the
proven zero-argument `size()` member directly. Symbolic analysis uses private
iterator and sentinel carriers only to prove that body. Concrete instance
analysis replaces them with the selected source declarations or fixed-array
strategy before HIR; HIR/MIR never carry an unresolved public associated type
or delegate range selection to C++.

The fixed-array case applies to a written reference parameter whose bare
generic type is substituted with the array, such as `Range&` or `mut Range&`.
It does not enable explicitly spelled or aliased array-reference parameters,
array-reference storage or returns, or array-reference callback signatures.

`<std/functional>` similarly provides `std::range_predicate<Predicate, Range>`
and `std::range_operation<Operation, Range, Result>`. They derive the exact
read-only element from the complete range and require one repeatable exact
callable. `<std/algorithm>` uses these facts for generic whole-range `all_of`,
`any_of`, `none_of`, `count_if`, and `for_each`; generic `fill` uses
`mutable_range_of`. The same source implementations accept application-defined
nominal ranges, standard-library ranges, and fixed arrays. No algorithm or
public range name is compiler-known. See
[`examples/083-complete-ranges.gti`](../../examples/083-complete-ranges.gti).

The bounded concepts intentionally expose no iterator associated-type syntax,
direct symbolic `begin()`/`begin_mut()`/`end()` extraction, range adaptor,
borrowed-range trait, implicit conversion, or invalidation customization.
Generic code should use range-based `for`, or accept already-formed endpoints
under the iterator concepts. Iterator-pair APIs remain available when a client
already owns non-overlapping endpoints; forming `begin_mut()` and `end()`
independently from one owner is still rejected because their loans overlap.

## Single-owner spans and text views

`std::span<T>` is an ordinary source-defined, read-only range over one checked
source prefix owner. `std::vector<T>::as_span()` and
`std::string::as_span()` construct a complete span without exposing an address
or pointer-length pair. A span stores an offset and count alongside its checked
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
  `bool I::operator!=(S&)` or `bool I::operator!=(S)` with copyable `S`;
- `std::output_iterator<I>` proves mut checked dereference returning a mutable
  reference plus mutable prefix increment, and `std::writes_to<I, T>` refines
  the mutable referent to exactly `T`;
- `std::bidirectional_iterator<I>` composes `forward_iterator` with public
  `void operator--() mut`, and `std::random_access_iterator<I>` further adds
  `void operator+=(std::ptrdiff_t) mut` and
  `void operator-=(std::ptrdiff_t) mut`;
- `std::subscripts_to_mut<I, T>` proves
  `mut T& I::operator[](std::ptrdiff_t) mut` for exactly `T`, the positioned
  write contract that keeps one exclusive cursor over a whole range;
- `std::sized_sentinel_for<S, I>` composes `sentinel_for` with read-only
  `std::ptrdiff_t I::operator-(S&)` or `std::ptrdiff_t I::operator-(S)` with
  copyable `S`, the signed distance from the iterator to the sentinel.

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

`std::copy(first, last, output)` is the first writable-output client: the
source side uses the input/sentinel contract and the destination uses
`std::output_iterator`. Every destination write remains checked at the access
itself, and the advanced destination cursor is not returned because a mutable
owner-tied cursor cannot escape through a return value. The read-only vector
cursor additionally implements the bidirectional, random-access, and
sized-sentinel operator sets, so `std::prev`, signed `std::next`, in-place
`+=`/`-=` movement, and `cursor - sentinel` distances are ordinary generic
code over it.

`std::fill` accepts any exact `mutable_range_of<Range, T>`.
`std::transform`, `std::reverse`, and `std::sort` still take the owning
`std::vector<T>` directly. These whole-owner forms exist because the
iterator-pair spelling would either need a read borrow of the owner while its
exclusive mutable cursor is live (`values.begin_mut()` and `values.end()` in
one call) or retainable mutable cursors in ordinary bodies. Each form keeps one
exclusive borrow for the whole rewrite and names the element type for locals.
`reverse` and both sort forms use checked indexed swaps and require
`std::relocatable`, not copyability or move assignment. They borrow elements
read-only for comparisons and never clone them or move out an exposed element.
`transform`
instead borrows each source element, obtains an owned same-type result, and
moves that result into the existing destination element. It requires
`std::movable`, supports move-only resources and strings, and never moves from
the source. Overlapping whole-owner access is rejected; an undersized
destination fails at the checked write after evaluating that element's mapper.

Both `std::sort(values)` (using `operator<`) and `std::sort(values, compare)`
use iterative heapsort with O(n log n) comparisons and constant auxiliary
storage. The latter retains one owned repeatable comparator throughout. The comparator
must supply a strict weak order. Neither overload promises stability; equal
elements may be reordered. This does not change the stable node-container
sort contracts. Source comparisons, checked element accesses and cleanup keep
their ordinary deterministic evaluation/failure behavior; a failed comparison
does not roll back preceding permutations.

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

Mutable spans and their shared/subspan projections use the retained-loan rules
described above. Pointer decay and independently splittable disjoint mutable
views remain outside that surface. Mutable iteration currently uses
conservative whole-owner invalidation exclusion rather than a public
container-specific invalidation trait. Confined generic `void` operations,
Exact `bool` predicates, exact context-supplied non-reference value results
without tracked borrowed state or lambda identity, and proven forwarding
through other confined callable parameters are available for algorithm
callbacks. Direct composition of open callables such as `outer(inner(value))`
also resolves the intermediate value from the selected concrete callable;
it need not have the outer function's result type. `<std/functional>` exposes
those same read/mut/once facts as public
unary, binary, and iterator-aware predicate/operation relations, so invalid
targets fail before concrete body reanalysis. Result
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
the input to already be sorted according to `operator<`. Whole-range `all_of`,
`any_of`, `none_of`, `count_if`, `for_each`, and `fill` use the structural
complete-range concepts above. `transform`, `reverse`, and comparator `sort`
remain bounded to `std::vector` while their additional output, movement, and
random-access complete-range relationships are not yet public. Public
invalidation capabilities also remain absent; ownership conservatively
protects the whole owner during iteration.

### Searching and rewriting containers

`<std/algorithm>` also supplies whole-range `contains`, `count`,
`is_partitioned`, `replace`, and `replace_if`. They work with structural
user-defined ranges as well as fixed arrays and standard containers. A value
needle is an exact readable `T&`; bind scalar needles to a local first rather
than relying on C++ temporary-to-reference conversions. Associative containers
keep their key-based member `find` and `contains`; generic algorithms perform
linear sequence traversal and do not substitute for tree/hash lookup.

Iterator/sentinel algorithms include `contains`, `is_partitioned`,
`partition_point`, `adjacent_find`, `search`, `search_n`, `find_first_of`, `lower_bound`,
`upper_bound`, `min_element`, `max_element`, `is_sorted`, and
`is_sorted_until`. `equal`, `starts_with`, and `lexicographical_compare` take
four endpoints, checking both sequence boundaries. Empty patterns match at
the start; missing searches return the exhausted first cursor; empty extrema
return that same cursor without dereferencing it. Extrema keep the first
equivalent element. Bounds require an already sorted sequence, use logarithmic
comparisons and linear cursor increments. Subsequence search is the ordinary
quadratic worst-case algorithm, not an allocation-backed optimized search.

`partition_point(first, last, predicate)` returns the first false element in a
range partitioned into a true prefix and a false suffix, or the exhausted
cursor when every element satisfies the predicate. It requires a forward
iterator, an exact sentinel and a repeatable exact-`bool` indirect predicate.
It makes no predicate calls for an empty range. Input need not be sorted, but
must be partitioned for that predicate.

`lower_bound`, `upper_bound` and `binary_search` also accept a final comparator:
`std::lower_bound(first, last, key, compare)`. The comparator receives two
read-only references to the exact element/key type and returns exactly `bool`;
it supplies ordering without requiring the element's `operator<`, equality,
copy construction or assignment. Lower bound partitions on
`compare(element, key)`; upper bound partitions on `!compare(key, element)`.
Binary search requires a sequence sorted under the comparator's strict weak
order and tests equivalence under that order, not element equality. Thus an
application may search records by one field or use descending order. This
slice does not supply heterogeneous key types or projections.

These queries retain one owned predicate/comparator, accepting read-callable
or mutable-callable targets but not consuming once-callables. A named move-only
target must be explicitly moved; it is not copied or recursively re-created.
The implementation uses O(log n) predicate/comparator calls (binary search
adds one final equivalence check), O(n) forward increments and constant
auxiliary cursor storage, with no query-owned allocations or element copies.
It currently uses that forward strategy even for random-access cursors.
Bounds and partition point advance and return the original first cursor,
preserving its owner dependency. Defined failure in a callback or iterator
operation performs ordinary deterministic cleanup, including the retained
callback. The sorted/partitioned preconditions are not dynamically validated;
all cursor accesses still obey their checked protocol.

See [`examples/138-ordered-search.gti`](../../examples/138-ordered-search.gti)
for sorting and searching application records with the same custom order.

`copy_if`, `copy_n`, `replace_copy`, `replace_copy_if`, `remove_copy`, and
`remove_copy_if` write into pre-existing output elements and return `void`,
like the existing `copy`. They neither resize the destination nor move from
the source. Writes require valid copy assignment; move-only elements must be
handled through an explicit ownership-aware operation. Checked destination
access reports exhaustion, and `copy_n` also checks source access. Predicates
are confined, repeatable, exact-`bool` callables. Comparison-only algorithms
check the concrete element operators during instantiation; input-iterator
capability alone does not promise that arbitrary element pairs compare.

Read-only `list` iterators support bidirectional traversal; `deque` and
`array` iterators additionally support signed jumps and sentinel distances.
Set key iterators support forward algorithms. `array`, `list`, and `deque`
provide `begin_mut()` for mutable range-for and confined output algorithms.
The cursors borrow their owner and expose no raw pointer or array decay.
Retaining a search result keeps that owner borrowed: mutation, movement, and
destruction cannot invalidate it. Array/list/deque mutable cursors can also be
bound locally under the existing nominal stored-reference rules, retaining an
exclusive owner borrow; they cannot escape a local owner. Vector's private
storage cursor retains its narrower confined contract. This slice adds neither
iterator reseating nor paired mutable subranges.
Iterator-returning search still uses explicit endpoints, not a
whole-range overload with an inferred associated iterator return type.
