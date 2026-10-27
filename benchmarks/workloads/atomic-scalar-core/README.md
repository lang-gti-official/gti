# Scalar atomic core

Each of 20 million iterations performs five operations on a uint32 atomic:
one relaxed store, one relaxed load, an acquire-release exchange, a successful
strong CAS (acquire-release/acquire), and a failed strong CAS (relaxed/relaxed).
The final load is sequentially consistent. Every observed value and CAS outcome
is checked. The fixed loop bound keeps ordinary arithmetic in range on both
implementations. The deterministic result record must match before timing.

This is uncontended synchronized scalar cost, not a threading speedup, fetch
arithmetic, contention, blocked waiting, fairness or lock-freedom measurement.
Inspect the emitted loop to ensure the intended atomic operations survive; a
correct constant result alone is insufficient benchmark evidence.

The existing source/C++ comparison harness can validate the descriptor:

```sh
python3 scripts/benchmark_compare.py --gti /path/to/optimized/gti \
  --cxx clang++ --optimization 3 --workload atomic-scalar-core \
  --output /tmp/atomic-core-cpp-smoke --smoke
```

That harness currently selects the C++ backend, not native LLVM. Native LLVM
acceptance must compile this same `main.gti` explicitly with `--backend llvm
--execution-profile concurrent -O3`; never label a C++ harness result as native
evidence. General native-harness integration is a separate shared dependency.
No timing thresholds or common harness policy are changed by this workload.

On the component's current base, the complete native workload rejects the
pending hosted stdout runtime declaration. `native_probe.gti` executes the exact
same shared `kernel.gti` and its observed-value checks without that output-only
dependency. It is a correctness/emitted-operation probe, not a benchmark variant
or acceptance of the full output-record workload. Retain that full-workload
failure until hosted service composition enables it; do not substitute probe
timings for matched whole-program measurements.

For a quiet-host native comparison, retain exact source/compiler/target
identities, commands and build costs, interleaved raw samples/distributions,
operation count/order, executable text/total size and allocation evidence.
Match effective target features, not only the machine name: host C++ may use
AArch64 LSE while a generic native LLVM target uses exclusive-load/store loops.
There are no explicit source allocations in the loop, but that alone does not
prove zero runtime allocations. Do not make a cost or speedup claim from shared
build-host samples or from correctness-only runs.
