# Fixed-array overflow

This is the admitted O-PERF-01 client. A 4,096-element update is bounded by
construction, but checked multiply/add side exits prevent the native compiler
from vectorizing the current GTI representation. The semantic C++ twin checks
the same operations in order, while the idiomatic twin omits checks only for
this fixed bounded input. The campaign's generated arithmetic ablation is an
opt-in runner artifact, not a source variant and never optimizer proof.

Expected record:

```text
GTI-BENCH-1 digest:0000000034322aaf work-units:327680000
```
