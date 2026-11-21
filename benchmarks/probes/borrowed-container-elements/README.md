# Borrowed container allocation probe

These standalone programs build and traverse 4,096 path entries from two live
strings. `owned.gti` stores an owned copy per entry; `views.gti` stores shared
views into the two inputs. Both reserve the vector once, consume every entry's
length and first byte, and destroy all storage before ending measurement.
Their output digests must match. This measures repeated path storage, not a
complete file-indexing application. Borrowed views require their input strings
to remain alive; the owned version has no such requirement.

For ordinary optimized runtime measurements, compile each with LLVM `-O3` and
link `benchmarks/comparative/support.c` (compiled separately as C). Set
`BENCH_ROUNDS` to the same value for both and compare repeated, alternating runs.
Include input construction, vector allocation, entry construction, traversal
and destruction in the timed region. These probes are not registered in CI.

For allocation counts on macOS, compile `allocation_counter_macos.c` as a
separate dynamic library and link each executable against its absolute path.
Run with `DYLD_INSERT_LIBRARIES` set to that library's absolute path. Output is
`GTI-ALLOC-1 digest successful-allocation-calls free-calls requested-bytes`.
The counter covers malloc, calloc, realloc and posix_memalign on the executing
thread inside the measured region; it is not a heap profiler or peak-memory
measurement. Reallocation counts as an allocation call, not a free call.

Confirm interposition with an independently compiled C allocation control
before trusting counts. Use the ordinary timing support for runtime results:
the counter itself changes execution cost. No installation or runtime library
modification is needed.

`allocation_control.c` is the independent control. Its output must be
`GTI-ALLOC-1 7 1 1 123`: one successful allocation, one free, and 123 requested
bytes. A zero count means interposition is not working.

## Observed allocation results

macOS arm64, LLVM 23.1.2, local borrowed-container snapshot
`951a9f533f11021f25502d0ff082a114ff9c402d30432a724b9a5590c8b54df1`:

| Program | LLVM modes | Digest | Allocations | Frees | Requested bytes |
| --- | --- | ---: | ---: | ---: | ---: |
| Owned copies | O0 and O3 | 385024 | 4099 | 4099 | 491616 |
| Borrowed views | O0 and O3 | 385024 | 3 | 3 | 65632 |

The independent control passed before these runs. The three view allocations
are the two input strings and reserved vector storage. The owned version adds
one allocation per copied string. These are allocation observations for this
workload, not runtime timing or a general promise about every container.

Use `run.py /absolute/path/to/gti --output /tmp/borrowed-container-timing`
from this directory for a standalone timing comparison. It builds both programs
with ordinary timing support, calibrates one shared iteration count, alternates
execution order, checks matching digests, and saves raw samples and build costs
in `evidence.json`. Run it on an otherwise idle machine; allocation counter
interposition is explicitly disabled for these timing runs.

## Observed timing result

The final shared-borrowed-storage candidate (snapshot
`951a9f533f11021f25502d0ff082a114ff9c402d30432a724b9a5590c8b54df1`)
measured 2.834838 seconds for owned copies and 0.118797 seconds for borrowed
views, using 34,085 rounds per sample and 15 alternating samples. Digests
matched (`13123543040`). Views took 4.19% of the owned-copy time for this
workload. Allocation interposition was disabled. An unrelated language-server
process occupied one core; this is not an idle-machine or universal
language-speed claim. These ownership contracts differ as described above.
