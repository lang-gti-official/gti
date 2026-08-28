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

Compiler-capability types and operations bind by an exact selected declaration
identity, never by qualified spelling or a public wrapper name. Most such
identities live in the trusted prelude; managed-thread and atomic identities
live only in the exact trusted `<std/thread>` and `<std/atomic>` source units so
unrelated programs do not carry their private declaration inventories. Aliases
preserve an identity.
Declarations whose exposed semantic type contains a private capability are not
published to application source, so a trusted unit cannot leak a private
handle through an otherwise public constructor, function, field, or alias.
Completion, hover, definition, and semantic classification apply the same
compiler-owned privacy facts. Public `std` wrappers remain ordinary GTI source.

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
| `std::print` and `std::println` | Portable output over string views, `char`, and all fixed-width integers |
| `std::unique_ptr<T>` | Nominal move-only unique owner with explicit null construction, reset, null assignment, and checked explicit base upcast |
| `std::make_unique<T>` | Ordinary generic unique-owner factory |
| `std::upcast_unique<Base, Derived>` | Explicit consuming conversion to a public polymorphic base owner |
| `std::transferable<T>` | Compiler-backed transfer-capability concept |
| `std::shareable<T>` | Compiler-backed read-only sharing concept |
| `std::copy_constructible<T>` | Copy construction is available; copy assignment is not implied |
| `std::copyable<T>` | Copy construction and copy assignment are both available |

`expected<T, E>` and `unexpected(error)` currently have language-level syntax
and semantics rather than being ordinary prelude templates.

## 7.3 Optional Standard Units

Optional units are loaded using `<std/name>` and are independently parsed GTI
source. An optional unit does not automatically re-export its own dependencies.

The current implemented foundation includes:

- `std::array<T, N>` over checked fixed-array storage, including front/back
  access and copyable-element fill; and
- `std::string` as a move-only owner over private character storage, including
  construction from an exact input iterator/sentinel pair whose dereference
  referent is `char`, front/back and mutable checked access, resize,
  shrink-to-fit, pop-back, and
  source-defined base-10 `std::to_string` conversion for every fixed-width
  integer, plus `print`/`println` traversal through either a read-only borrow
  of a named owner or direct ownership transfer from a fresh string result;
  and
- `std::vector<T>` as a move-only dynamic owner with checked indexed access,
  reserve, resize, shrink-to-fit, clear, push/pop, indexed insertion/erasure,
  read-only traversal, in-place emplacement, explicit copyable-element
  cloning, and construction from one contextual fixed-array value such as
  `std::vector<int>({1, 2, 3})`; this constructor copies elements from its
  owned array parameter and therefore requires copyable `T` when used; and
- `std::forward_list<T>` as a move-only recursive unique owner with empty/front,
  clear, and constant-time front push/pop operations; and
- `<std/iterator>` public `std::input_iterator<I>`,
  `std::sentinel_for<S, I>`, `std::dereferences_to<I, T>`, and
  `std::forward_iterator<I>` concepts over exact private structural and
  lifecycle capabilities, plus forward-only `advance`, `distance`, and
  `next`; and
