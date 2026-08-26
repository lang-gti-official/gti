# Fixed-array loop proof workload

This workload is the reproducible client from the O-MIR-08 measured audit. It
performs 38.4 million reads and writes through a 64-element fixed array while
updating a checked digest. `argc` keeps the trip count and initial values opaque
to compile-time constant folding; the benchmark runner invokes every variant
with the normal single program-name argument.

At the audited exact-main revision, GTI's interval solver widens the loop-carried
index before reaching 64 and retains both hot bounds checks. The equivalent
4-element loop converges before widening and discharges its check. This is a
MIR proof-availability near miss, not evidence of a runtime opportunity by
itself: Apple Clang 21 removes the retained bounds paths at native `-O3`.

Reproduce the workload with:

```sh
python3 scripts/benchmark_compare.py \
  --gti build/gti \
  --cxx /usr/bin/clang++ \
  --optimization 3 \
  --cpp-standard c++23 \
  --warmup 5 \
  --runs 20 \
  --seed 3108 \
  --timeout 180 \
  --workload fixed-array-loop-proof \
  --output build/benchmark-fixed-array-loop-proof \
  --json build/benchmark-fixed-array-loop-proof/report.json \
  --markdown build/benchmark-fixed-array-loop-proof/report.md
```

Generate proof and MIR evidence separately:

```sh
build/gti benchmarks/workloads/fixed-array-loop-proof/main.gti \
  -O3 \
  --emit-cpp \
  -o build/fixed-array-loop-proof.cpp \
  --dump-mir-after build/fixed-array-loop-proof.mir \
  --optimization-report build/fixed-array-loop-proof-optimization.json
```
