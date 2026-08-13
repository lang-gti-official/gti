# 7. Standard Library

Status: Partial normative draft

## 7.1 Library Boundary

Public portable library declarations reside under `std`. They are ordinary GTI
source unless this specification explicitly identifies a compiler-bound
operation. Public wrapper names shall not become backend shortcuts merely for
convenience.

Compiler-private declarations reside under `gti_internal`. They may enforce an
irreducible invariant unavailable to ordinary GTI, but they are not stable
application APIs and shall not expose public container or ownership policy.

The source graph distinguishes application, implicit-prelude, and installed
standard-library units. Only the latter two roles are compiler-trusted, and
standard-library trust requires a physical source unit beneath a configured
standard-library root. Application source shall not declare, directly name, or
create a namespace alias to the root `gti_internal` namespace; semantic
diagnostic `GTI-S2058` rejects each such attempt.

Compiler-capability types and operations bind by the selected trusted prelude
declaration identity, never by qualified spelling or a public wrapper name.
Aliases preserve that identity. Declarations whose exposed semantic type
contains a private capability are not published to application source, so a
trusted unit cannot leak a private handle through an otherwise public
constructor, function, field, or alias. Completion, hover, definition, and
semantic classification apply the same compiler-owned privacy facts. Public
`std` wrappers remain ordinary GTI source.

Host services are reached through ordinary GTI wrappers over bounded
C-linkage declarations. Fixed-width scalars cross directly; text inputs use the
explicit non-retained `gti_c_string_view` counted record. The runtime ABI is not
the ABI of public GTI classes. The legacy compiler-owned `@runtime` allowlist is
a compatibility mechanism rather than a requirement for new host entries.

## 7.2 Prelude

The prelude is implicitly visible to every ordinary source unit. Its current
public surface includes:

| Facility | Contract |
| --- | --- |
| `std::size_t` | Transparent alias of `uint64_t` |
| `std::ptrdiff_t` | Transparent alias of `int64_t` |
| `std::string_view` | Counted read-only view over static literal storage |
| `std::print` and `std::println` | Portable output operations over string views |
| `std::unique_ptr<T>` | Nominal move-only unique owner |
| `std::make_unique<T>` | Ordinary generic unique-owner factory |
| `std::transferable<T>` | Compiler-backed transfer-capability concept |
| `std::shareable<T>` | Compiler-backed read-only sharing concept |

`expected<T, E>` and `unexpected(error)` currently have language-level syntax
and semantics rather than being ordinary prelude templates.

## 7.3 Optional Standard Units

Optional units are loaded using `<std/name>` and are independently parsed GTI
source. An optional unit does not automatically re-export its own dependencies.

The current implemented foundation includes:

- `std::array<T, N>` over checked fixed-array storage, including front/back
  access and copyable-element fill; and
- `std::string` as a move-only owner over private character storage, including
  front/back and mutable checked access, resize, shrink-to-fit, and pop-back;
  and
- `std::vector<T>` as a move-only dynamic owner with checked indexed access,
  reserve, resize, shrink-to-fit, clear, push/pop, read-only traversal,
  in-place emplacement, and explicit copyable-element cloning; and
- `std::forward_list<T>` as a move-only recursive unique owner with empty/front,
  clear, and constant-time front push/pop operations; and
- `<std/iterator>` public `std::input_iterator<I>` and
  `std::sentinel_for<S, I>` concepts over exact private structural
  capabilities, plus forward-only `advance`, `distance`, and `next`; and
