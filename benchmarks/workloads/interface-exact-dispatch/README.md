# Exact interface dispatch

This is a deterministic 1,024-lane update stage with 120,000 ticks, not a
whole-engine frame-rate claim. Each update applies `(value * 17 + bias) %
65521`, with `bias = tick % 127`. The interface receiver is an immutable,
complete local `Linear`, borrowed through `Update&`; its type never changes.
One work unit is one lane update (122,880,000 total).

The variants are:

- `main.gti`: ordinary checked GTI virtual call syntax;
- `proxy.gti`: a disposable source-level ceiling, retaining the object and
  reference but calling the known scalar implementation directly;
- `semantic.cpp`: the same virtual interface, bounds checks, and checked
  signed arithmetic for the admitted inputs;
- `idiomatic.cpp`: ordinary C++ arithmetic/indexing, safe for these fixed
  inputs but not an implementation of GTI's general failure guarantees.

All lanes remain in `[0, 65520]`, the product is at most 1,113,840, and the
sum is at most 1,113,966. No variant relies on undefined overflow. All lanes
feed the checked rolling digest, whose expected record is:

```text
GTI-BENCH-1 digest:000000001ca7a45b work-units:122880000
```

Define `FULL_STATE` to print every final lane instead of the record; this
provides byte-for-byte state comparison in addition to the normal harness
digest. The C++ twins are success-domain comparisons, not oracles for GTI's
failure ABI. `optimizer_devirtualization_runtime` separately compares exact
GTI failure diagnostics, partial output, and cleanup at O0/O2/O3 in C++20/23.

Run the canonical `scripts/benchmark_compare.py` with
`--workload interface-exact-dispatch --optimization 3 --native-optimization 3`
and at least five warmups and twenty interleaved repetitions. Compare an
unchanged baseline compiler and the new compiler using identical sources and
native flags. Keep compiler builds and test suites out of the timed interval.
Retain MIR/report, generated C++, disassembly, executable size, peak memory,
and raw timings. Report the bounded local-receiver scope: an interface passed
through an unknown formal parameter is intentionally not specialized.
