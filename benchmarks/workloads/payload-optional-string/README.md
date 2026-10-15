# Source-defined optional: allocating payload

The source and builtin variants execute the same ten-million-iteration lookup
loop, with an empty result every fourth iteration. Each present result owns one
61-byte string; the full accumulated length is checked before the benchmark
record is printed. This exercises construction, observation, borrowing and
cleanup without consuming extraction. Native allocation counters must be used
separately when making measured allocation-count claims; elapsed time alone is
not allocation evidence.

Run with `scripts/benchmark_compare.py --gti <compiler> --cxx <native-compiler>
--optimization 2 --cpp-standard c++23 --warmup 3 --runs 15 --output <evidence>
--workload payload-optional-string`. The harness records exact compiler/source
identities, native build commands, executable sizes and interleaved samples.
Use an otherwise quiet host and retain raw samples, not only minima.
