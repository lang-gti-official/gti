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
identity, never by qualified spelling or a public wrapper name. Shared prefix
storage, text, and core constraint identities live in the trusted prelude.
Module-specific identities live only in their exact trusted source units:
unique ownership and sparse storage in `<std/memory>`, managed threads in
`<std/thread>`, checked one-slot storage in `<std/optional>`, atomics in
`<std/atomic>`, mutexes in `<std/mutex>`, defined integer arithmetic in
`<std/numeric>`, iterators in `<std/iterator>`, and callable relations in
`<std/functional>`. Unrelated programs therefore do not carry those private
declaration inventories. Aliases preserve an identity.
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

- `<std/build>` host-side package build-program support through
  `std::build::context`, exact-argv `std::build::command`, and structured
  `std::build::outputs`. This unit is intended for consumer-approved
  `build.gti` programs; command execution is not sandboxed, and Windows fails
  closed in the current implementation; and
- `<std/memory>` `std::unique_ptr<T>` as a nominal move-only unique owner with
  explicit null construction, reset, null assignment, and checked explicit
  base upcast, plus the ordinary generic `std::make_unique<T>` for class/struct
  construction or exact-value/default scalar initialization, and consuming
  `std::upcast_unique<Base, Derived>` functions; and
- `std::array<T, N>` over checked fixed-array storage, including front/back
  access and copyable-element fill; and
- `std::string` as a move-only owner over private character storage, including
  construction from an exact input iterator/sentinel pair whose dereference
  referent is `char`, indexed half-open range copying, front/back and mutable
  checked access, resize,
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
- `std::forward_list<T>` as a move-only linked owner with constant-time front
  access/push/pop, iterative clear, resize, reverse, stable merge/sort, removal,
  and adjacent uniqueness; and
- `<std/iterator>` public `std::input_iterator<I>`,
  `std::sentinel_for<S, I>`, `std::dereferences_to<I, T>`, and
  `std::forward_iterator<I>` concepts over exact private structural and
  lifecycle capabilities, plus forward-only `advance`, `distance`, and
  `next`; and
