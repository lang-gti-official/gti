# GTI benchmarks

This directory contains deterministic whole-program workloads for comparing
GTI with native C++ under one compiler and optimization policy. Benchmarks are
correctness experiments first: the runner builds every declared variant,
requires one identical `GTI-BENCH-1` result record, and only then collects
timings. Shared CI may validate builds and records, but must not enforce a wall
time threshold.

`atomic-wait-ready` compares 50 million already-satisfied atomic waits plus
one/all notifications against the same native atomic operations. It measures
the no-parking fast path, not blocked wake latency, fairness or scheduler
throughput. The source's state and final observation are identical; thread
handoff/broadcast correctness and TSan are covered separately by
`tests/fixtures/atomic_wait.gti`. No GTI per-wait state/allocation is required;
platform parking implementations may have their own process-global costs.

`borrowing-callbacks` compares an exact mutable borrowed lambda, a direct
signature-erased `function_ref`, and a two-word C++ borrowed view over the same
40-million-step checked integer recurrence. Every variant checks the expected
final state. The callback environment and view require no heap allocation.
This measures an optimizer-visible, single-threaded target; it does not claim
to measure opaque cross-module dispatch, retained native registrations or
concurrent callbacks. Use the ordinary comparison runner with
`--workload borrowing-callbacks` and retain emitted-code/size evidence alongside
timings.

## Compiler latency

Use an optimized **host build** of GTI and matched native compiler, standard,
optimization level, and strict floating-point policy. Compare equivalent work
and library use, not merely equal source-line counts. Clean compilation,
unchanged project builds, and edited rebuilds are different measurements.

```sh
python3 scripts/compile_time_compare.py \
  --gti build/gti --baseline-gti /path/to/preserved/gti \
  --cxx clang++ --output /tmp/gti-compile-comparison-new \
  --workload fixed-array-loop-proof --workload vector-checked-loop \
  --workload scalar-fp-update --workload game-update-stage \
  --workload interface-exact-dispatch --runs 5 --warmup 1
```

The optional baseline is a preserved compiler executable using the same source
libraries/runtime. The output directory must be new. Defaults measure O0 and
O2; repeat `--optimization` to select levels. Do not run builds, tests, or other
benchmarks concurrently. The runner randomizes/interleaves sequential samples
with a fixed seed and records commands, raw times, compiler/source hashes,
build provenance, retained C++, and stdout/stderr. It measures full direct
builds, GTI through C++ emission, native rebuilding of that generated C++, and
both C++ twins separately. These independent samples do not add up exactly:
native-only attribution retains the provenance command's dependency-file
generation, while timed GTI full builds omit build-report generation.

The filesystem is warm; this is not a cold-disk experiment. Direct builds do
not use the project cache. O2/O3 executables must produce identical complete
stdout; large O0 runtime loops are not executed by this compile-time runner.
Before/after generated-C++ equality is reported, not required for arbitrary
compiler changes. `compile_time_harness` is a threshold-free correctness gate
for the measurement path, output preservation, and argument validation.

The **provisional** comparison band is GTI <= 2 × idiomatic C++ + 250 ms.
This is a transparent starting policy for small interactive workloads, not an
established user requirement or noisy CI timing threshold. “Comparable” means
representative workloads meet an agreed band; a “practical plateau” instead
means further measured improvement would require disproportionate complexity.
Neither means GTI has reached a physical lower bound. The bounded investigation
and remaining costs are recorded in
[performance tooling](../docs/plans/performance-tooling.md).

## LSP scalability

`scripts/lsp_benchmark.py` drives `gti_lsp` over its real JSON-RPC transport
and retains raw latency samples plus opt-in frontend work traces. Its fixture
contains a root, configurable direct dependencies, and the standard-library
prelude. Each iteration measures cold open to diagnostics, a root-only edit,
a dirty dependency edit and dependant refresh, semantic tokens, hover,
completion, cancellation, and superseded document generations.

