# Runtime facility candidate probes

These bounded source probes preserve the investigation behind
`O-MEASURE-02`; they are not a new compiler subsystem or timing gate. The
canonical selected workload is `workloads/interface-exact-dispatch`.

Five candidates use the same 1,024-lane scalar recurrence, publishing **every
final lane**. Callback, interface, owner, and cross-unit probes use 120,000
ticks; SoA uses 240,000 so its fast C++ variant clears the duration floor.
Compare saved time per 1,024-lane tick, not raw durations across those scales.

| Candidate | Source proxy and retained semantics |
| --- | --- |
| Callback/accessor chain | Direct known scalar call, retaining construction and destruction of the same nonempty owning callback. |
| Interface helper | Direct known scalar call, retaining the same concrete receiver; tests the ceiling of interprocedural receiver propagation, not just the implemented local slice. `interface-local.gti` isolates that local slice. |
| Owner flow | Return the computed scalar directly but retain the allocation and destruction. GTI allocation failure is observable, so deleting the allocation would be an invalid proxy. |
| SoA | Expose the two distinct vector origins by placing the helper body at its caller; retain arithmetic and access checks. |
| Cross-translation-unit | ThinLTO on the native object and final link, with exactly the same source and checked arithmetic. No unchecked flags or undefined behavior. |

`reference.cpp` selects semantic-checking and idiomatic C++ variants. The
semantic twin represents the relevant arithmetic/bounds/allocation checks on
the declared successful input domain, not GTI's complete failure ABI. Native
allocation/callback representations differ; do not attribute those differences
to MIR alone. In particular, the idiomatic owner uses `std::make_unique`, which
C++ may eliminate entirely; the checking twin and GTI proxy retain the
allocation. The separate failure tests own GTI failure equivalence.

Build into a **new** external output directory, then measure with no concurrent
compiler/test work:

```sh
python3 benchmarks/probes/runtime-facilities/screen.py build \
  --gti /absolute/path/to/preserved-baseline-gti --output /tmp/gti-candidates
python3 benchmarks/probes/runtime-facilities/screen.py measure \
  --gti /absolute/path/to/preserved-baseline-gti --output /tmp/gti-candidates
```

This macOS investigation helper uses `/usr/bin/time -l` and `otool` to retain
peak memory and assembly, plus GTI's exact build reports, MIR, optimization
reports, source/executable hashes, complete output, and all five warmups and
twenty seeded interleaved samples. It invokes argument vectors, not shell
commands. Build timings from its parallel build phase are not comparative
compiler-latency evidence. The canonical performance plan records the machine,
compiler identities, interpretation, and remaining workload limitations.
