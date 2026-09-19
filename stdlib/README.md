# GTI standard library

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

`<std/random>` provides a deterministic source-defined `std::random_engine`
initialized from an explicit `uint64_t` seed and a fallible
`std::random_device` for nondeterministic host entropy. The engine owns and
advances its four-word state in GTI; only `random_device` crosses the bounded
`gti_rt_random_bytes` C boundary to refill its private fixed buffer. Its call
returns `expected<uint32_t, int>` so an entropy-source failure is explicit.

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

`<std/filesystem>` provides `std::filesystem::path` as pure pathname
manipulation with no filesystem access. It carries C++'s decomposition and
`has_*` query set, the modifiers, `assign`/`append`/`concat`, directory joining
through `/` and `/=`, element-wise `compare` with ordering and equality
operators, and `lexically_normal`.
Element-wise comparison is what makes `"/a/b"` and `"/a//b"` equal while
`"/a/b/"` differs from both, since a trailing separator contributes an empty
final element. A pathname is stored exactly as supplied; normalization happens
only in `lexically_normal`. Copying is explicit through `clone()`, and the
converting `path(std::string_view)` constructor is implemented through the
ordinary verified class-construction route.

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
reference. Iterators remain a later library layer. The language now defines
the structural `begin`/`end` iterator protocol and range-based `for`
independently of `std::array`; adding array iterators remains ordinary library
work. The compiler-owned fixed-array strategy now supports read-only and
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
cover non-finite values. Binary64 text and float arguments to `std::format`
are separate sub-slices.
Read-only `begin()`/`end()` iteration uses the language's
structural range protocol and a source-defined iterator that retains a checked
borrow of the backing storage. A read cursor can be copy-constructed into an
independently positioned cursor; both copies retain the same owner loan and
remain non-assignable. This avoids hidden allocation on an ordinary string
copy and prevents mutation or movement while an iterator remains live. Dynamic
`view()` conversion to `std::string_view` retains an owner-tied lifetime;
the source cannot be invalidated while that view remains live.
`<std/format>` implements sequential `{}`
replacement for heterogeneous fixed-width integral packs, escaped `{{`/`}}`,
and exact field counts. It builds an owning `std::string`; malformed patterns
and count mismatches are returned through `std::format_errc` before output.
Floating-point conversion and formatter customization remain later
standard-library layers and must not make the compiler recognize the public
`std::string` name.
Front/back, mutable `at`, resize, shrink-to-fit, and pop-back are implemented;
`swap` remains bodyless until moving through mutable references is expressible.

`std::vector<T>` is implemented in `std/vector.gti` as an ordinary source-defined
class over `gti_internal::prefix_storage<T>` and imported with
`#include <std/vector>`. `T` must satisfy `std::movable`, and the vector itself is
move-only. The first working surface includes default and size construction,
construction from one contextual `T[N]` value,
size/capacity observation, reserve, clear, push/pop, indexed insert/erase,
checked `at` and `operator[]`, front/back, both resize forms, shrink-to-fit,
variadic `emplace_back`, explicit copyable-element `clone`, and read-only plus
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
`swap` remains bodyless because the current ownership model deliberately
forbids moving an owner out through a mutable reference.

`std::forward_list<T>` now has a source-defined recursive
`std::unique_ptr` node chain. Empty/front, clear, push-front, and pop-front are
working constant-time operations. Its iterator, emplacement, resize, reverse,
merge, removal, uniqueness, and sorting declarations remain bodyless: nullable
owner-tied traversal and the required mutation/invalidation proofs are not yet
available.

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

Container implementations may use the reserved
`gti_internal::storage<T>` compiler facility documented in
[`docs/language/ownership-and-lifetimes.md`](../docs/language/ownership-and-lifetimes.md).
It owns partially initialized capacity and supports
checked variadic in-place construction, receiver-tied read-only and mutable
borrows, destruction, movable-element relocation, and checked within-storage
slot shifts without making raw pointers or manual deallocation part of the
public language. Storage element types containing borrowed state are rejected.
Its allocation extent and slot-live bitmap remain private representation state;
narrow observers exist for the ordinary `<std/memory>` wrapper and expose no
address or bitmap mutation. Containers keep logical size and growth policy as
ordinary private GTI fields.
One read-only `storage<T>&` may be retained by a validated stored-reference
carrier declared in an implicit or imported standard-library unit. This narrow
capability supports source-defined iterators; it does not permit application
storage references, writable stored borrows, raw addresses, or storage escape.
Container classes may provide exact-match constructor overloads; their
default/copy/move/assignment/destruction policy is derived by the compiler from
field lifecycle metadata. A public `~Type()` may drain live elements before
storage teardown. Declared cleanup makes the wrapper noncopyable and uses
compiler-generated active-state moves, preventing moved-from containers from
running the cleanup body twice.

