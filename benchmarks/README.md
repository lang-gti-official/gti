# GTI benchmarks

This directory contains deterministic whole-program workloads for comparing
GTI with native C++ under one compiler and optimization policy. Benchmarks are
correctness experiments first: the runner builds every declared variant,
requires one identical `GTI-BENCH-1` result record, and only then collects
timings. Shared CI may validate builds and records, but must not enforce a wall
time threshold.

## Workloads and variants

Each directory under `workloads/` contains a schema-1 `benchmark.json` and the
sources named by its `variants` array. Schema 1 deliberately contains only
structured data; descriptors cannot run shell commands, interpolate the
environment, or select output paths.

The normal variants are:

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
then performs 20,000 rounds. Every round scales all elements by a varying
`-1`/`1` multiplier and sums all elements from left to right.

One logical work unit is one element visit in a timed `scale` or `sum` pass.
Setup is excluded, so the workload performs
`5,000 * 20,000 * 2 = 200,000,000` work units. Each round's checked sum feeds a
bounded rolling digest; the final expected record is:

```text
GTI-BENCH-1 digest:0000000014b81952 work-units:200000000
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

## Reproduction

The repository runner is the canonical path because its JSON report records
the resolved GTI and native compiler identities, exact argument vectors, raw
samples, source digests, randomization seed, and normalized correctness record:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 3 \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 1 \
  --workload vector-checked-loop \
  --output build/benchmarks \
  --json build/benchmarks/report.json \
  --markdown build/benchmarks/report.md
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
