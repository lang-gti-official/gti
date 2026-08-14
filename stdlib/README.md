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
`std::print(std::string_view)` and `std::println(std::string_view)` are
implemented in GTI and end at the exact C symbol `gti_rt_write_stdout`. The
prelude's `std::string_view` is a transparent alias for a compiler-defined
counted view; a C-linkage call lowers it to the public `gti_c_string_view`
record from `<gti/c_abi.h>`. The native callee receives immutable bytes and an
explicit `uint64_t` length and must not retain the buffer after the call.

The prelude also defines transparent aliases `std::size_t = uint64_t` and
`std::ptrdiff_t = int64_t`. Public container sizes, capacities, and indexes use
`std::size_t`; signed differences use `std::ptrdiff_t`. Native C/C++ size types
remain checked ABI-boundary representations rather than GTI source semantics.

Native runtime entries are low-level target services, not user-facing
built-ins. Add formatting and other portable behavior in GTI, then cross the
bounded C ABI only for operations that require the host platform. The legacy
compiler-owned `@runtime` allowlist remains an internal compatibility surface;
new standard-library host calls should use explicit `extern "C"` declarations.
See
[`docs/language/native-c-interop.md`](../docs/language/native-c-interop.md) for the exact
source and ABI contract.

`<std/cstdio>` provides the first host-input slice with C++-familiar names:
`std::getchar`, `std::fopen`, `std::fgetc`, `std::fclose`, and the move-only
RAII class `std::FILE`. It is intentionally read-only and unbuffered: each read
requests one byte from the operating system, and only modes `"r"` and `"rb"`
are accepted. Recoverable outcomes use `expected` and `std::io_errc`; `fopen`
returns `expected<std::unique_ptr<std::FILE>, std::io_errc>` rather than a
nullable or forgeable native handle. See
[`docs/language/io.md`](../docs/language/io.md) for the exact contract.

`<std/tcp>` provides the first POSIX-only public networking ownership slice.
`std::tcp::open()` returns
`expected<std::tcp::socket, std::tcp::errc>` by forwarding to the public static
factory `std::tcp::socket::open()`. The move-only socket keeps its descriptor
behind the compiler-private implementation detail
`gti_internal::tcp_socket_handle`, and its descriptor-adopting constructor is
private as a second factory-boundary check. Application source cannot name the
handle or the adopting constructor; only compiler-trusted standard-library
source can construct it. Both the TCP handle and the prelude file handle use
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

Optional facilities live under `stdlib/std/` and are imported through logical
standard-library paths such as `#include <std/array>`. These imports resolve
against the compiler installation, not relative to the application or through
the native C++ header search path. Library files remain ordinary independently
parsed GTI source units and do not re-export their own dependencies.

Comparison algorithms and function objects use source-defined
`std::equality_comparable` and `std::totally_ordered` concepts from the prelude.
The same prelude exposes compiler-backed `std::transferable` and
`std::shareable` concepts over structural type facts and nominal capability
policy.
Nominal types meet their underlying compiler capabilities only through exact
public read-only member operators; the public library names do not trigger
compiler recognition. Library authors may define conjunction concepts with one
or more type parameters. Generic functions and ordinary non-polymorphic methods
may use bounded trailing `requires` clauses for type capabilities. Constructors,
operators, and polymorphic methods still cannot carry such clauses, so a
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
work once fixed-array owner dependencies or the compiler-owned fixed-array
iteration strategy are implemented. See
[`docs/language/ranges.md`](../docs/language/ranges.md) for that
lifetime boundary.

`std::string` is implemented in `std/string.gti` over
`gti_internal::storage<char>` and imported with `#include <std/string>`. It is a
move-only owner: construction and append accept `std::string_view`, mutable
indexing requires a mutable receiver, and allocating duplication is explicit
through `clone()`. Read-only `begin()`/`end()` iteration uses the language's
structural range protocol and a source-defined iterator that retains a checked
borrow of the backing storage. This avoids hidden allocation on an ordinary
copy and prevents mutation or movement while an iterator remains live. Dynamic
conversion back to `std::string_view` remains unavailable until views can
retain an owner-tied lifetime. Formatting is a later standard-library layer
and must not make the compiler recognize the public `std::string` name.
Front/back, mutable `at`, resize, shrink-to-fit, and pop-back are implemented;
`swap` remains bodyless until moving through mutable references is expressible.