- `<std/algorithm>` value-returning `min`, `max`, and `clamp`, and structural
  input-iterator `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and
  `count_if`, exact-value `find` and `count`, and returned-callable `for_each`,
  plus `binary_search` over an exact ordered forward-iterator subrange. These
  algorithms accept a distinct exact sentinel type. `find` returns its moved
  iterator while retaining the concrete borrowed-owner dependency, and
  `for_each` returns the exact moved callable after repeated confined
  invocation. Predicate and operation clients accept read-callable or
  mut-callable targets but not consuming once-callables; and
- `<std/cmath>` binary32 `abs`, `isfinite`, `isinf`, and `isnan` implemented
  from specified GTI floating-point operations; and
- `<std/functional>` source-defined `less<T>`, `greater<T>`, `equal_to<T>`, and
  `not_equal_to<T>` function objects over the corresponding exact comparison
  concepts; `hash<T>` remains a customization scaffold; and
- `<std/numeric>` public `std::accumulates_into<I, T>` and an ordinary
  source-defined default and operation-based `std::accumulate`, homogeneous
  default and operation-based `std::inner_product`, and unary
  `std::transform_reduce` over transferable input-iterator values, exact
  dereference referents, numeric accumulators, and confined exact-result
  callables. The current reduction algorithms execute deterministically from
  left to right rather than inheriting C++'s permission to reorder. Each
  operation is retained by value in one confined mutable local, so read-callable
  and mut-callable targets are accepted, consuming once-callables are rejected,
  and the element/intermediate/result type is the same exact numeric `T`. The
  unit also supplies exact fixed-width `wrapping_add/sub/mul` and
  `saturating_add/sub/mul` functions, and failure-free
  `checked_add/sub/mul` functions returning
  `expected<T, std::arithmetic_errc>`, plus source-defined integral `gcd` and
  `lcm` and numeric `midpoint`; and
- `<std/utility>` source-defined `std::pair` and `std::make_pair`; and
- `<std/cstdio>` unbuffered stdin and read-only file byte input through
  `std::getchar`, `std::fopen`, `std::fgetc`, `std::fclose`, and a move-only
  `std::FILE` owner; and
- `<std/format>` sequential integral replacement through source-defined
  `std::format`, `std::try_print`, and `std::try_println`. The grammar is
  limited to `{}`, `{{`, and `}}`; pattern and argument-count errors return
  `expected<..., std::format_errc>` before output begins; and
- `<std/random>` source-defined deterministic `std::random_engine` sequences
  from an explicit `uint64_t` seed, plus fallible `std::random_device` draws
  backed by bounded host entropy; and
- `<std/thread>` source-defined `std::thread`, a move-only managed worker
  owning one mandatory join obligation. `std::thread::start` consumes one
  exact transfer-safe `void()` callable and returns
  `expected<std::thread, std::thread_errc>`; `join()` returns recoverable
  native join errors, destruction autojoins, and either join route re-raises an
  exact worker defined failure after worker cleanup. Move construction
  transfers the join obligation; move assignment is unavailable.
  `std::thread_result<Result>` accepts an exact transfer-safe `Result()` task,
  returns the owned value from explicit `join()`, and joins then abandons an
  unobserved value during destruction. This first surface has no detach, stop
  token, separate argument pack, or cross-thread borrow; and
- `<std/atomic>` source-defined move-only `std::atomic<T>` for `bool` and every
  fixed-width signed and unsigned integer. Construction, load, store, exchange,
  and strong compare/exchange default to sequential consistency. Static
  `std::memory_order_relaxed()`, `acquire()`, `release()`, `acq_rel()`, and
  `seq_cst()` tag types expose only operation-legal overloads. Strong
  compare/exchange never fails spuriously and returns success plus the actual
  observation without exposing storage. The wrapper is transfer- and
  share-capable but promises neither lock freedom, stable layout, nor native
  ABI compatibility; and
- `<std/tcp>` POSIX IPv4 stream-socket creation and close through a move-only
  `std::tcp::socket` owner with typed creation and close errors; and
- `<std/filesystem>` a move-only `std::filesystem::path` giving C++'s
  decomposition (`root_name`, `root_directory`, `root_path`, `relative_path`,
  `parent_path`, `filename`, `stem`, `extension`), the matching `has_*`
  queries, `is_absolute`/`is_relative`, the `clear`, `make_preferred`,
  `remove_filename`, `replace_filename`, `replace_extension`, and `swap`
  modifiers, `assign`/`append`/`concat`, directory joining through `/` and
  `/=`, element-wise `compare` with the ordering and equality operators, and
  `lexically_normal`. The unit performs no filesystem access: it is pure
  pathname manipulation. Three departures from C++ are deliberate — a
  pathname is stored exactly as supplied rather than normalized at
  construction, `operator+=` has not yet been added to this source-defined
  class, and `begin`/`end` are absent because a C++ path iterator walks elements
  rather than characters and GTI's element iterator awaits the generic
  iterator work. Copying is explicit through `clone()`. The converting
  constructor `path(std::string_view)` is implemented through the ordinary
  verified class-construction route.

Detailed API contracts remain in source and `stdlib/README.md` until migrated
into per-component specification sections.

Some implemented units also carry explicitly documented bodyless declarations
for a reviewed next API slice. Such a declaration allows frontend checking but
is not an implemented library operation: using it in a native build fails to
link until its GTI body is supplied. Array/vector/string swap, forward-list
iteration and middle-node algorithms, and the list scaffold remain in that
category.

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
deallocation, logical size, or capacity policy as public language state.

`<std/memory>` implements the first public typed allocation slice:

- `std::allocator` is a stateless process allocator with
  `try_storage<T>(count)` and
  `try_storage<T>(count, std::alignment minimum)`;
- `std::storage<T>` is a move-only owner of `capacity()` independently
  initialized typed slots for `std::movable T` without borrowed state;
- `std::alignment` carries an explicit minimum byte alignment; and
- `std::alloc_errc` distinguishes `invalid_alignment`, `size_overflow`, and
  `resource_exhausted`.

Both allocator operations return
`expected<std::storage<T>, std::alloc_errc>`. Zero capacity succeeds without a
host allocation. An explicit alignment must be a nonzero power of two, and the
effective alignment is never weaker than `alignof(T)`. Allocation is
failure-atomic and the resulting storage owner, rather than the allocator,
carries release authority.

Each public storage slot is checked. `construct(index, value)` consumes one
completed `T` and requires an uninitialized in-bounds slot; `get` and `get_mut`
require a live slot and return references tied to the storage receiver;
`destroy` requires a live slot; `initialized` reports checked slot state; and
`clear` destroys all live slots in reverse index order. Owner destruction
clears any remaining values and releases the allocation. Arbitrary in-slot
constructor argument forwarding is not part of this first slice. The surface
exposes no address, raw byte view, manual deallocation, `new`, or `delete`.
ADR 022 defines its provenance and comparison with other systems-language
models.

The same module implements two fixed-capacity policies over that owner:

- `try_make_arena<T>(capacity)` returns
  `expected<arena<T>, alloc_errc>`. The move-only arena inserts monotonically,
  reports `arena_errc::full`, and `reset()` destroys all values before reusing
  positions from zero.
- `try_make_pool<T>(capacity)` returns
  `expected<pool<T>, alloc_errc>`. The move-only pool inserts into the lowest
  empty position, reports `pool_errc::full`, and permits checked `erase()` and
  reuse.

Both provide `capacity`, `size`, `empty`, `full`, checked `get`/`get_mut`, and
deterministic reverse-slot cleanup. A returned `size_t` is a position, not a
generation-bearing handle: arena reset or pool erase ends that slot lifetime,
and later reuse may assign the same number to another value. Receiver-tied
borrows prevent mutation, movement, or cleanup while an element reference is
retained. The factories and classes are ordinary GTI source; only their
underlying typed-storage operations are compiler-private.

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

`insert(index, value)` and `erase(index)` support movable, including move-only,
elements. They use checked private slot shifts while the source-defined vector
retains logical size, capacity growth, and API policy. Invalid indices take the
defined storage failure path rather than performing unchecked memory access.

The ordinary vector iterator is read-only and retains one checked borrow of its
owner. It is copy-constructible: copies retain the same shared owner dependency
but carry independent positions, which is sufficient for the source-defined
`std::forward_iterator` contract. It satisfies `std::copy_constructible`, not
the broader `std::copyable`, because it is not copy-assignable or reseatable.
A source-defined helper that receives the iterator by value may return only
the iterator's exact owner dependency: either the carrier value or its exact
stored referent of the same type, never an iterator-position/shell projection.
`begin_mut()` supplies the separate ephemeral mutable iterator used by range
`for`; its exclusive parent loan prevents overlapping owner operations and each
iteration carries one mutable element child loan. The mutable iterator remains
non-copyable. Container-specific invalidation precision remains outside this
slice.

`std::span<T>` is an ordinary source-defined, checked, read-only view over one
contiguous owner. `std::vector<T>::as_span()` and
`std::string::as_span()` are its public owner factories; `first`, `last`, and
`subspan` preserve the same owner dependency without exposing a pointer-length
pair. Local copies, moves, exact factory/relay returns, empty views, checked
indexing, and iteration retain that dependency. Moving, destroying, or
invalidating the owner while any retained view is live is rejected, and
dropping the final view releases the owner for later mutation.

`std::string::view()` returns the same bounded single-owner dependency through
the existing `std::string_view` value type. Literal views remain ownerless
references to static storage; dynamic views retain their source string through
local copies and exact factory/relay calls. A dynamic text view cannot escape a
local owner or be assigned/reseated, placed in a field, global, fixed array, or
dynamic storage, or used to construct a nested dependency graph. Mutable,
multi-owner, nested, and generally reseatable views remain deliberately
unsupported.

`std::forward_list<T>` implements its front operations as ordinary GTI over a
recursive `std::unique_ptr` node chain. Its declaration-only iterator uses the
same exact read-only input-iterator/sentinel shape and is explicitly move-only.
Returning a nullable owner-tied cursor, mutating through traversal, and proving
invalidation remain unavailable, so iteration, reverse, resize, merge, remove,
unique, and sort are still bodyless. The separate list scaffold has no node
representation because a compliant constant-time back operation requires a
safe non-owning link that GTI cannot yet retain. The declaration-only span is
replaced by the executable read-only single-owner `std::span<T>` surface above;
mutable access and raw address exposure remain absent.

The standard library currently has no stable public raw allocator, shared
owner, weak owner, or general unsafe memory API. The typed
`std::allocator`/`std::storage<T>` pair is intentionally not a raw-memory escape
hatch.

`<std/functional>` declares the generic customization primary `std::hash<T>`.
A package may provide a complete exact specialization for a nominal type it
owns, such as `class std::hash<Widget> { ... };`; the specialization must expose
the declared exact call shape. The standard library does not yet provide the
primitive hash implementations or the public hashability capability needed by
an associative container. Exact specialization is the customization mechanism,
not evidence that those library contracts are complete.

## 7.5 Errors And Failure

Library operations that model environmental or resource conditions callers can
reasonably handle return `expected`. Operations documented as infallible at the
type level produce a defined runtime failure for violated checked
preconditions or allocation failure.

An infallible convenience wrapper over a fallible host service may use
`GTI-R0012` when its host operation fails, but it shall not discard that
failure. It should have a recoverable sibling when callers reasonably need to
handle the condition. The current `std::print`/`std::println` implementation
discards the status from the counted and scalar-byte stdout runtime entries;
this is an implementation gap owned by the hosted-service work, not a
permitted hidden stream state.
`std::format` and the `try_print` family return `format_errc` only for malformed
replacement syntax or an argument-count mismatch. The `try_` output names do
not yet make a native stdout failure recoverable; they guarantee that a format
error is reported before any bytes are written.

Wrong-state `expected.value()` and `expected.error()` access is the checked
`GTI-R0009` failure. Infallible allocation uses `GTI-R0011`; recoverable
`std::allocator::try_storage` and future `try_make_*` factories return
`expected` without invoking the failure observer.
Best-effort resource cleanup, such as destruction-time close, may discard a
host error only when the component contract explicitly says so and an explicit
recoverable close operation exists.

`std::thread::start` reports `resource_unavailable` or
`operation_not_permitted`; explicit `join()` reports `not_joinable` or a
recoverable `join_failed`. Those native conditions are not worker task
failures. A task's ordinary defined-failure record is preserved byte-for-byte
and re-raised by explicit or automatic join. Destruction of the transferred
callable and captures remains worker cleanup: its first failure is propagated,
while a failure after an existing record preserves the exact `GTI-R0014`
envelope and terminates on join. Destroying an outstanding handle cannot return
a recoverable join error, so an environmental automatic-join failure moves the
still-coherent state to a non-resumable terminal quarantine and maps to
`GTI-R0012`; it does not detach the worker.

Public vector/string indexing reports `GTI-R0007` against logical size. A
private-storage `GTI-R0010` is an internal invariant failure and must not leak
as the public bounds category. The source-defined wrappers perform the trusted,
identity-bound logical-size check before private storage access; reaching
`GTI-R0010` through a valid public index therefore indicates a broken internal
storage invariant rather than an alternate public bounds outcome.

Each component specification must state whether failure is recoverable,
terminating, or impossible under its preconditions. Hidden native exception or
stream state is not a GTI error model. The common category, cleanup, embedding,
and report rules are in [Execution And Runtime Semantics](execution.md#410-defined-runtime-failure).

## 7.6 Documentation And Conformance

Public library declarations use retained `///` Markdown from GTI source. Hover
already presents that contract from the compiler symbol; completion, signature
help, and generated reference material remain consumers to be connected to the
same value.