Application code may import `<std/memory>` for the direct public
`std::allocator`, `std::storage<T>`, `std::alignment`, `std::alloc_errc`,
`std::arena<T>`, and `std::pool<T>` surface. Allocation returns `expected`, and
the move-only storage owner provides checked capacity, initialization,
construct-by-value placement, access, destruction, and reverse clear operations
for `std::movable T` without borrowed state. `try_make_arena` and
`try_make_pool` build fixed-capacity move-only policies over that owner. Arenas
insert monotonically and reset as one operation; pools reuse the lowest empty
position after checked erase. Their `size_t` positions are not
generation-bearing handles. Successful zero capacity is well-defined.
Arbitrary in-slot constructor forwarding is deferred until it has an explicit
MIR failure boundary.
The public types are ordinary GTI wrappers: neither compiler nor backend
recognizes their names, and no raw address or manual deallocation is exposed.

`<std/optional>` provides a production move-only `std::optional<T>` over one
declaration-bound inline slot. It supports empty and engaged construction,
checked read-only and mutable access, `has_value`, contextual `bool`,
dereference/arrow, `value_or`, reset, emplacement, move-only payloads, and move
construction. Empty access reports the stable checked owner failure after
ordinary cleanup. The representation performs no allocation for absence and
drops every engaged payload exactly once. Allocation-free describes the slot,
not construction of `T`: payload construction can report its ordinary
`allocation_failure` / `element_construction` outcome, and failed emplacement
leaves the slot empty. Copy construction and both assignment forms are
deliberately unavailable; replace a value through `reset` or `emplace` until
GTI can express a source-defined conditional special-member assignment body.

Intrinsic behavior is restricted to exact compiler-trusted declaration
identities. Shared prefix-storage, text, and core constraints remain
prelude-owned. Unique-owner and sparse-storage identities belong to
`<std/memory>`; managed-thread and defined-integer identities belong to the
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

## Binary32 mathematics

`<std/cmath>` implements its complete declared `float` surface: `abs`,
`isfinite`, `isinf`, `isnan`, `floor`, `ceil`, `round`, `trunc`, `sqrt`,
`cbrt`, `pow`, `fmod`, `exp`, `log`, `log10`, `sin`, `cos`, `tan`, `asin`,
`acos`, `atan`, and `atan2`. Public wrappers are ordinary GTI; the narrow
scalar runtime ABI supplies host numerical implementations. Invalid domains
return NaN and poles/overflow return infinity, not a checked integer failure.
The exact rounding, signed-zero, and portability contract is in
[Standard Library §7.3.1](../docs/language/standard-library.md#731-binary32-mathematics).

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
| `<std/algorithm>` | Implemented `min`, `max`, `clamp`, iterator/sentinel `all_of`, `any_of`, `none_of`, `find`, `find_if`, `find_if_not`, `count`, `count_if`, and `for_each`, forward-iterator `binary_search`, writable-output `copy`, structural complete-range predicate/operation overloads plus generic `fill`, and vector-bounded `transform`, `reverse`, ordered `sort`, and comparator `sort`; iterator-pair in-place forms remain |
| `<std/forward_list>` | Implemented unique-owner front operations; declaration-only traversal and middle-node algorithms |
| `<std/functional>` | Implemented public exact callable/predicate/operation/comparator concepts, consuming `invoke_once`, `less`, `greater`, and equality function objects; declaration-only `hash` |
| `<std/iterator>` | Structural input-iterator/sentinel/exact-dereference concepts, source-composed `forward_iterator`, and implemented forward-only `advance`, `distance`, and `next`, plus a deliberately unconstrained `prev` placeholder |
| `<std/list>` | Move-only owner shape, capability-gated value algorithms, and a move-only read-only iterator/sentinel pair; node storage and bodies remain absent |
| `<std/memory>` | Implemented `unique_ptr`, `make_unique`, `upcast_unique`, typed `allocator`, `storage<T>`, `alignment`, `alloc_errc`, fixed-capacity `arena<T>`/`pool<T>`, and recoverable factories; declaration-only `shared_ptr`, `weak_ptr`, and `make_shared` |
| `<std/numeric>` | Implemented exact fixed-width `wrapping_add/sub/mul`, `saturating_add/sub/mul`, `checked_add/sub/mul`, default and operation-based `accumulate`, homogeneous default and operation-based `inner_product`, unary `transform_reduce`, `gcd`, `lcm`, and `midpoint` |
| `<std/span>` | Executable checked read-only single-owner spans with vector/string factories, local copy/move, subviews, empty ranges, iteration, and no raw-address `data()` API; mutable and multi-owner views remain unsupported |
| `<std/utility>` | Implemented `pair` and `make_pair`; declaration-only `swap` and `exchange`; compiler-defined `std::move` remains implicitly available |

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
- `std::function<R(Args...)>` needs generic class packs, function-signature
  types, escaping callable storage, and ownership rules that GTI does not yet
  have. `<std/functional>` therefore contains only the function objects GTI can
  represent honestly.
- buffered streams still require an owner/view buffer model and a decision on
  `<<` and `>>` customization; type traits require constant evaluation and
  substantially more compile-time reflection. The unbuffered byte-oriented
  `<std/cstdio>` layer does not promise that eventual stream design.
- associative containers should wait for stable hashing, comparison,
  allocation, and iterator-invalidation contracts rather than exposing hollow
  classes whose eventual semantics are unknown.
