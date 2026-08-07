# GTI standard library

Standard-library functions will live here as ordinary GTI declarations and
runtime bindings. Language services such as output should not require keywords,
statement AST nodes, or special cases in the parser and C++ emitter.

GTI loads `prelude.gti` automatically. Its public API lives under `std`, while
compiler-owned declarations under `gti_internal` bind to the native runtime.
`std::print(string)` and `std::println(string)` are implemented in GTI and end
at the `stdout.write` runtime binding.

Runtime bindings are low-level target services, not user-facing built-ins. Add
formatting and other portable behavior in GTI, then cross the runtime boundary
only for operations that require the host platform.

Safe ownership and container APIs should likewise be ordinary nominal GTI
classes under `std`, implemented over compiler-defined `gti_internal`
capabilities. The compiler recognizes the capabilities by trusted semantic
identity; it should not hard-code public wrapper names such as
`std::unique_ptr` or `std::vector` into a backend.

`std::unique_ptr<T>` now follows this structure. Its source-defined operators
wrap `gti_internal::unique_owner<T>`, and `std::make_unique<T>(args...)` is a
source-defined variadic factory. Semantic analysis temporarily recognizes the
factory call to validate the selected `T` constructor before generic-body
monomorphization exists; C++ emission still calls the resolved stdlib function.

Container implementations may use the reserved
`gti_internal::storage<T>` compiler facility documented in
`docs/ownership.md`. It owns partially initialized capacity and supports
checked construction, receiver-tied borrowed reads, destruction, and relocation
without making raw pointers or manual deallocation part of the public language.
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
