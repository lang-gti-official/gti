# Annotated tests

Run `gti test` in this directory. The single `engine-tests` target collects
the `[[test]] void` functions in its source graph, including `events.gti`.
There is no hand-written main or per-function manifest declaration.

GTI compiles the suite once and runs each named function in a fresh child
process. Both state tests see `executions == 0` on entry. A failed check or
panic is reported with its source location while the other children still run.

`std::testing::check(condition, message)` is an ordinary caller-tracked helper
over `std::panic`; successful return means the test passed. Test functions are
safe, defined namespace-scope functions with no parameters and a `void` return.
