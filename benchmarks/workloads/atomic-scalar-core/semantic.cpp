#include <atomic>
#include <cstdint>
#include <cstdio>

int main() {
  std::atomic<std::uint32_t> value{0};
  // The fixed bound proves every index/input addition fits uint32_t, matching
  // GTI's successful checked-arithmetic contract without a wrapping workload.
  for (std::uint32_t index = 0; index < 20000000; ++index) {
    const std::uint32_t input = index + 1;
    value.store(input, std::memory_order_relaxed);
    if (value.load(std::memory_order_relaxed) != input)
      return 1;
    const auto exchanged = value.exchange(input + 1, std::memory_order_acq_rel);
    if (exchanged != input)
      return 2;
    auto observed = input + 1;
    const bool changed = value.compare_exchange_strong(
        observed, input + 2, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (!changed || observed != input + 1)
      return 3;
    observed = input + 1;
    const bool rejected = value.compare_exchange_strong(
        observed, input + 3, std::memory_order_relaxed,
        std::memory_order_relaxed);
    if (rejected || observed != input + 2)
      return 4;
  }
  if (value.load() != 20000002)
    return 5;
  std::puts("GTI-BENCH-1 digest:0000000001312d02 work-units:20000000");
  return 0;
}
