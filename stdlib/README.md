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
