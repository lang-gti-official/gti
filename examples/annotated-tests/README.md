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

The scene-state case also demonstrates value equality, quoted string checks,
approximate numeric equality with an explicit tolerance, and element comparison
through borrowed contiguous spans. Successful checks do not format diagnostics;
failed checks use the same cleanup-preserving, caller-tracked panic path.

Use runner controls to select and bound the work:

```sh
gti test --list
gti test --filter engine::tests::fresh_scene_state --exact
gti test --skip event
gti test --jobs 2 --timeout 5s
gti test --format json
```

The JSON format emits one versioned event per line for test inventory,
execution and the final result; build progress remains on the error stream.
An empty selection fails unless `--allow-empty` is explicit. Each test retains
its own process when several tests run concurrently.