Before 1.0, every standardized component requires tests covering primitive,
copyable aggregate, move-only, polymorphic, empty, error, and boundary cases as
applicable.

## 7.7 Library Specification Gaps

The following component families remain planned or incomplete:

- shared and weak ownership;
- optional and general sum types;
- mutable, nested, reseatable, and multi-owner borrowed views;
- complete vector copy, allocator, iterator reassignment, and precise iterator
  invalidation support;
- the remaining callable, complete-range, heterogeneous accumulation, and hash
  generic-capability families;
- indexed/named/specifier-based and floating-point formatting, formatter
  customization, buffered streams, file writes, seeking, and structured I/O;
- general filesystem operations, time, random distributions and engine
  breadth, connected networking, traffic buffers, atomic fetch/wait/fence
  breadth, mutexes, condition
  variables, stop tokens, detached/scoped tasks, typed argument packs, and
  broader threading;
- stateful allocator policy, allocator-aware growing containers,
  generation-bearing pool handles, and heterogeneous arenas;
- broader native interoperability beyond the current bounded records, opaque
  handles, pointers, C strings, returned fixed arrays, and same-thread
  callbacks, including general pointer nesting and ownership transfer, array
  parameters, varargs, alternate calling conventions, native variables, and
  direct C++ linkage.

Their absence is not permission to substitute similarly named C++ facilities
without a GTI contract.
