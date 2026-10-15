# Source-defined optional: scalar payload

The source and builtin variants execute the same 200-million-iteration signed
integer lookup loop, with an empty result every fourth iteration. Both validate
the complete accumulated sum before publishing the benchmark record. The only
source difference is the optional implementation. No allocation is required by
either scalar optional; normal output/runtime initialization is outside that
claim.

Use `scripts/benchmark_compare.py --gti <compiler> --cxx <native-compiler>
--optimization 2 --cpp-standard c++23 --warmup 3 --runs 15 --output <evidence>
--workload payload-optional-int`. Preserve the report's tool/source hashes and
interleaved samples. Run on an otherwise quiet host; sub-threshold or noisy
samples are not evidence of parity. The original unsigned pilot is not the
signed workload retained here.