The LSP writes trace records only when the benchmark supplies the
`GTI_LSP_FRONTEND_TRACE` environment variable. Each record names every unit
loaded, lexed, parsed, and semantically analysed for that request, along with
lex/parse cache hits, phase times, and bounded-cache occupancy. Schema 2
summarizes those counters for each interaction and fails unless a root-only
edit reuses every unchanged dependency and prelude unit, dirty dependency work
is scoped to that unit plus its affected dependant parse, and observed cache
occupancy stays within both limits.
The benchmark's `--smoke` mode is a correctness and schema gate only and never
enforces wall time.

For a stable local comparison, use the same binary configuration, machine,
dependency count, warmup count, and sample count for both runs:

```sh
python3 scripts/lsp_benchmark.py \
  --lsp build/gti_lsp --stdlib stdlib \
  --output build/lsp-benchmarks --json build/lsp-benchmarks/baseline.json \
  --dependencies 16 --warmup 2 --runs 10

python3 scripts/lsp_benchmark.py \
  --lsp build/gti_lsp --stdlib stdlib \
  --output build/lsp-benchmarks --json build/lsp-benchmarks/after.json \
  --dependencies 16 --warmup 2 --runs 10 \
  --baseline build/lsp-benchmarks/baseline.json
```

When both binaries are available, prefer one interleaved campaign so thermal
or background-load drift affects them evenly:

```sh
python3 scripts/lsp_benchmark.py \
  --lsp build/gti_lsp \
  --baseline-lsp /path/to/unchanged-baseline/gti_lsp \
  --stdlib stdlib --output build/lsp-benchmarks \
  --json build/lsp-benchmarks/interleaved.json \
  --dependencies 16 --warmup 5 --runs 30
```

The runner alternates current-first and baseline-first pairs and retains both
sets of raw samples in the schema-2 report.

The comparison reports the repeated-edit reduction and cold-open regression,
and reaches `targetMet` only when the work invariants also pass. A result is a
valid campaign comparison only when both binaries use the same build type,
machine, dependency count, warmup count, and sample count.
Reports and generated fixtures belong under the selected output directory and
are not source artifacts.

## Workloads and variants

Each directory under `workloads/` contains a versioned `benchmark.json` and
the sources named by its `variants` array. Descriptors contain only structured
data: they cannot run shell commands, interpolate the environment, or select
output paths.

Schema 1 owns the GTI-versus-C++ comparison shape and requires exactly one GTI
variant. Schema 2 also permits multiple GTI variants so one language feature
can be compared against another GTI execution strategy. Every schema-2 GTI
variant declares `execution_profile` as `single-threaded` or `concurrent`;
every C++ variant declares it as `null`. The runner validates the field and
retains the selected profile in the exact GTI build command and schema-2 build
record. Schema-1 commands and serialized build records retain their original
shape and do not acquire an `execution_profile` field.

`--native-optimization` enables a report-schema-3 attribution run without
changing either descriptor schema. `--optimization` still selects GTI's MIR
policy, while the bounded native option selects one of `0` through `3` for
every measured native executable. For a GTI variant, the runner first performs
the ordinary driver build with retained C++ and a validated build-report
sidecar, then derives a second native command from that exact sidecar. Only the
single driver-owned optimization flag, dependency-file path, and output path
may change. Both commands and both executables remain in the report. Runs that
omit the option retain the frozen schema-1 shape or existing schema-2
provenance behavior exactly.

`--arithmetic-success-proxy WORKLOAD/VARIANT` additionally enables report
schema 4. The selected target must be a GTI variant in the same invocation and
the mode requires `--native-optimization`. The runner derives one extra
measured executable from that target's validated build provenance and retained
C++. It locates exactly one definition of each compiler-owned
`gti_internal::backend::mir_checked_add_v1`,
`mir_checked_subtract_v1`, and `mir_checked_multiply_v1` helper, then replaces
exactly the helpers' three `if (overflow) {` guards with `if (false) {`. Missing,
duplicate, or structurally unexpected helpers fail the run. There is no
descriptor-provided pattern, command, or transformation surface.

