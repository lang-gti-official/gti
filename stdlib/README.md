# GTI standard library

The dependency-ordered path from the current library foundation to the stable
v1 surface is tracked in
[`docs/roadmap-to-1.0.md`](../docs/roadmap-to-1.0.md).

Standard-library functions will live here as ordinary GTI declarations and
runtime bindings. Language services such as output should not require keywords,
statement AST nodes, or special cases in the parser and C++ emitter.

GTI loads `prelude.gti` automatically. Its public API lives under `std`, while
compiler-owned declarations under `gti_internal` bind to the native runtime.
`std::print(std::string_view)` and `std::println(std::string_view)` are
implemented in GTI and end at the `stdout.write` runtime binding. The prelude's
`std::string_view` is a transparent alias for a compiler-defined counted view;
the native adapter alone translates it to the private `(data, size)` C ABI.

The prelude also defines transparent aliases `std::size_t = uint64_t` and
`std::ptrdiff_t = int64_t`. Public container sizes, capacities, and indexes use
`std::size_t`; signed differences use `std::ptrdiff_t`. Native C/C++ size types
remain checked ABI-boundary representations rather than GTI source semantics.

Runtime bindings are low-level target services, not user-facing built-ins. Add
formatting and other portable behavior in GTI, then cross the runtime boundary
only for operations that require the host platform.

Optional facilities live under `stdlib/std/` and are imported through logical
standard-library paths such as `include <std/array>`. These imports resolve
against the compiler installation, not relative to the application or through
the native C++ header search path. Library files remain ordinary independently
parsed GTI source units and do not re-export their own dependencies.

`std::array<T, N>` is implemented in `std/array.gti` over a private fixed-array
field. It preserves exact value-generic identity and checked indexing without a
compiler rule for the public class name. The first API supports default
construction, construction from an exact `T[N]` value, `size`, `empty`, `at`,
and read-only or mutable `operator[]`. Class list initialization, iterators,
and constrained `front`/`back` operations remain later library layers. The
language now defines the structural `begin`/`end` iterator protocol and
range-based `for` independently of `std::array`; adding array iterators remains
ordinary library work once container-owned iterator borrows are representable.
See `docs/ranges.md` for that lifetime boundary.

`std::string` is implemented in `std/string.gti` over
`gti_internal::storage<char>` and imported with `include <std/string>`. It is a
move-only owner: construction and append accept `std::string_view`, mutable
indexing requires a mutable receiver, and allocating duplication is explicit
through `clone()`. Read-only `begin()`/`end()` iteration uses the language's
structural range protocol and a source-defined iterator that retains a checked
borrow of the backing storage. This avoids hidden allocation on an ordinary
copy and prevents mutation or movement while an iterator remains live. Dynamic
conversion back to `std::string_view` remains unavailable until views can
retain an owner-tied lifetime. Formatting is a later standard-library layer
and must not make the compiler recognize the public `std::string` name.

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
`docs/ownership.md`. It owns partially initialized capacity and supports
checked construction, receiver-tied read-only and mutable borrows, destruction,
and relocation without making raw pointers or manual deallocation part of the
public language.
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

`gti_internal` is currently restricted to compiler and standard-library code.
A future explicitly opt-in namespace, tentatively described as `dangerous`,
may expose selected low-level capabilities to systems programmers. That API,
its spelling, and whether it includes `new`/`delete`-like operations remain
undecided; standard-library wrappers stay the default application interface.

## Declaration scaffolds

The following optional modules provide C++-familiar names and API shapes as a
starting point for source-defined implementations. Their bodyless functions
and methods are intentional declarations, not working library facilities. A
program may parse and type-check against a declaration, but calling it before a
definition is added will fail during native linking.

Generic callable parameters support confined `void` operations and exact
`bool` predicates. Algorithm bodies still wait for generic range capabilities
and proven callable forwarding rather than relying on native C++ lookup.

| Include | Scaffolded surface |
| --- | --- |
| `<std/algorithm>` | `min`, `max`, `clamp`, predicates, searches, counts, copying, transformation, reversing, and sorting |
| `<std/cmath>` | Common arithmetic, rounding, power, logarithmic, trigonometric, and floating-point classification functions |
| `<std/functional>` | `less`, `greater`, equality function objects, and `hash` |
| `<std/iterator>` | Familiar iterator category vocabulary plus `advance`, `distance`, `next`, and `prev` |
| `<std/memory>` | Declaration-only `shared_ptr`, `weak_ptr`, and `make_shared`; `unique_ptr` and `make_unique` remain in the prelude |
| `<std/numeric>` | `accumulate`, `inner_product`, `gcd`, `lcm`, and `midpoint` |
| `<std/optional>` | The common `optional<T>` observer, access, reset, and emplacement surface |
| `<std/span>` | A safe indexed view shape without a raw-address `data()` API |
| `<std/utility>` | `pair`, `make_pair`, `swap`, and `exchange`; compiler-defined `std::move` remains implicitly available |

Empty constructor bodies in these scaffolds are placeholders because GTI does
not yet support declaration-only constructor syntax. Replace them when adding
the corresponding private state and lifecycle behavior. Keep portable policy
in GTI source and introduce a compiler-private intrinsic or runtime binding only
when the operation cannot be expressed safely in ordinary GTI.

Some familiar C++ facilities are deliberately not scaffolded yet:

- `std::expected` cannot be declared while `expected` and `unexpected` are
  reserved language forms. That naming decision should be resolved before a
  standard-library wrapper is promised.
- `std::function<R(Args...)>` needs generic class packs, function-signature
  types, escaping callable storage, and ownership rules that GTI does not yet
  have. `<std/functional>` therefore contains only the function objects GTI can
  represent honestly.
- streams require a decision on `<<` and `>>` customization and recoverable I/O
  errors; type traits require constant evaluation and substantially more
  compile-time reflection.
- associative containers should wait for stable hashing, comparison,
  allocation, and iterator-invalidation contracts rather than exposing hollow
  classes whose eventual semantics are unknown.
