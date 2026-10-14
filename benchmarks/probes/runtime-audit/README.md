# Runtime optimization audit probes

These probes preserve the evidence behind the 2026-10-08
[runtime optimization audit](../../../docs/plans/optimization.md#runtime-optimization-audit-2026-10-08).
They are screening programs, not benchmark workloads or timing gates. Each
directory pairs an ordinary GTI program (`main.gti`) with an idiomatic C++
twin (`idiomatic.cpp`) that prints the same checksum line. `sort-heap` reuses
the `sort` program but its twin runs the iterative heapsort that `std::sort`
used before introsort. It isolated the library algorithm from compiler
overhead in the original audit and now compares GTI's introsort with that
earlier algorithm.

| Probe | Everyday shape it exercises |
| --- | --- |
| `calls` | failure-capable scalar helper called in a hot loop, result fed back |
| `vector-push` | `push_back` growth and range-`for` summation |
| `matrix` | dense multiply with computed `i * n + j` vector indices |
| `particles` | mutable range-`for` over a vector of float records |
| `hashmap` | `unordered_map` insert, `contains`, and `at` |
| `strings` | `string::push_back` and `find(char)` |
| `dispatch` | interface call through a reference parameter |
| `expected` | `expected<T, E>` with prefix `?` propagation |
| `division` | integer division and remainder with proved and loaded divisors |
| `sort`, `sort-heap` | `std::sort` of one million integers |
| `lambda` | `std::count_if` with a capturing lambda |
| `shifts` | xorshift with literal and masked shift counts |
| `conversions` | narrowing integer conversions and masks |

Some C++ twins avoid undefined behavior that GTI would report: for example,
key generation widens to `int64_t` because the original `int32_t` product
overflowed and GTI correctly failed. Twins never use unchecked access where the
GTI source would fail on the probe's input; they are idiomatic, not
semantic-checking.

Use an optimized host build of GTI and do not run other builds or tests at the
same time. Both modes require a new output directory:

```sh
python3 benchmarks/probes/runtime-audit/screen.py compare \
  --gti build-release/gti --output /tmp/gti-rt-audit-compare
python3 benchmarks/probes/runtime-audit/screen.py attribute \
  --gti build-release/gti --runtime-library build-release/libgti_runtime.a \
  --output /tmp/gti-rt-audit-attribute matrix lambda vector-push
```

`--cxx` and `--std` select the same native compiler/standard for both sides
and for attribution variants. The original dated measurements predate this
argument-forwarding correction. Check its protocol with
`python3 benchmarks/probes/runtime-audit/screen_test.py`.
The `expected` reference uses C++23 `std::expected`, so comparing that probe
requires `--std c++23`; other probes such as `shifts` also support C++20.

`compare` builds GTI and C++ with the same native policy GTI uses (`-std=c++23
-fno-fast-math -ffp-contract=off`), requires identical stdout, and reports
medians and minimums of interleaved samples. On a shared machine, prefer the
minimum: these CPU-bound probes only get slower under interference. `attribute` recompiles the probe's emitted C++ under one
variant at a time and reports each variant's minimum:

- `inline`: the native inline threshold raised to 2000;
- `arith`, `bounds`, `conv`, `slots`: one check family's helper guard rewritten
  to succeed. These are checked add/subtract/multiply, vector index bounds,
  integral conversion range, and the backend's lifetime-slot "read while empty"
  assertion; and
- `all`: every rewrite plus the raised threshold.

**The guard rewrites are unsafe ceilings.** They are valid only because these
inputs never fail, and every variant's output must still match the unmodified
build. They estimate what a proof, hoist, or representation change could
recover; they are never a candidate implementation or a release mode.