The generated variant is a **successful-path-only, non-semantic attribution
proxy**. It participates in the same correctness validation and randomized
sample interleaving as ordinary variants, but it does not preserve GTI's
overflow-failure behavior and is never an exact-head proof input. Schema 4
records the original and transformed source hashes, exact helper family and
replacement count, native command, stdout, stderr, dependency file, and
executable. It is suitable for measuring the upper bound available from
successful checked arithmetic, not for validating a compiler optimization.

The normal schema-1 variants are:

- `gti`: the checked GTI program compiled by the normal direct driver;
- `cpp-semantic`: C++ with the relevant GTI checks represented explicitly;
- `cpp-idiomatic`: an optional safe-for-the-declared-input C++ baseline whose
  deliberate semantic differences are documented below.

Generated sources, executables, reports, profiles, and raw samples belong
under the runner's selected output root, normally `build/benchmarks/`. They
must not be written beside workload sources.

The runner invokes tools with argument vectors and never accepts shell command
text. Descriptor and source paths cannot escape through `..` or symbolic
links; report paths must remain under the canonical output root. Each run gets
a new directory and the runner never recursively deletes or replaces a prior
run. Schema 1 permits only a small target/vectorization native-flag allowlist;
optimization, C++ standard, output, response-file, and linker-control flags
cannot override the common build policy. The semantic C++ variants receive
the same strict floating-point flags that the GTI driver enforces.

## Result protocol

Every executable performs fixed work and prints exactly one record on success:

```text
GTI-BENCH-1 digest:<16 lowercase hexadecimal digits> work-units:<decimal count>
```

The digest must depend on the measured result rather than on an unrelated
constant. A process exits with zero only after its computed digest and any
workload-specific invariants match the checked-in expectation. Timings are
invalid if variants produce different normalized records.

## `vector-checked-loop`

This workload reproduces the checked `std::vector<int32_t>` scale-and-sum
shape reported in issue #32. It constructs and value-initializes 5,000 logical
elements, writes a deterministic `1..17` pattern through checked indexing,
then performs 240,000 rounds. Every round scales all elements by a varying
`-1`/`1` multiplier and sums all elements from left to right.

One logical work unit is one element visit in a timed `scale` or `sum` pass.
Setup is excluded, so the workload performs
`5,000 * 240,000 * 2 = 2,400,000,000` work units. Each round's checked sum feeds a
bounded rolling digest; the final expected record is:

```text
GTI-BENCH-1 digest:00000000135e33a6 work-units:2400000000
```

The comparison contract is intentionally explicit:

- GTI uses the source-defined vector size constructor and checked `operator[]`.
  Its `int32_t` scaling and left-to-right summation retain GTI overflow
  behavior at every optimization level.
- `semantic.cpp` value-initializes the same logical vector, checks every
  logical index before access, and uses checked signed and unsigned arithmetic.
  It does not replace the reduction with a widened accumulator.
- `idiomatic.cpp` uses ordinary range-based C++ loops. It has no undefined
  behavior for this fixed input: elements stay in `[-17, 17]`, each partial sum
  stays in `[-44,985, 44,985]`, and digest operands are bounded before each
  operation. It does not provide GTI's dynamic bounds and overflow guarantees,
  so it is an idiomatic baseline, not the semantic-equivalent comparison.

The alternating multiplier keeps the arithmetic bounded while still making
the element-wise multiply and reduction checks observable. No variant relies
on signed overflow, wrapping, disabled checks, or unchecked invalid storage.

Separate `FULL_STATE` builds print the final size, every element, final sum and
digest, so migration comparisons do not rely on a digest alone. GTI's separate
`ALLOCATION_COUNTS` mode links the existing allocation-observer fixture and
reports request count/bytes; its observer checks the matching region release
when the vector leaves scope. Neither mode belongs in runtime timing samples;
the default workload remains unchanged.