`std::vector<T>` is implemented in `std/vector.gti` as an ordinary source-defined
class over `gti_internal::storage<T>` and imported with
`#include <std/vector>`. `T` must satisfy `std::movable`, and the vector itself is
move-only. The first working surface includes default and size construction,
construction from one contextual `T[N]` value,
size/capacity observation, reserve, clear, push/pop, checked `at` and
`operator[]`, front/back, both resize forms, shrink-to-fit, variadic
`emplace_back`, explicit copyable-element `clone`, and read-only structural
iteration. The size constructor value-initializes its elements; it is not a
reserve-only constructor. The fixed-array constructor is ordinary GTI source:
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
borrow and therefore prevents vector mutation or movement while live. Mutable
iteration, precise invalidation effects, owned temporary ranges, insert/erase,
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

`std::unique_ptr<T>` now follows this structure. Its source-defined operators
wrap `gti_internal::unique_owner<T>`, and `std::make_unique<T>(args...)` is a
source-defined variadic factory resolved through the same generic machinery as
application functions. Concrete HIR instantiation validates its nested owner
allocation and constructor call; C++ emission calls the resolved stdlib
function rather than recognizing the public factory name.

Container implementations may use the reserved
`gti_internal::storage<T>` compiler facility documented in
[`docs/language/ownership-and-lifetimes.md`](../docs/language/ownership-and-lifetimes.md).
It owns partially initialized capacity and supports
checked variadic in-place construction, receiver-tied read-only and mutable
borrows, destruction, and movable-element relocation without making raw
pointers or manual deallocation part of the public language. Storage element
types containing borrowed state are rejected.
Storage does not expose its allocation extent or per-slot initialization state.
Containers keep logical size and capacity as ordinary private GTI fields.
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

Intrinsic behavior is restricted to exact trusted-prelude declaration
identities. The root `gti_internal` namespace and declarations whose exposed
signatures contain its private capabilities are visible only to
compiler-trusted prelude and physical standard-library source. Application
declarations, direct references, aliases, and tooling queries cannot enter that
surface; public wrappers such as `std::FILE` and `std::unique_ptr` remain the
supported boundary. A future explicitly opt-in low-level API may expose
selected capabilities, but its spelling and manual-lifetime contract remain
undecided.

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
through `auto`, borrowed results, and owned callable escape remain unavailable.
Implemented predicate and numeric algorithms retain their callable by value in
one confined mutable local. They accept read-callable and mut-callable targets,
may invoke them repeatedly, and reject consuming once-callables.

| Include | Scaffolded surface |
| --- | --- |
| `<std/algorithm>` | Implemented `min`, `max`, `clamp`, `all_of`, `any_of`, `none_of`, `find_if`, `find_if_not`, and `count_if`; remaining search, copy, transform, reverse, and sort declarations |
| `<std/cmath>` | Implemented binary32 `abs`, `isfinite`, `isinf`, and `isnan`; remaining arithmetic, rounding, power, logarithmic, and trigonometric declarations |
| `<std/forward_list>` | Implemented unique-owner front operations; declaration-only traversal and middle-node algorithms |
| `<std/functional>` | Implemented `less`, `greater`, and equality function objects; declaration-only `hash` |
| `<std/iterator>` | Structural input-iterator/sentinel concepts and implemented forward-only `advance`, `distance`, and `next`, plus a deliberately unconstrained `prev` placeholder |
| `<std/list>` | Move-only owner shape, capability-gated value algorithms, and a move-only read-only iterator/sentinel pair; node storage and bodies remain absent |
| `<std/memory>` | Declaration-only `shared_ptr`, `weak_ptr`, and `make_shared`; `unique_ptr` and `make_unique` remain in the prelude |
| `<std/numeric>` | Implemented exact fixed-width `wrapping_add/sub/mul`, `saturating_add/sub/mul`, `checked_add/sub/mul`, default and operation-based `accumulate`, homogeneous default and operation-based `inner_product`, unary `transform_reduce`, `gcd`, `lcm`, and `midpoint` |
| `<std/optional>` | The common `optional<T>` observer, access, reset, and emplacement surface |
| `<std/span>` | A move-only read-only indexed view shape without source construction, mutable access, iteration, or a raw-address `data()` API |
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
  substantially more compile-time reflection. The unbuffered read-only
  `<std/cstdio>` slice does not promise that eventual stream design.
- associative containers should wait for stable hashing, comparison,
  allocation, and iterator-invalidation contracts rather than exposing hollow
  classes whose eventual semantics are unknown.
