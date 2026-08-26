# Fixed-array overflow

This is the completed O-PERF-01 client. A 4,096-element update is bounded by
construction. Before O-PERF-01, checked arithmetic side exits prevented the
native compiler from vectorizing the GTI representation. The semantic C++ twin
checks the same operations in order, while the idiomatic twin omits checks only
for this fixed bounded input. The campaign's generated arithmetic ablation is
an opt-in runner artifact, not a source variant and never optimizer proof.

Exact clean commit `f1a4075f` discharges all five hot arithmetic outcomes and
all three hot fixed-array bounds outcomes through MIR proofs. Five warmups and
twenty seeded interleaved samples measured GTI at 1.011x idiomatic C++ at O2
and 0.999x at O3. The canonical report identities and raw samples are retained
in [`docs/plans/performance-tooling.md`](../../../docs/plans/performance-tooling.md).

Expected record:

```text
GTI-BENCH-1 digest:0000000034322aaf work-units:327680000
```