## `managed-thread-parallelism`

This schema-2 workload compares four deterministic, row-interleaved
Mandelbrot partitions, each rendered four times, in three GTI binaries. All are
built with the same
compiler, optimization level, C++ standard, native flags, and explicit
`concurrent` execution profile.
`gti-sequential` runs the four partitions in series;
`gti-four-threads` starts all four as owned `std::thread` tasks and joins
them; `gti-four-scoped` uses synchronous scoped tasks that borrow four distinct
result locals. Both parallel variants create native threads, not pooled jobs.

One work unit is one computed pixel in one pass, for 2,800,000 work units.
Every repeated partition validates its own checksum, and the parent publishes
a partition only after ordinary return or a successful join. All variants
must produce:

```text
GTI-BENCH-1 digest:000000097bfbb42c work-units:2800000
```

The sequential variant is the reference. The reciprocal of
each parallel variant's `median_ratio_to_reference` is the observed speedup.
The number is machine-specific and should be interpreted with the report's
CPU identity, raw samples, and variance; the benchmark demonstrates real
parallel execution, not a universal speedup guarantee. See the workload's
[`README.md`](workloads/managed-thread-parallelism/README.md) for its
safe-state and result-validation contract.

## Runtime-optimizer campaign controls

The runtime-optimizer campaign uses several independently attributable
workloads rather than one aggregate score:

- [`string-growth-search`](workloads/string-growth-search/README.md) covers
  repeated string growth, character search and complete-state comparison for
  the raw-buffer migration;
- [`checked-scalar-chain`](workloads/checked-scalar-chain/README.md) is the
  scalar checked-arithmetic control;
- [`fixed-array-loop-proof`](workloads/fixed-array-loop-proof/README.md) is the
  existing bounds-proof/no-op control;
- [`fixed-array-overflow`](workloads/fixed-array-overflow/README.md) exercises
  bounded checked arithmetic in a fixed-array loop;
- [`scalar-fp-update`](workloads/scalar-fp-update/README.md) is the strict
  floating-point and closed-method control;
- [`game-update-stage`](workloads/game-update-stage/README.md) is the initial
  representative scalar/container update stage;
- [`opaque-checked-update`](workloads/opaque-checked-update/README.md) isolates
  one runtime-opaque checked scalar recurrence and its exact source preflight
  proxy;
- [`optimizer-level-control`](workloads/optimizer-level-control/README.md)
  replaces the former manual optimizer example with a descriptor-backed
  fixed-native campaign;
- [`interface-exact-dispatch`](workloads/interface-exact-dispatch/README.md)
  exercises bounded exact local interface receivers, with a source proxy and
  complete-state output mode. The five-family selection probes are retained
  under [`probes/runtime-facilities`](probes/runtime-facilities/README.md).

Run `optimizer-level-control` once for each GTI optimization level from `0`
through `3` while holding `--native-optimization 3`. This keeps the native
compiler policy fixed and attributes any executable difference to GTI's MIR
policy.

## Reproduction

The repository runner is the canonical path because its JSON report records
the resolved GTI and native compiler identities, exact argument vectors, raw
samples, randomization seed, and normalized correctness record. For schema-2
GTI variants it requests a versioned `--build-report` sidecar and validates the
driver-owned record instead of reconstructing includes or toolchain layout.
That sidecar contains the exact `SourceLoader` graph and consumed source text,
the native depfile-discovered GTI runtime and compatibility headers, the exact
runtime archive, the native argument vector, and producer/compiler executable
hashes. Individual material records retain canonical paths. Aggregate digests
instead use normalized logical roles/names, flags, graph roots/edges, byte
counts, and content hashes so moving an otherwise identical source package or
toolchain does not change its aggregate identity:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 3 \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 1 \
  --workload optimizer-level-control \
  --output build/benchmarks \
  --json build/benchmarks/report.json \
  --markdown build/benchmarks/report.md
