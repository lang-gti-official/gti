# GTI standard library

`<std/io>` supplies checked reader/writer concepts, exact-read/all-write/copy
helpers, line reading, memory and process-stream adapters, reusable buffering,
and `std::byte_buffer`. `<std/span>` adds exclusive `mutable_span<T>` views
from initialized vector/string elements. Buffered output must be explicitly
flushed; see [I/O](../docs/language/io.md#composable-buffered-io),
[mutable views](../docs/language/ranges.md#mutable-contiguous-views), and
examples 107/108 for ownership and partial-progress behavior.

`<std/simd>` provides fixed float/double lane values and masks, checked
`std::array` copying, arithmetic, explicit lane comparisons and ordered
reductions. See [the SIMD contract](../docs/language/standard-library.md#fixed-lane-simd)
for examples, native/fallback behavior, and the initial slice's bounds.

The dependency-ordered path from the current library foundation to the stable
v1 surface is tracked in
[`docs/plans/roadmap-to-1.0.md`](../docs/plans/roadmap-to-1.0.md).

Standard-library functions live here as ordinary GTI declarations. Host calls
use the same bounded `extern "C"` declaration syntax available to application
code; language services such as output should not require dedicated statements
or call-site spelling recognized by the compiler.

GTI loads `prelude.gti` automatically. Its public API lives under `std`, while
compiler-owned declarations under `gti_internal` bind to the native runtime.
`std::print` and `std::println` accept `std::string_view`, `char`, and every
fixed-width integer type. `<std/string>` adds a read-only `std::string&`
overload for named owners and an owning `std::string` overload for fresh
results. The latter transfers a temporary directly into the call, so an
expression such as `std::println(path.string())` needs neither a named local
nor a reference bound to temporary storage. These operations are implemented
in GTI and end at the exact C symbols `gti_rt_write_stdout` and
`gti_rt_write_stdout_byte`; integer output is ordinary source-defined base-10
conversion rather than a compiler or native formatting shortcut. The
prelude's `std::string_view` is a transparent alias for a compiler-defined
counted view; a C-linkage call lowers it to the public `gti_c_string_view`
record from `<gti/c_abi.h>`. The native callee receives immutable bytes and an
explicit `uint64_t` length and must not retain the buffer after the call.

The prelude also defines transparent aliases `std::size_t = uint64_t` and
`std::ptrdiff_t = int64_t`. Public container sizes, capacities, and indexes use
`std::size_t`; signed differences use `std::ptrdiff_t`. Native C/C++ size types
remain checked ABI-boundary representations rather than GTI source semantics.
Unique ownership is not injected implicitly: source that names
`std::unique_ptr`, `std::make_unique`, or `std::upcast_unique` imports
`<std/memory>` directly.

Native runtime entries are low-level target services, not user-facing
built-ins. Add formatting and other portable behavior in GTI, then cross the
bounded C ABI only for operations that require the host platform. The legacy
compiler-owned `@runtime` allowlist remains an internal compatibility surface;
new standard-library host calls should use explicit `extern "C"` declarations.
See
[`docs/language/native-c-interop.md`](../docs/language/native-c-interop.md) for the exact
source and ABI contract.

`<std/cstdio>` provides the byte-oriented host-I/O foundation with
C++-familiar names and typed policy: counted stdout/stderr, bounded owned stdin
and file chunks, file writes, seek/tell, explicit close, and the move-only RAII
class `std::FILE`. Familiar `r/w/a` mode spellings and their `+`/`b` variants
map to `std::file_mode`. Recoverable outcomes use `expected` and
`std::io_errc`; `fopen` returns
`expected<std::unique_ptr<std::FILE>, std::io_errc>` rather than a nullable or
forgeable native handle. The public layer exposes no native descriptor or raw
buffer. See
[`docs/language/io.md`](../docs/language/io.md) for the exact contract.

`<std/tcp>` provides the first POSIX-only public networking ownership slice.
`std::tcp::open()` returns
`expected<std::tcp::socket, std::tcp::errc>` by forwarding to the public static
factory `std::tcp::socket::open()`. The move-only socket keeps its descriptor
behind the compiler-private implementation detail
`gti_internal::tcp_socket_handle`, and its descriptor-adopting constructor is
private as a second factory-boundary check. Application source cannot name the
handle or the adopting constructor; only compiler-trusted standard-library
source can construct it. Both the TCP handle and the `<std/cstdio>` file
handle use
`[[no_transfer, no_share]]`, so their native thread affinity is explicit even
though each current representation is only an integer descriptor.

Explicit `close()` reports `close_failed` or `not_open`; lexical destruction
performs one best-effort close when the socket remains open. The module binds
the exact C symbols `socket` and `close`, rejects Windows or unknown targets at
import time, and currently opens only an unconnected IPv4 stream socket. It
does not expose a descriptor, address, connect, accept, send, or receive API
through the public wrapper; the `gti_internal` declarations remain
compiler-private implementation details. See
[`docs/language/tcp.md`](../docs/language/tcp.md) for the exact contract.

`<std/http>` provides an owned move-only URL abstraction and a synchronous
reusable HTTP/HTTPS client. URLs parse absolute HTTP(S) text, build encoded
path segments and query parameters from raw components, expose receiver-tied
views or owned copies, and can be cloned before per-request mutation. Requests
copy either URL objects or URL text and own their headers, body, timeouts,
response-size bound, and optional CA bundle path. Responses own their
already-buffered status, duplicate/order-preserving headers, and body; HTTP
error statuses remain response values. Redirects are disabled and TLS peer
and hostname verification cannot be turned off. Portable URL, request, and
error policy are GTI source over a private versioned runtime ABI that
dynamically loads libcurl without exposing its types. See
[`docs/language/http.md`](../docs/language/http.md) for the exact contract.

`<std/json>` is entirely source-defined. It provides a strict owned move-only
DOM, UTF-8-aware parsing with configurable limits, exact decimal number
lexemes and checked conversions, deterministic compact and pretty output,
structured path/location errors, explicit deep cloning, and exact
`std::json::codec<T>` specializations for application models. It rejects
comments, trailing commas, duplicate keys, invalid Unicode, and implicit
object insertion. The source-defined operations execute through verified MIR
and the native C++ backend, including errors that own their JSON Pointer. See
[`docs/language/json.md`](../docs/language/json.md) for the exact contract.

`<std/random>` provides a deterministic source-defined `std::random_engine`
initialized from an explicit `uint64_t` seed and a fallible
`std::random_device` for nondeterministic host entropy. The engine owns and
advances its four-word state in GTI; only `random_device` crosses the bounded
`gti_rt_random_bytes` C boundary to refill its private fixed buffer. Its call
returns `expected<uint32_t, int>` so an entropy-source failure is explicit.

`<std/chrono>` provides exact signed nanoseconds and nominally distinct steady
and system time points. `steady_clock::now()` supports nondecreasing elapsed
measurement without giving its epoch portable meaning; `system_clock::now()`
returns signed Unix nanoseconds and normalizes negative timestamps with
floor-based seconds. Clock reads and elapsed subtraction are recoverable, and
the runtime boundary consists only of two versioned scalar-output entries.
Generic units, sleep, calendars, formatting, and time zones remain outside the
first slice; see
[`docs/language/chrono.md`](../docs/language/chrono.md).

`<std/process>` provides a reusable exact-argv `std::process::command`. It does
not invoke or emulate a shell. `run()` waits synchronously, inherits the current
environment, working directory, and standard streams, and returns either a
structured `exit_status` or a stable `process::errc`. Ordinary exit and signal
termination remain distinct, including the difference between an exec failure
and a program that starts and exits with code 127. The source wrapper owns
validation and policy over the versioned `gti_rt_process_run_v1` scalar/counting
ABI. Capture, redirection, environment/directory overrides, asynchronous child
ownership, and Windows support remain later slices; see
[`docs/language/process.md`](../docs/language/process.md).

`<std/thread>` provides the first public managed-thread slice as an ordinary
source-defined `std::thread`. `std::thread::start(task)` consumes one exact
transfer-safe `void()` lambda or callable object and returns
`expected<std::thread, std::thread_errc>`. The move-only handle may be joined
explicitly and otherwise autojoins during destruction. Move construction
transfers its join obligation; move assignment is unavailable in this first
surface. `std::thread_result<Result>` provides the corresponding typed owner:
`start(task)` consumes one exact transfer-safe `Result()` callable, explicit
`join()` returns `expected<Result, thread_errc>`, and destruction joins then
abandons an unobserved result exactly once. A recoverable native
creation/join condition uses `thread_errc`; a GTI failure in the worker is
cleaned there, preserved exactly, and re-raised by explicit or automatic join.
References, borrowed captures, raw pointers, and non-transferable owned
captures or results are rejected before lowering. Detach, stop tokens,
separate argument packs, scoped cross-thread borrows, and foreign-thread entry
remain outside this surface.

`<std/atomic>` provides move-only `std::atomic<T>` for `bool` and every
fixed-width signed and unsigned integer. Construction, load, store, exchange,
strong compare/exchange, and the fixed-width integer `fetch_add`, `fetch_sub`,
`fetch_and`, `fetch_or`, and `fetch_xor` families default to sequential
consistency. Static order tag types expose every legal
relaxed/acquire/release/acquire-release/SC combination without a runtime enum.
The strong operation never fails spuriously and returns both success and the
actual observed value without exposing a reference into storage. Fetch
operations are source-defined strong-CAS loops and return the replaced value;
add/subtract retain GTI's defined checked-overflow failure rather than wrapping.
Atomic values are transfer- and
share-capable, but no lock-free, stable-layout, or native ABI promise is made.

`<std/mutex>` provides noncopyable `std::mutex<T>` and the move-only,
non-transferable `std::mutex_guard<T>`. `lock()` creates one owner-tied scoped
guard; `get()` exposes a read-only or mutable loan no longer than that guard;
and deterministic cleanup unlocks exactly once, including before a defined
failure propagates. Unlock synchronizes-with a later successful lock of the
same mutex. There is no public manual unlock, recursive locking, fairness, or
poisoning promise.

All three concurrency modules require
`--execution-profile concurrent`; project profiles use
`execution-profile = "concurrent"`.

`<std/filesystem>` provides `std::filesystem::path` for lexical pathname
manipulation alongside recoverable host filesystem operations. The path carries C++'s decomposition and
`has_*` query set, the modifiers, `assign`/`append`/`concat`, directory joining
through `/` and `/=`, element-wise `compare` with ordering and equality
operators, and `lexically_normal`.
Element-wise comparison is what makes `"/a/b"` and `"/a//b"` equal while
`"/a/b/"` differs from both, since a trailing separator contributes an empty
final element. A pathname is stored exactly as supplied; normalization happens
only in `lexically_normal`. Copying is explicit through `clone()`, and the
converting `path(std::string_view)` constructor is implemented through the
ordinary verified class-construction route.
Free operations include status queries, directory creation and enumeration,
copying, renaming, removal, links, working-directory access, and canonical
resolution. They return `expected` values; see
[the filesystem contract](../docs/language/filesystem.md).

Optional facilities live under `stdlib/std/` and are imported through logical
standard-library paths such as `#include <std/array>`. These imports resolve
against the compiler installation, not relative to the application or through
the native C++ header search path. Library files remain ordinary independently
parsed GTI source units and do not re-export their own dependencies.

Comparison algorithms and function objects use source-defined
`std::equality_comparable` and `std::totally_ordered` concepts from the prelude.
The same prelude exposes compiler-backed `std::transferable`,
`std::shareable`, and `std::copy_constructible` concepts over structural type
facts and nominal capability policy. Copy construction is distinct from the
broader `std::copyable` contract, which also requires copy assignment.
Nominal types meet their underlying compiler capabilities only through exact
public read-only member operators; the public library names do not trigger
compiler recognition. Library authors may define conjunction concepts with one
or more type parameters. Generic functions and ordinary non-polymorphic methods
may use bounded trailing `requires` clauses for type capabilities. Ordinary
constructors may also carry bounded requirements. Operators and polymorphic
methods still cannot carry such clauses, so a
container skeleton must not pretend that every C++ conditional member can be
expressed yet.

`std::array<T, N>` is implemented in `std/array.gti` over a private fixed-array
field. It preserves exact value-generic identity and checked indexing without a
compiler rule for the public class name. The first API supports default
construction, construction from an exact `T[N]` value, `size`, `empty`, and
read-only or mutable `front`, `back`, `at`, and `operator[]`, plus
copyable-element `fill`. The contextual fixed-array argument feature permits
the ordinary constructor call `std::array<int, 3>({1, 2, 3})`; it does not add
C++ class list-initialization or `std::initializer_list` semantics. `swap`
remains bodyless because GTI cannot yet move a value out through a mutable
reference. Ordinary owner-borrowing `begin`/`end` cursors support forward,
bidirectional, and signed-jump traversal; `begin_mut` supports confined output
and mutable range-for without exposing the storage address. The language
defines this structural protocol independently of `std::array`.
The compiler-owned fixed-array strategy also supports read-only and
mutable parent/element loans without pointer decay. See
[`docs/language/ranges.md`](../docs/language/ranges.md) for that
lifetime boundary.

`std::string` is implemented in `std/string.gti` over
`gti_internal::prefix_storage<char>` and imported with `#include <std/string>`. It is a
move-only owner: construction and append accept `std::string_view`, mutable
indexing requires a mutable receiver, and allocating duplication is explicit
through `clone()` or `std::string(begin, end, other)`. The indexed constructor
copies `[begin, end)` into independent storage and requires
`begin <= end <= other.size()`, including empty endpoint ranges. Invalid
ranges take the checked `GTI-R0007` string-bounds path before range allocation
or source access. Indices count code units and preserve embedded zero bytes.
`std::to_string(integer)` constructs the canonical base-10
owning representation for every fixed-width integer, including signed minima.
`std::to_string(float)` and the binary32 `std::print`/`std::println` overloads
render an optional sign, the integer part, a point, and exactly six fractional
digits taken from the stored value's exact decimal expansion and rounded to
nearest with ties to even. Signed zero is preserved; `nan`, `inf`, and `-inf`
cover non-finite values. `std::format` instead uses locale-independent
character conversion for both binary32 and binary64 values. Its default
floating presentation is the shortest round-tripping decimal representation;
checked width, alignment, sign, alternate, zero-padding, precision, and
`f`/`e`/`g` presentation specifiers are available through replacement fields.
Alternate default presentation preserves those shortest digits and adds a
decimal point only when one is absent; explicit general precision retains
significant trailing zeroes. Infinity and NaN ignore numeric zero-padding and
use ordinary fill/alignment.
Read-only `begin()`/`end()` iteration uses the language's
structural range protocol and a source-defined iterator that retains a checked
borrow of the backing storage. A read cursor can be copy-constructed into an
independently positioned cursor; both copies retain the same owner loan and
remain non-assignable. This avoids hidden allocation on an ordinary string
copy and prevents mutation or movement while an iterator remains live. Dynamic
`view()` conversion to `std::string_view` retains an owner-tied lifetime;
the source cannot be invalidated while that view remains live.
`c_string()` returns a non-null NUL-terminated view tied to the same owner;
zero-capacity empty strings use a process-lifetime empty sentinel. Embedded NUL
bytes are preserved and C observes the prefix before the first one.
`try_c_string()` returns `nullptr` when the logical string contains an embedded
NUL. Both pointers retain a checked read-only dependency on the string until
their last use, but native code that retains one beyond a call still requires a
binding-specific lifetime contract.
`<std/format>` implements heterogeneous automatic or numeric-indexed fields
for booleans, characters, fixed-width integers, binary32/binary64 values,
string views, and explicitly transferred owning strings, plus escaped
`{{`/`}}`. Fields support fill/alignment, sign, alternate form, zero-padding,
width, precision, integer `b`/`B`/`o`/`d`/`x`/`X`/`c`, floating
`f`/`F`/`e`/`E`/`g`/`G`, and string `s` presentation. It builds an owning
`std::string`; malformed patterns/specifiers, count mismatches, and dynamic
formatter rejections are returned through `std::format_errc` by `try_format`
before output; `format` uses the same implementation and panics on failure.
Exact source-defined `std::formatter<T>` specializations extend the protocol.
The source-defined `std::formattable<T>` concept rejects unsupported static
argument types, including every later heterogeneous pack element. Named
strings use `.view()` for non-consuming formatting because current value packs
have no reference-pack declarator. Named and dynamic fields, locale-aware
formatting remain later standard-library layers;
none of this makes the compiler recognize the public `std::string` name.
Front/back, mutable `at`, resize, shrink-to-fit, pop-back, and constant-time
storage `swap` are implemented. Swap also exchanges size and capacity without
copying characters or allocating.

`<std/encoding>` provides source-defined binary-to-text codecs over counted
`std::string_view` input. `std::encoding::base64` emits and accepts strict
standard-alphabet RFC 4648 text with canonical `=` padding;
`std::encoding::hex` emits lowercase and accepts either letter case. Decoders
return `expected<std::string, std::encoding::decode_errc>` and do not ignore
whitespace, separators, prefixes, or malformed padding. Both codecs preserve
embedded zeroes and arbitrary byte values. They are representations, not
encryption, authentication, or Unicode transforms.

`std::vector<T>` is implemented in `std/vector.gti` as an ordinary source-defined
class over `gti_internal::prefix_storage<T>` and imported with
`#include <std/vector>`. `T` must satisfy `std::move_constructible`; element
assignment is not required. The vector itself is move-only. The first working
surface includes default and size construction,
construction from one contextual `T[N]` value,
size/capacity observation, reserve, clear, push/pop, indexed insert/erase,
checked `at` and `operator[]`, front/back, both resize forms, shrink-to-fit,
variadic `emplace_back`, explicit `std::cloneable`-element `clone`, and read-only plus
mutable structural iteration. The size constructor value-initializes its elements; it
is not a reserve-only constructor. The fixed-array constructor is ordinary GTI
source:
`std::vector<int>({1, 2, 3})` infers `N` from the brace count and copies each
element from the owned array parameter. It therefore requires a copyable
element type when selected; there is no `std::initializer_list`, list-overload
preference, or compiler recognition of `std::vector`.

The hosted entry signature
`int main(int, std::vector<std::string>)` is a narrow language boundary over
these two ordinary owners. The compiler validates their canonical installed
declaration identities and copies native arguments into them before user code
runs. This does not make general vector or string operations compiler
intrinsics: semantics resolves the exact source-defined `push_back` operation
used by the startup adapter, and later phases retain that identity.

`emplace_back(args...)` selects one exact accessible `T` constructor and builds
the element directly in its final storage slot. It returns a writable
receiver-tied reference. GTI does not implement C++ forwarding references:
the method receives an immutable by-value pack, so copyable arguments may be
copied at that boundary and a pack containing a move-only argument is consumed
by its first expansion. The read-only iterator retains one checked storage
borrow. It may be copy-constructed for independent multi-pass traversal while
sharing that owner dependency, but it cannot be copy-assigned or reseated.
Consequently it satisfies `std::copy_constructible` but not `std::copyable`.
A helper receiving this carrier by value may relay only the iterator value or
its exact stored storage referent; cursor fields and other shell projections do
not inherit the external owner dependency.
`begin_mut()` returns a separate compiler-confined, non-copyable iterator carrying
the exclusive range loan; mutable dereference returns the current element's
loop-bounded child loan through checked storage access. Reserve, push, clear,
movement, and other overlapping owner operations are rejected while that range
is active. Owned vector temporaries are supported and destroyed once after
range cleanup. Precise public invalidation effects, iterator-position mutation,
allocator customization, and a complete C++ `vector` API remain future work.
The iterator types are named by the public member aliases `const_iterator`
(`begin()`), `iterator` (`begin_mut()`), and `sentinel` (`end()`), as on the
other containers and views; application code should not spell their
`std::detail` names.
`swap` exchanges storage and capacity without allocation or element movement.
Private receiver methods replace their own storage and return the previous
owner; no move through a borrowed reference is needed. Both owners must be
mutable and distinct, with no live overlapping element/view/iterator loans.

`std::forward_list<T>` has a source-defined exclusive `std::unique_ptr` node
chain. Empty/front and front push/pop are constant-time; clear and destruction
are iterative and linear. Resize, reverse, stable sorted merge, removal,
adjacent uniqueness, and stable O(n log n) sort transfer node owners without
copying or moving stored elements. Only growing resize allocates new nodes.
Merge consumes its other list. Swap exchanges the node chains in constant time
without allocation or element movement and obeys the same exclusive-owner
borrow rules as vector swap. Iterators and emplacement remain bodyless pending
their lifetime and construction capabilities.

Safe ownership and container APIs should likewise be ordinary nominal GTI
classes under `std`, implemented over compiler-defined `gti_internal`
capabilities. The compiler recognizes the capabilities by trusted semantic
identity; it should not hard-code public wrapper names such as
`std::unique_ptr` or `std::vector` into a backend.

`<std/memory>` owns `std::unique_ptr<T>` and its related functions. Its
source-defined operators wrap `gti_internal::unique_owner<T>`, and
`std::make_unique<T>(args...)` is a
source-defined variadic factory resolved through the same generic machinery as
application functions. Concrete HIR instantiation validates its nested owner
allocation and constructor call; C++ emission calls the resolved stdlib
function rather than recognizing the public factory name. Explicit
`std::upcast_unique<Base, Derived>(owner)` consumes one derived owner only when
the compiler-private capability proves a public polymorphic base with safe
destruction; no ordinary call receives an implicit owner conversion.
`unique_ptr<T>::swap(other)` instead requires two matching mutable handles and
exchanges their private owner tokens without consuming either public handle,
copying either pointee, or running replacement cleanup.
`std::shared_ptr<T>` follows the same explicit base-conversion rule through
`shared_ptr<T>::upcast<Base>()` and
`std::upcast_shared<Base, Derived>(owner)`. The conversion consumes one handle
without allocating or changing the shared count, preserves the same control
block for concrete and base aliases, and permits final checked cleanup through
either static view.

Sparse owners and application-defined containers use public
`std::uninitialized<T>`, `construct_at`, `destroy_at` and `relocate_at`,
documented in [raw pointers](../docs/language/raw-pointers.md).
The source-defined `std::storage<T>` implementation owns object and metadata
regions through two separate leases from the same resource. Source GTI owns
live-slot bytes, bounds/state checks, reverse drain guards, relocation loops
and failure-atomic publication. The compiler owns layout, typed lifecycle,
checked loans and exact failure cleanup—not a sparse bitmap or container name.
The former private sparse-storage capability and native helpers are retired;
prefix and optional storage remain separate current capabilities.

The safe storage interface rejects borrowed-state elements and returns
receiver-tied references. General stored-reference rules permit unrelated
application wrappers and iterators to borrow the public owner without exposing
its addresses or slot-state mutation. Container lifecycle is derived from
ordinary field metadata; declared cleanup and generated active-state moves
prevent moved-from owners from running cleanup twice.

Application code may import `<std/memory>` for the direct public
`std::allocator`, `std::storage<T>`, `std::alignment`, `std::alloc_errc`,
`std::arena<T>`, and `std::pool<T>` surface. Allocation returns `expected`, and
the move-only storage owner provides checked capacity, initialization,
construct-by-value placement, access, destruction, and reverse clear operations
for complete object types without borrowed state. Final-address `emplace`
supports immovable payloads; transferring payload values still requires their
movement capabilities. `try_make_arena` and
`try_make_pool` build fixed-capacity move-only policies over that owner. Arenas
insert monotonically and reset as one operation; pools reuse the lowest empty
position after checked erase. Their `size_t` positions are not
generation-bearing handles. Successful zero capacity is well-defined.
Variadic emplacement selects an exact constructor and uses the general MIR
final-address construction and failure boundary.
The public types are ordinary GTI wrappers: neither compiler nor backend
recognizes their names, and no raw address or manual deallocation is exposed.

`<std/optional>` provides a production move-only `std::optional<T>` over one
declaration-bound inline slot. It supports empty and engaged construction,
checked read-only and mutable access, `has_value`, contextual `bool`,
dereference/arrow, `value_or`, reset, emplacement, move-only payloads, and move
construction. Empty access reports the stable checked owner failure after
ordinary cleanup. The representation performs no allocation for absence and
drops every engaged payload exactly once. Allocation-free describes the slot,
not construction of `T`: emplacement resets the old value, constructs the new
payload as ordinary GTI, then transfers the completed payload into the slot.
Constructor failure retains its original record and partial cleanup, and
failed emplacement leaves the slot empty. The private storage operation cannot
dispatch a source constructor. Copy construction and both assignment forms are
deliberately unavailable; replace a value through `reset` or `emplace` until
GTI can express a source-defined conditional special-member assignment body.

Intrinsic behavior is restricted to exact compiler-trusted declaration
identities. Shared prefix-storage, text, and core constraints remain
prelude-owned. Unique-owner, uninitialized-storage and object-lifetime identities
belong to `<std/memory>`; managed-thread and defined-integer identities belong to the
exact installed `<std/thread>` and `<std/numeric>` units. Atomic, iterator, and
callable constraints likewise live in their canonical units, so none of these
module inventories are present when the module is not imported. The root
`gti_internal` namespace and
declarations whose exposed signatures contain its private capabilities are
visible only to compiler-trusted prelude and physical standard-library source.
Application declarations, direct references, aliases, and tooling queries
cannot enter that surface; public wrappers such as `std::FILE` and
`std::unique_ptr` remain the
supported boundary. A future explicitly opt-in low-level API may expose
selected capabilities, but its spelling and manual-lifetime contract remain
undecided.

## Numeric boundary utilities

The ordinary `<std/numeric>` gcd/lcm/midpoint templates handle their full
fixed-width domains: gcd and lcm return nonnegative results, zero inputs are
defined, and an unrepresentable result takes checked failure after cleanup.
Integral midpoint avoids intermediate overflow and rounds toward its first
argument; float/double midpoint preserves finite and subnormal results without
spurious overflow. See [Standard Library §7.3.2](../docs/language/standard-library.md#732-greatest-common-divisor-least-common-multiple-and-midpoint).

## Binary32 mathematics

`<std/cmath>` implements its complete declared `float` surface: `abs`,
`isfinite`, `isinf`, `isnan`, `floor`, `ceil`, `round`, `trunc`, `sqrt`,
`cbrt`, `pow`, `fmod`, `exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, and `atan2`. Public wrappers are ordinary GTI; the narrow
scalar runtime ABI supplies host numerical implementations. Invalid domains
return NaN and poles/overflow return infinity, not a checked integer failure.
The exact rounding, signed-zero, and portability contract is in
[Standard Library §7.3.1](../docs/language/standard-library.md#731-binary32-mathematics).

## C-style threading

`<std/cthread>` currently implements `std::thrd_yield()`: a scheduling hint
that may let another thread run. It is usable from the initial or a managed
thread in either execution profile and grants no synchronization, fairness,
or minimum-delay guarantee. It has no parameters or result and raises no
defined failure. Use atomics or locks, not yield, to order shared accesses.

Safe `std::thrd_current()` and `std::thrd_equal()` observe stable process-local
identities and compare their values in either profile. A manual worker sees
the exact handle returned by creation. Identity alone does not grant callback
entry, a join obligation, or synchronization.

`std::thrd_sleep(std::timespec(...))` waits for a relative duration in either
profile. The optional mutable-reference output receives normalized remaining
time only on interruption. Results are 0 for elapsed, -1 for interrupted, and
-2 for invalid duration or host error. The safe value-input overload permits
in-place retry; the raw-pointer overload requires `unsafe`. `timespec` is an
ordinary GTI value, not a native C layout promise.

The concurrent profile also supports unsafe `std::thrd_create` and
`std::thrd_join`: the caller must keep raw context alive and synchronized, and
successfully join every created worker before hosted entry returns. See the
[concurrency contract](../docs/language/concurrency.md) for exact entry,
failure, and shutdown rules. Unsafe `mtx_init`/`mtx_destroy`, `mtx_lock`,
`mtx_trylock`, `mtx_timedlock`, and `mtx_unlock` now supply plain, recursive,
and timed manual mutexes. Successful acquisition and final release establish
the mutex ordering edge; callers balance every lock and explicitly destroy
each mutex after all uses end. Timed locking uses an absolute UTC timestamp,
not sleep's relative duration. Unsafe `cnd_*` conditions add explicit lifetime,
signal/broadcast, and wait/timed-wait with atomic enrollment/release and mutex
reacquisition, including on timeout or recoverable wait error. Predicate loops
and shared-data synchronization remain caller obligations. Source-defined
`once_flag` and unsafe `call_once` provide atomic one-time initialization with
registered `void()` callbacks. Flags are stable, noncopyable/nonmovable objects;
normal returns observe initialization, and callback failures clean up and
terminate rather than retrying. Detach/exit and thread-specific storage remain
in the completion campaign.
`std::thread` remains the distinct safe managed-thread owner.

## Partial modules and declaration scaffolds

The following optional modules provide C++-familiar names and API shapes as a
starting point for source-defined implementations. Their bodyless functions
and methods are intentional declarations, not working library facilities. A
program may parse and type-check against a declaration, but calling it before a
definition is added will fail during native linking.

Generic callable parameters support confined `void` operations, exact `bool`
predicates, exact non-reference value results supplied by an explicit source
context when they carry no tracked borrowed state or lambda identity, and
proven forwarding through other confined callable parameters. Result inference
through `auto`, borrowed callable results, and general owned callable escape
remain unavailable. One bounded exception permits every reachable return to
move the same by-value callable parameter after repeated confined invocation;
`std::for_each` uses that rule without storage or type erasure. Implemented
predicate and numeric algorithms retain their callable by value in one confined
mutable local. They accept read-callable and mut-callable targets, may invoke
them repeatedly, and reject consuming once-callables. `<std/functional>` now
exposes those same exact relationships as public `read_callable`,
`mut_callable`, `once_callable`, unary/binary predicate and operation,
comparator, indirect iterator predicate/operation, and `once_action` concepts.
`std::invoke_once` is the first consuming utility; no concept or wrapper name
is recognized by the backend.

| Include | Scaffolded surface |
| --- | --- |
| `<std/algorithm>` | Search, bounds, extrema, sortedness, sequence/prefix comparison, contains/count and predicate algorithms; whole-range fill/replace; confined output copy/filter/replace/remove-copy families; vector transform/reverse/sort. See [range contracts](../docs/language/ranges.md#searching-and-rewriting-containers) for exact forms and remaining boundaries. |
| `<std/forward_list>` | Implemented front operations, iterative cleanup, resize, reverse, stable merge/sort, remove, unique, and swap; iteration and emplacement remain declarations |
| `<std/functional>` | Public exact callable/predicate/operation/comparator concepts, consuming `invoke_once`, comparison function objects, and source-defined read-only, mutable, and consuming `function` / `move_only_function`; declaration-only `hash` |
| `<std/iterator>` | Structural input/output, forward, bidirectional, signed-jump and sized-sentinel capabilities, exact element relations, complete-range concepts, `advance`, `distance`, `next`, and `prev` |
| `<std/list>` | Executable doubly-linked slot-owned nodes, checked indexed mutation, ends, emplacement, read traversal, reverse, remove, unique, merge, sort, and iterative cleanup |
| `<std/deque>` / `<std/queue>` | Circular per-element-owned double-ended storage and its FIFO adapter, with checked access and deterministic cleanup |
| `<std/set>` / `<std/unordered_set>` | Unique immutable keys over AVL/hash dictionaries, checked insertion results and traversal, lookup/erase, bounds or hash capacity controls |
| `<std/memory>` | Implemented `unique_ptr` with reset/swap, `make_unique`, `upcast_unique`, explicit-clone `shared_ptr`, `make_shared`, and `upcast_shared`, typed `allocator`, `storage<T>`, `alignment`, `alloc_errc`, fixed-capacity `arena<T>`/`pool<T>`, and recoverable storage factories; declaration-only `weak_ptr` |
| `<std/numeric>` | Implemented exact fixed-width `wrapping_add/sub/mul`, `saturating_add/sub/mul`, `checked_add/sub/mul`, default and operation-based `accumulate`, homogeneous default and operation-based `inner_product`, unary `transform_reduce`, `gcd`, `lcm`, and `midpoint` |
| `<std/span>` | Checked read-only `span<T>` and exclusive move-only `mutable_span<T>` over initialized prefix storage, with vector/string factories, subviews and checked iteration; mutable views support retained shared projections. No raw-address `data()` API or fixed-array factory. |
| `<std/utility>` | Implemented `pair`, `make_pair`, explicit `clone`, and restoring `swap`/`exchange` for structurally relocatable initialized places; compiler-defined `std::move` remains implicitly available |

Empty constructor bodies in these scaffolds are placeholders because GTI does
not yet support declaration-only constructor syntax. Replace them when adding
the corresponding private state and lifecycle behavior. Keep portable policy
in GTI source and introduce a compiler-private intrinsic only when the
operation cannot be expressed safely in ordinary GTI. A required host symbol
should use the bounded `extern "C"` ABI and remain behind a source-defined GTI
wrapper.

Some familiar C++ facilities are deliberately not scaffolded yet:

- `std::expected` cannot be declared while `expected` and `unexpected` are
  reserved language forms. That naming decision should be resolved before a
  standard-library wrapper is promised.
- Owning callbacks have source-defined read-only, mutable, and consuming forms in
  `<std/functional>`: `std::function<R(Args...)>` requires a copy-constructible
  target and supports explicit deep `clone()`, while `std::move_only_function`
  also accepts move-only captures. The wrappers are themselves move-only and
  support construction, empty observation, reset, replacement and invocation.
  Value, read-only reference, and mutable reference arguments are supported, including borrows of
  noncopyable objects. Eligible unambiguous namespace functions are retained as
  exact zero-state items, including through captures, cloning and replacement.
  `R(Args...) mut` wrappers require mutable access for invocation; their clones
  retain independent copies of the target's current state.
  `R(Args...) &&` requires moved invocation; remaining captures live until the
  caller's enclosing full expression ends, including later-argument failure.
  Allocation and empty invocation use the existing checked unique-owner failure
  contract. Borrowed captures/results, inferred fresh-lambda return types,
  global closure storage, and native callback conversion are not supplied by
  these wrappers. L-CALL-02 tracks final combined validation and release, not
  unimplemented consuming signatures or mutable argument packs.
- buffered streams still require an owner/view buffer model and a decision on
  `<<` and `>>` customization; type traits require constant evaluation and
  substantially more compile-time reflection. The unbuffered byte-oriented
  `<std/cstdio>` layer does not promise that eventual stream design.
- associative containers should wait for stable hashing, comparison,
  allocation, and iterator-invalidation contracts rather than exposing hollow
  classes whose eventual semantics are unknown.
