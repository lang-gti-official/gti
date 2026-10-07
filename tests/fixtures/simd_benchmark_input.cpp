namespace {
volatile float seed = 1.25f;
volatile int iterations = 300000000;
} // namespace
extern "C" float simd_benchmark_seed() { return seed; }
extern "C" int simd_benchmark_iterations() { return iterations; }