- `<std/algorithm>` value-returning `min`, `max`, and `clamp`, and structural
  input-iterator `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and
  `count_if`, exact-value `find` and `count`, and returned-callable `for_each`,
  plus `binary_search` over an exact ordered forward-iterator subrange and
  complete-`vector` predicate/operation overloads. Whole-vector copy, fill,
  transform, reverse, ordered sort, and comparator sort are source-defined.
  Iterator clients accept a distinct exact sentinel type. `find` returns its
  moved iterator while retaining the concrete borrowed-owner dependency, and
  both `for_each` forms return the exact moved callable after repeated confined
  invocation. Predicate, operation, and comparator clients state public exact
  concepts, accept read-callable or mut-callable targets, and reject consuming
  once-callables; and
- `<std/cmath>` binary32 classification, absolute value, rounding, roots,
  powers, remainder, exponentials, logarithms, and trigonometry. Public
  functions are ordinary GTI; scalar runtime C calls supply host numerical
  implementations under the contract in Section 7.3.1 below; and
- `<std/cthread>` supplies `std::thrd_yield()`, a host scheduling hint usable
  from the initial or a managed thread in either execution profile. It creates
  no synchronization edge and promises neither fairness nor a minimum delay.
  Safe `std::thrd_current()` observes a stable, nonzero, never-reused process
  identity; `std::thrd_equal()` compares identity values without testing liveness
  or granting join/callback permissions. Both work in either execution profile.
  `std::timespec` and `std::thrd_sleep` supply relative sleep, optional remaining
  time on interruption, safe value/reference overloads, and an unsafe pointer
  overload. Sleep works in either profile and creates no synchronization edge;
  its exact validation and status contract is in
  [Concurrency](concurrency.md#c-style-relative-sleep).
  Its `[[unsafe]]` `std::thrd_create`/`std::thrd_join` wrappers provide manual
  integer-result workers in the concurrent profile, with exact registered
  callback entry, caller-owned context lifetime, and an obligation to join
  before hosted entry returns. Its unsafe C-style mutex family provides explicit
  init/destroy and lock/try/timed/unlock, with plain, recursive, and timed flags,
  conditional acquire/release synchronization, and caller-owned lifetime.
  Unsafe conditions provide init/destroy, signal/broadcast, and wait/timed-wait
  with atomic enrollment/release and reacquisition before return, including
  timeout. `once_flag` and unsafe `call_once` supply registered, release/acquire
  one-time initialization; detach/exit and thread-specific storage remain
  unimplemented; and
- `<std/functional>` public exact `read_callable`, `mut_callable`,
  `once_callable`, unary/binary predicate and operation, comparator,
  iterator-aware indirect predicate/operation, and nullary `once_action`
  concepts; consuming `invoke_once`; and source-defined `less<T>`,
  `greater<T>`, `equal_to<T>`, and `not_equal_to<T>` function objects over the
  corresponding exact comparison concepts. `hash<T>` remains a customization
  scaffold. Source-defined owning callable classes provide
  `std::function<R(Args...)>` and `std::move_only_function<R(Args...)>` for
  repeatable read-only targets with value, read-only reference, or mutable
  reference arguments (for example, `std::function<void(mut int&)>`).
  The corresponding `R(Args...) mut` forms retain a mutable target and require
  a mutable wrapper binding for invocation. Read-callable targets can also be
  retained by a mutable wrapper; the signature itself does not implicitly
  convert between receiver modes. Cloning a mutable `function` copies its
  current target state into an independent owner.
  The `R(Args...) &&` forms require explicit moved invocation and retain their
  remaining captures through the caller's enclosing full expression, not just
  until the target returns. A `function` clone made before consumption creates
  an independently consumable target. These are ordinary GTI source classes
  using verified unique-owner access and consuming receiver rules.
  Borrowed arguments retain caller storage, including noncopyable objects.
  Unambiguous eligible namespace functions can be passed directly or as stored
  function items, and retain their exact target through cloning and replacement.
  Both support empty
  construction, `empty()`, `reset()`, `replace(callback)`, invocation and
  explicit ownership transfer. `function` requires copy-constructible targets
  and exposes an explicit deep `clone()`; the wrapper itself is move-only, as
  with the other owning containers. `move_only_function` also accepts move-only
  captures and has no clone. Allocation uses `make_unique`, and invoking an
  empty wrapper follows checked null-owner failure, not C++ exceptions.
  Borrowed captures/results, inferred fresh-lambda return types, global closure
  storage, and native callback conversion are outside this wrapper contract.
  Combined validation and local-delivery evidence is recorded under L-CALL-02; and
- `<std/numeric>` public `std::accumulates_into<I, T>` and an ordinary
  source-defined default and operation-based `std::accumulate`, homogeneous
  default and operation-based `std::inner_product`, and unary
  `std::transform_reduce` over transferable input-iterator values, exact
  dereference referents, numeric accumulators, and public exact operation
  concepts. The current reduction algorithms execute deterministically from
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
- `<std/optional>` source-defined move-only `std::optional<T>` for
  `std::movable T`. Empty construction performs no allocation; value
  construction, `emplace`, `reset`, `has_value`, contextual `bool`, checked
  `value`, dereference, arrow, and `value_or` are implemented. A move
  construction transfers an engaged payload and its cleanup obligation.
  Constructing `T` can still report its ordinary `allocation_failure` /
  `element_construction` outcome; a failed emplacement leaves the slot empty.
  An empty checked access raises `GTI-R0008` after ordinary cleanup. Copy
  construction and copy/move assignment are unavailable in this slice;
  replacement uses `reset()` or `emplace()`, and a moved source cannot be used
  under GTI's ordinary ownership rules; and
- `<std/cstdio>` counted stdout/stderr, bounded stdin reads, and unbuffered file
  open/read/write/seek/tell/close through recoverable source-defined wrappers
  and a move-only `std::FILE` owner; and
- `<std/chrono>` exact signed `std::chrono::nanoseconds`, distinct monotonic and
  Unix-wall-clock time points, recoverable clock reads, checked elapsed
  subtraction, and normalized pre-epoch Unix second/subsecond decomposition;
  see [Clocks And Durations](chrono.md); and
- `<std/process>` reusable exact-argv `std::process::command` values with no
  shell expansion, synchronous execution inheriting the current environment,
  working directory, and standard streams, recoverable spawn/execute/wait
  errors, and structured ordinary-exit versus signal termination status; see
  [Child Processes](process.md); and
- `<std/http>` an owned move-only `std::http::url` with fallible absolute
  HTTP(S) parsing, secure authority construction, component-safe path/query
  building, explicit cloning, receiver-tied views, and request/client
  overloads, plus a synchronous reusable HTTP/HTTPS client with owned request
  headers and bodies, strict TLS verification, redirects disabled by default,
  bounded buffered responses, duplicate/order-preserving response headers,
  stable construction and transport outcomes, and 4xx/5xx statuses returned
  as ordinary responses. A private versioned runtime boundary dynamically
  loads libcurl without exposing its handles or option surface; see
  [Buffered HTTP and HTTPS](http.md); and
- `<std/json>` a strict move-only owned DOM, UTF-8 and duplicate-key validation,
  exact number-lexeme preservation with checked numeric conversions,
  deterministic compact/pretty serialization, parser resource limits,
  structured path/location errors, explicit deep cloning, and exact
  `std::json::codec<T>` specializations without reflection or implicit field
  mapping. Its source-defined surface executes through verified MIR and native
  C++ emission, including move-only owning error alternatives;
  see [JSON](json.md); and
- `<std/format>` sequential heterogeneous replacement for booleans,
  characters, fixed-width integers, binary32 floats, string views, and
  transferred owning strings through source-defined `std::format`,
  `std::try_print`, and `std::try_println`. The grammar is limited to `{}`,
  `{{`, and `}}`; pattern, argument-count, and unsupported-type errors return
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
  strong compare/exchange, and integer `fetch_add`, `fetch_sub`, `fetch_and`,
  `fetch_or`, and `fetch_xor` default to sequential consistency. Static
  `std::memory_order_relaxed()`, `acquire()`, `release()`, `acq_rel()`, and
  `seq_cst()` tag types expose only operation-legal overloads. Strong
  compare/exchange never fails spuriously and returns success plus the actual
  observation without exposing storage. Fetch operations return the replaced
  value and are source-defined strong-CAS loops; add/subtract use defined
  checked-overflow failure rather than native wrapping. The wrapper is
  transfer- and
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
link until its GTI body is supplied. Array swap, forward-list
iteration and emplacement, and the list scaffold remain in that
category.

The owned program-entry form names the canonical installed
`std::vector<std::string>` specialization. This exact type identity is part of
the language's `main` contract, not a general intrinsic or a promise that
public GTI class layout crosses an ABI. The implementation constructs ordinary
source-defined string and vector values before calling user code; importing
both optional units is therefore required. See
[`programs-and-targets.md`](programs-and-targets.md).

### 7.3.1 Binary32 Mathematics

`<std/cmath>` implements every currently declared `float` operation. `abs`
maps either zero to positive zero; `isfinite`, `isinf`, and `isnan` classify
the value without an integer conversion. `floor` rounds toward negative
infinity, `ceil` toward positive infinity, `trunc` toward zero, and `round`
to nearest with halfway cases away from zero. These operations accept the
whole binary32 domain, including finite magnitudes outside integer ranges.

`sqrt` and `cbrt` compute square and real cube roots. `pow` computes a power;
a finite negative base with a finite non-integral exponent produces NaN.
`fmod(x, y)` is the remainder using a quotient truncated toward zero and
retains the sign of `x`, including a zero result. `exp`, `log`, and `log10`
provide the exponential, natural logarithm, and base-ten logarithm. `sin`,
`cos`, `tan`, `asin`, `acos`, `atan`, and `atan2(y, x)` use radians; `atan2`
uses both signs to select a quadrant, including signed-zero inputs.

Invalid domains produce NaN rather than a GTI checked failure: these include
negative nonzero square-root arguments, negative logarithm arguments,
out-of-range arcsine/arccosine arguments, infinite direct trigonometric
arguments, and a zero divisor or infinite numerator for `fmod`. Logarithms of
either zero produce negative infinity. Poles and overflow produce signed
infinity as appropriate; underflow produces a subnormal or signed zero.
NaN inputs propagate except for the power identities `pow(NaN, 0) == 1`
and `pow(1, NaN) == 1`. Zero-preserving operations retain its sign, including
square/cube root, odd trigonometric operations, and all four rounding operations.

The hosted scalar ABI uses IEEE binary32 with the ordinary round-to-nearest,
gradual-underflow environment. Native code must not leave that environment
changed when returning to GTI. Numerical approximations use the host math
library: finite transcendental results are not promised bit-identical across
platforms. Native `errno`, floating-point status flags, NaN payloads, and
signaling-NaN behavior are not public APIs. These functions are runtime
operations, not `constexpr`; binary64 overloads are not currently declared.

### 7.3.2 Greatest Common Divisor, Least Common Multiple, And Midpoint

`<std/numeric>` supplies source-defined `gcd<T>`, `lcm<T>`, and `midpoint<T>`.
`gcd` and `lcm` accept each of the eight fixed-width integer types; their two
arguments have the same exact type. `gcd` returns the nonnegative greatest
common divisor, with `gcd(0, 0) == 0`. Negative inputs and signed minima are
valid when the result fits `T`; for example, `gcd(INT_MIN, 2)` is 2. `gcd`
remains usable in supported constant-evaluation contexts.

`lcm` returns the nonnegative least common multiple; either zero input yields
zero, including `lcm(0, 0)`. It divides by the gcd before multiplying. If a
nonnegative gcd or lcm cannot fit `T`, ordinary checked arithmetic or conversion
reports a defined failure after caller cleanup, rather than wrapping or
returning a negative value. A failed required constant evaluation is diagnosed
at compile time.

Integral `midpoint(a, b)` returns the halfway value rounded toward `a`, without
an overflowing intermediate, including opposite extrema. Float and double
midpoints avoid spurious overflow for finite arguments and preserve subnormal
accuracy; IEEE round-to-nearest/ties-to-even applies. Two negative zeros give
negative zero, mixed zero signs give positive zero, equal-sign infinities give
that infinity, opposite infinities give NaN, and a NaN input gives NaN without
a payload guarantee. Inferred and explicit generic calls share this contract.
`bool` and `char` are not numeric arguments for these templates.

The existing `std::MAX_UINT64` constant is exactly 18446744073709551615, not
the signed 64-bit maximum.

## 7.4 Allocation And Containers

Public ownership and container classes retain logical state and policy in
ordinary GTI fields. Compiler-private capabilities may supply unique ownership
or partially initialized storage without exposing raw addresses, manual
deallocation, logical size, or capacity policy as public language state.

`vector::swap`, `string::swap`, and `forward_list::swap` exchange their owned
storage in constant time without allocation or copying, moving, or destroying
elements. Vector and string capacities follow the exchanged storage; sizes and
string code units (including embedded zeros) are preserved. Empty owners are
valid, and both owners remain independently mutable and destructible afterward.
`filesystem::path::swap` delegates to string storage exchange.

Both arguments must be distinct mutable owners. Existing loans into either
owner, including element references, iterators, spans, and string views,
prevent swap while live. In particular, `a.swap(a)` is rejected as overlapping
mutable access, not treated as a runtime no-op. Swapping does not migrate a
retained reference to the other owner. General `std::swap`/`std::exchange` and
`array::swap` remain separate incomplete operations.

`std::string(begin, end, other)` copies the code units in the half-open interval
`[begin, end)` into an independent owning string. Its `size_t` endpoints must
satisfy `begin <= end <= other.size()`. Equal endpoints are valid, including
`other.size()` and an empty source. Reversed or out-of-bounds intervals raise
`GTI-R0007` (`index_out_of_bounds` in `string`) after ordinary cleanup; checks
precede endpoint subtraction, allocation of the copied range, and source reads.
The source is borrowed only during construction. Later mutations of either
string do not affect the other, and the result may outlive its source. Indices
count code units, not Unicode characters, and embedded zero bytes are copied.

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

`std::forward_list<T>` uses an exclusive `std::unique_ptr` node chain. Node
algorithms detach and reattach owners without retaining traversal borrows.
`reverse`, `remove`, adjacent `unique`, and sorted `merge` are linear; `sort`
is stable bottom-up mergesort with O(n log n) comparisons and constant
auxiliary node ownership. These algorithms allocate no nodes and do not copy
or move stored elements. `merge` consumes the other list by value and requires
both inputs sorted; equal elements from the receiving list come first.
`unique` keeps the first element in each adjacent equal run. `remove` and
`unique` return the number removed. Resize preserves the existing prefix,
default-constructs or copies only added elements, and destroys a truncated
suffix. Clear and ordinary destruction drain the chain iteratively.

The declaration-only iterator is explicitly move-only and read-only.
Nullable owner-tied traversal and invalidation remain unavailable; iterator
operations and generic emplacement are still bodyless. The separate
list scaffold has no node representation because a compliant constant-time
back operation requires a
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
`std::format` and the `try_print` family return `format_errc` for malformed
replacement syntax, an argument-count mismatch, or an unsupported argument
type. The `try_` output names do not yet make a native stdout failure
recoverable; they guarantee that a format error is reported before any bytes
are written.

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
- the remaining callable, output/random-access complete-range, heterogeneous
  accumulation, and hash generic-capability families;
- indexed/named/specifier-based and binary64 formatting, formatter
  customization, buffered streams, explicit text decoding, and structured I/O;
- general filesystem operations, calendar/time-zone breadth, random distributions and engine
  breadth, connected networking, traffic buffers, atomic wait/notify, pointer
  atomics and fences, condition variables, recursive/read-write mutexes, stop tokens,
  detached/scoped tasks, typed argument packs, and broader threading;
- stateful allocator policy, allocator-aware growing containers,
  generation-bearing pool handles, and heterogeneous arenas;
- broader native interoperability beyond the current bounded records, opaque
  handles, pointers, C strings, returned fixed arrays, and same-thread
  callbacks, including general pointer nesting and ownership transfer, array
  parameters, varargs, alternate calling conventions, native variables, and
  direct C++ linkage.

Their absence is not permission to substitute similarly named C++ facilities
without a GTI contract.
