# Optimiser runtime workload

`main.gti` is a deterministic, branch-heavy Mandelbrot escape-time workload
for comparing the runtime of binaries built with GTI's `-O0` through `-O3`
levels. It evaluates 700,000 complex-plane samples with a maximum of 600
iterations each, folds every result into a checked `uint64_t` checksum, prints
one stable record, and exits nonzero if that checksum changes.

This is a convenient Hyperfine target, not a replacement for the canonical
comparison harness under [`benchmarks/`](../../benchmarks/README.md). The
repository harness also compares GTI with semantically equivalent and
idiomatic C++ variants and records reproducibility metadata.

## Build the four binaries

From the repository root, build GTI first, then compile the workload once per
optimization level. Keep the selected native compiler and C++ standard the
same across all four commands:

```sh
cmake --build build --target gti
mkdir -p build/examples/optimiser

build/gti examples/optimiser/main.gti -O0 --std c++23 \
  -o build/examples/optimiser/mandelbrot-O0
build/gti examples/optimiser/main.gti -O1 --std c++23 \
  -o build/examples/optimiser/mandelbrot-O1
build/gti examples/optimiser/main.gti -O2 --std c++23 \
  -o build/examples/optimiser/mandelbrot-O2
build/gti examples/optimiser/main.gti -O3 --std c++23 \
  -o build/examples/optimiser/mandelbrot-O3
```

Run each executable once before timing it. All four must print the identical
record and return zero:

```sh
build/examples/optimiser/mandelbrot-O0
build/examples/optimiser/mandelbrot-O1
build/examples/optimiser/mandelbrot-O2
build/examples/optimiser/mandelbrot-O3
```

## Compare with Hyperfine

```sh
hyperfine --warmup 2 --runs 10 \
  --export-markdown build/examples/optimiser/hyperfine.md \
  --export-json build/examples/optimiser/hyperfine.json \
  'build/examples/optimiser/mandelbrot-O0' \
  'build/examples/optimiser/mandelbrot-O1' \
  'build/examples/optimiser/mandelbrot-O2' \
  'build/examples/optimiser/mandelbrot-O3'
```

Compile before starting Hyperfine: these commands compare program runtime, not
compiler runtime. GTI's optimization flag selects both its verified MIR policy
and the matching native optimization level. At present, `-O2` and `-O3` use
the same GTI middle-end pass set but still request distinct native `-O2` and
`-O3` policies.

For less noisy results, close CPU-heavy applications, keep the machine on
power, avoid thermal throttling, and increase `--warmup`/`--runs` when the
initial comparison is stable. Generated binaries and Hyperfine reports belong
under `build/`; do not write them beside the example source.