- `<std/algorithm>` value-returning `min`, `max`, and `clamp`, and structural
  input-iterator `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and
  `count_if`; and
- `<std/cmath>` binary32 `abs`, `isfinite`, `isinf`, and `isnan` implemented
  from specified GTI floating-point operations; and
- `<std/numeric>` public `std::accumulates_into<I, T>` and an ordinary
  source-defined default and operation-based `std::accumulate`, homogeneous
  default and operation-based `std::inner_product`, and unary
  `std::transform_reduce` over transferable input-iterator values, exact
  dereference referents, numeric accumulators, and confined exact-result
  callables. The current reduction algorithms execute deterministically from
  left to right rather than inheriting C++'s permission to reorder; their
  callable access is read-only and their element/intermediate/result type is
  the same exact numeric `T`,
  plus exact fixed-width `wrapping_add/sub/mul` and
  `saturating_add/sub/mul` functions, and failure-free
  `checked_add/sub/mul` functions returning
  `expected<T, std::arithmetic_errc>`; and
- `<std/utility>` source-defined `std::pair` and `std::make_pair`; and
- `<std/cstdio>` unbuffered stdin and read-only file byte input through
  `std::getchar`, `std::fopen`, `std::fgetc`, `std::fclose`, and a move-only
  `std::FILE` owner; and
- `<std/tcp>` POSIX IPv4 stream-socket creation and close through a move-only
  `std::tcp::socket` owner with typed creation and close errors.

Detailed API contracts remain in source and `stdlib/README.md` until migrated
into per-component specification sections.

Some implemented units also carry explicitly documented bodyless declarations
for a reviewed next API slice. Such a declaration allows frontend checking but
is not an implemented library operation: using it in a native build fails to
link until its GTI body is supplied. Array/vector/string swap, forward-list
iteration and middle-node algorithms, and the list and span scaffolds remain in
that category.

The owned program-entry form names the canonical installed
`std::vector<std::string>` specialization. This exact type identity is part of
the language's `main` contract, not a general intrinsic or a promise that
public GTI class layout crosses an ABI. The implementation constructs ordinary
source-defined string and vector values before calling user code; importing
both optional units is therefore required. See
[`programs-and-targets.md`](programs-and-targets.md).

## 7.4 Allocation And Containers

Public ownership and container classes retain logical state and policy in
ordinary GTI fields. Compiler-private capabilities may supply unique ownership
or partially initialized storage without exposing raw addresses, manual
deallocation, logical size, capacity policy, or slot engagement as public
language state.

The initial `std::vector<T>` requires movable elements and rejects element
types that retain borrowed state. It is move-only even when `T` is copyable.
`vector(n)` constructs `n` value-initialized elements, matching the familiar
C++ size-constructor meaning; it is not a reserve-only constructor. `at` and
`operator[]` are both checked in this initial safe surface.

`emplace_back<Args...>(Args... args)` constructs `T` directly in the selected
storage slot and returns a receiver-tied mutable borrow of that element. Its
arguments follow GTI's existing immutable by-value pack rules: copyable pack
elements may be copied and noncopyable movable elements are consumed once.
This is in-place construction, but it is not C++ perfect forwarding and does
not preserve every source value category.

The current vector iterator is read-only and retains one checked borrow of its
owner. Mutable iteration, multiple retained cursors, complete invalidation
semantics, temporary-range traversal, and general owner-dependent views remain
outside this slice.

`std::forward_list<T>` implements its front operations as ordinary GTI over a
recursive `std::unique_ptr` node chain. Its declaration-only iterator uses the
same exact read-only input-iterator/sentinel shape and is explicitly move-only.
Returning a nullable owner-tied cursor, mutating through traversal, and proving
invalidation remain unavailable, so iteration, reverse, resize, merge, remove,
unique, and sort are still bodyless. The separate list scaffold has no node
representation because a compliant constant-time back operation requires a
safe non-owning link that GTI cannot yet retain. The declaration-only span is
likewise move-only and read-only: construction from an owner, mutable access,
iteration, and raw address exposure remain absent until the frontend can retain
the source owner and invalidation state directly.

The standard library currently has no stable public raw allocator, shared
owner, weak owner, dynamic borrowed view, or general unsafe memory API.

## 7.5 Errors And Failure

Library operations that model environmental or resource conditions callers can
reasonably handle return `expected`. Operations documented as infallible at the
type level produce a defined runtime failure for violated checked
preconditions or allocation failure.

An infallible convenience wrapper over a fallible host service may use
`GTI-R0012` when its host operation fails, but it shall not discard that
failure. It should have a recoverable sibling when callers reasonably need to
handle the condition. The current `std::print`/`std::println` implementation
discards the status from `gti_rt_write_stdout`; this is an implementation gap
owned by the hosted-service work, not a permitted hidden stream state.

Wrong-state `expected.value()` and `expected.error()` access is the checked
`GTI-R0009` failure. Infallible allocation uses `GTI-R0011`; recoverable
`try_make_*` factories return `expected` without invoking the failure observer.
Best-effort resource cleanup, such as destruction-time close, may discard a
host error only when the component contract explicitly says so and an explicit
recoverable close operation exists.

Public vector/string indexing reports `GTI-R0007` against logical size. A
private-storage `GTI-R0010` is an internal invariant failure and must not leak
as the public bounds category. The current wrappers delegate directly to
capacity-sized private storage, so an index between logical size and capacity
can still reach `invalid_storage_state`; the container/failure implementation
must add the public logical check before claiming execution-contract
conformance.

Each component specification must state whether failure is recoverable,
terminating, or impossible under its preconditions. Hidden native exception or
stream state is not a GTI error model. The common category, cleanup, embedding,
and report rules are in [Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

## 7.6 Documentation And Conformance

Public library declarations should retain documentation comments from GTI
source so the same contract can serve API documentation, hover, completion,
signature help, and generated reference material.

Before 1.0, every standardized component requires tests covering primitive,
copyable aggregate, move-only, polymorphic, empty, error, and boundary cases as
applicable.

## 7.7 Library Specification Gaps

The following component families remain planned or incomplete:

- shared and weak ownership;
- optional and general sum types;
- owner-tied spans and dynamic string views;
- complete vector insertion/erasure, copy, allocator, and iterator support;
- the remaining callable, complete-range, heterogeneous accumulation, and hash
  generic-capability families;
- formatting, buffered streams, file writes, seeking, and structured I/O;
- general filesystem operations, time, randomness, connected networking,
  traffic buffers, and threading;
- recoverable allocation factories;
- allocator-aware containers and safe pools/arenas; and
- broader native interoperability beyond the current call-only fixed-scalar
  and counted-text-input surface.

Their absence is not permission to substitute similarly named C++ facilities
without a GTI contract.
