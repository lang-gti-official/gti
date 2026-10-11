#include <chrono>
#include <cstdint>
#include <cstdio>

namespace {
volatile std::uint64_t observed = 0;
}

extern "C" std::uint64_t forward_traversal_now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

extern "C" void forward_traversal_observe(std::uint64_t checksum) {
  observed = checksum;
}

extern "C" void forward_traversal_report(std::uint64_t length,
                                         std::uint64_t elapsed,
                                         std::uint64_t allocations,
                                         std::uint64_t checksum) {
  std::printf("%llu %llu %llu %llu\n", static_cast<unsigned long long>(length),
              static_cast<unsigned long long>(elapsed),
              static_cast<unsigned long long>(allocations),
              static_cast<unsigned long long>(checksum));
}
