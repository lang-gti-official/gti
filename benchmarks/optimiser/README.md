# Optimiser runtime workload

`main.gti` is a deterministic, branch-heavy Mandelbrot escape-time workload
for comparing the runtime of binaries built with GTI's `-O0` through `-O3`
levels. It evaluates 700,000 complex-plane samples with a maximum of 600
iterations each, folds every result into a checked `uint64_t` checksum, prints
one stable record, and exits nonzero if that checksum changes.

This is a convenient Hyperfine campaign for comparing optimization levels,
not a replacement for the canonical variant-comparison harness documented in
the parent [`benchmarks/README.md`](../README.md). The repository harness also
records correctness, compiler identity, source and emitted-code digests,
randomized samples, and complete reproduction metadata.

## Build the four binaries

From the repository root, build GTI first, then compile the workload once per
optimization level. Keep the selected native compiler and C++ standard the
same across all four commands:

```sh
cmake --build build --target gti
mkdir -p build/benchmarks/optimiser

build/gti benchmarks/optimiser/main.gti -O0 --std c++23 \
  -o build/benchmarks/optimiser/mandelbrot-O0
build/gti benchmarks/optimiser/main.gti -O1 --std c++23 \
  -o build/benchmarks/optimiser/mandelbrot-O1
build/gti benchmarks/optimiser/main.gti -O2 --std c++23 \
  -o build/benchmarks/optimiser/mandelbrot-O2
build/gti benchmarks/optimiser/main.gti -O3 --std c++23 \
  -o build/benchmarks/optimiser/mandelbrot-O3
```

Run each executable once before timing it. All four must print the identical
record and return zero:

```sh
build/benchmarks/optimiser/mandelbrot-O0
build/benchmarks/optimiser/mandelbrot-O1
build/benchmarks/optimiser/mandelbrot-O2
build/benchmarks/optimiser/mandelbrot-O3
```

## Compare with Hyperfine

```sh
hyperfine --warmup 2 --runs 10 \
  --export-markdown build/benchmarks/optimiser/hyperfine.md \
  --export-json build/benchmarks/optimiser/hyperfine.json \
  'build/benchmarks/optimiser/mandelbrot-O0' \
  'build/benchmarks/optimiser/mandelbrot-O1' \
  'build/benchmarks/optimiser/mandelbrot-O2' \
  'build/benchmarks/optimiser/mandelbrot-O3'
```

Compile before starting Hyperfine: these commands compare program runtime,
not compiler runtime. GTI's optimization flag selects both its verified MIR
policy and the matching native optimization level. At present, `-O2` and
`-O3` use the same GTI middle-end pass set but still request distinct native
`-O2` and `-O3` policies.

For less noisy results, close CPU-heavy applications, keep the machine on
power, avoid thermal throttling, and increase `--warmup`/`--runs` when the
initial comparison is stable. Generated binaries and Hyperfine reports belong
under `build/`; do not write them beside the benchmark source.
