# Managed-thread parallelism

This workload compares the same 2,800,000-pixel-visit Mandelbrot computation
in two GTI binaries built at the same optimization level, C++ standard, native
compiler, and `concurrent` execution profile:

- `gti-sequential` evaluates four interleaved 175-row partitions, four passes
  each, one after another;
- `gti-four-jthreads` starts four owned `std::jthread` tasks, then joins them.

One work unit is one pixel whose escape count contributes to its partition
checksum in one of four complete passes. Rows are distributed by `row % 4`,
which keeps the two iteration-heavy center bands balanced across workers. Both
variants validate the same four checked-in repeated-partition checksums and
publish exactly:

```text
GTI-BENCH-1 digest:000000097bfbb42c work-units:2800000
```

The first managed-thread slice has no shared mutable state and no task-result
channel. Each worker therefore computes and validates its checksum privately.
A mismatch raises a defined failure inside that worker, and `join()` re-raises
it in the parent. The parent adds a partition checksum to the published digest
only after that worker joins successfully. This keeps the benchmark in safe
GTI and prevents a successful record from hiding missing or incorrect work.

The descriptor declares a conservative `minimum_sample_seconds` of `0.10`.
Work was increased by four complete deterministic passes; an exploratory run
measured the fastest threaded variant at about 225 ms, leaving more than twice
the declared floor. That exploratory timing only validates workload size and
is not exact-head campaign evidence.

The sequential variant is the report reference, so the threaded variant's
`median_ratio_to_reference` is elapsed threaded time divided by elapsed
sequential time. Its reciprocal is the observed speedup. Results depend on
available physical cores, system load, power policy, and thermal state; a
single-core or heavily constrained machine is not expected to improve.

The canonical runner command is documented in
[`benchmarks/README.md`](../../README.md). Hyperfine can independently
cross-check already-built binaries without becoming a repository dependency:

```sh
mkdir -p build/benchmarks/managed-thread-parallelism

build/gti \
  benchmarks/workloads/managed-thread-parallelism/sequential.gti \
  --execution-profile concurrent -O3 --std c++23 \
  -o build/benchmarks/managed-thread-parallelism/sequential
build/gti \
  benchmarks/workloads/managed-thread-parallelism/threaded.gti \
  --execution-profile concurrent -O3 --std c++23 \
  -o build/benchmarks/managed-thread-parallelism/threaded

build/benchmarks/managed-thread-parallelism/sequential
build/benchmarks/managed-thread-parallelism/threaded

hyperfine --warmup 5 --runs 20 \
  --export-json build/benchmarks/managed-thread-parallelism/hyperfine.json \
  --export-markdown build/benchmarks/managed-thread-parallelism/hyperfine.md \
  'build/benchmarks/managed-thread-parallelism/sequential' \
  'build/benchmarks/managed-thread-parallelism/threaded'
```

Both validation commands must print the exact record above before timing.