```

To measure managed-thread parallelism, select its descriptor instead:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 3 \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 1 \
  --workload managed-thread-parallelism \
  --output build/benchmarks \
  --json build/benchmarks/threading.json \
  --markdown build/benchmarks/threading.md
```

To attribute GTI optimizer changes independently of the native compiler, keep
the native level fixed explicitly. Repeat the command with GTI optimization
levels `0` through `3`, using a distinct output/report path for each run:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 0 \
  --native-optimization 3 \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 1 \
  --workload vector-checked-loop \
  --output build/benchmarks-fixed-native-o0 \
  --json build/benchmarks-fixed-native-o0/report.json \
  --markdown build/benchmarks-fixed-native-o0/report.md
```

To add the bounded arithmetic successful-path ceiling for one checked GTI
variant:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 0 \
  --native-optimization 3 \
  --arithmetic-success-proxy checked-scalar-chain/gti \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 1 \
  --workload checked-scalar-chain \
  --output build/benchmarks-arithmetic-ceiling \
  --json build/benchmarks-arithmetic-ceiling/report.json \
  --markdown build/benchmarks-arithmetic-ceiling/report.md
```

`--smoke` builds every variant and executes each once in a seeded order. That
validation execution is retained as the sole raw sample; smoke mode forces zero
warmups and does not apply minimum-duration or variance thresholds. Ordinary CI
uses this mode: an invalid build or digest fails, but a wall-clock comparison
never does. Non-smoke reports preserve every raw sample and label data
`inconclusive` when there are fewer than five samples, a median is shorter than
the descriptor's minimum, or a variant's sample coefficient of variation
exceeds `0.20`. The runner does not discard observations or manufacture a
result.

Schema-2 reports expose `exact_head_evidence.exact` as a mechanically derived
claim. It is true only when the repository has the same established commit
before and after measurement, both repository states are clean, every sidecar
has an established clean compiler build identity at that same commit, and
every sidecar producer SHA-256 equals the selected `--gti` executable SHA-256.
Missing facts, dirty state, commit drift or mismatch, inconsistent compiler
identities, and producer mismatch make the claim false with stable reason
codes. The identity compiled into `gti` is generated from commit plus worktree
state at build time. A packaged `BUILD_ID` may be cross-checked independently,
but does not establish this executable binding and is not used to turn the
claim true.

For a manual correctness check from the repository root:

```sh
mkdir -p build/benchmarks/manual
GTI_STDLIB_PATH="$PWD/stdlib" build/gti \
  benchmarks/workloads/vector-checked-loop/main.gti \
  -O3 --std c++23 --cxx /usr/bin/clang++ \
  -o build/benchmarks/manual/gti
/usr/bin/clang++ \
  benchmarks/workloads/vector-checked-loop/semantic.cpp \
  -O3 -std=c++23 -fno-fast-math -ffp-contract=off \
  -D__gti_strict_ieee754=1 \
  -o build/benchmarks/manual/cpp-semantic
/usr/bin/clang++ \
  benchmarks/workloads/vector-checked-loop/idiomatic.cpp \
  -O3 -std=c++23 -fno-fast-math -ffp-contract=off \
  -D__gti_strict_ieee754=1 \
  -o build/benchmarks/manual/cpp-idiomatic
build/benchmarks/manual/gti
build/benchmarks/manual/cpp-semantic
build/benchmarks/manual/cpp-idiomatic
```

To retain emitted C++ for code-generation and native vectorization evidence,
emit it beneath the output root and compile it with the same native flags:

```sh
GTI_STDLIB_PATH="$PWD/stdlib" build/gti \
  benchmarks/workloads/vector-checked-loop/main.gti \
  --emit-cpp -O3 --std c++23 \
  -o build/benchmarks/vector-checked-loop/main.cpp
```

After the canonical runner has built and validated the executables, Hyperfine
may be used as an optional independent timing cross-check. It is not required
by this repository and its output is not a canonical GTI benchmark report.
