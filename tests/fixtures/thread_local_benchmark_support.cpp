#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
std::uint64_t allocations = 0;
std::uint64_t releases = 0;
} // namespace

extern "C" std::uint64_t gti_tls_bench_clock() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
extern "C" std::uint64_t gti_tls_bench_iterations() { return 20000000; }
extern "C" std::uint64_t gti_tls_bench_step(std::uint64_t iteration) {
  return 1 + (iteration & 1);
}
extern "C" void *gti_tls_bench_acquire() {
  void *result = std::malloc(64);
  if (result == nullptr)
    std::abort();
  ++allocations;
  return result;
}
extern "C" void gti_tls_bench_release(void *value) {
  ++releases;
  std::free(value);
}
extern "C" void gti_tls_bench_report(std::uint64_t warm, std::uint64_t cold,
                                     std::uint64_t value) {
  std::printf("%" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
              warm, cold, value, allocations, releases);
}
