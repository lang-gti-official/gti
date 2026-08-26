#include <cstdint>
#include <iomanip>
#include <iostream>

int main() {
  constexpr std::int32_t round_count = 20000;
  constexpr std::int32_t width_count = 5000;
  std::int64_t accumulator = 0;

  for (std::int32_t round = 0; round < round_count; ++round) {
    std::int32_t value = round % 13;
    for (std::int32_t index = 0; index < width_count; ++index) {
      value = (value * 3 + index) % 1000003;
      accumulator += value;
    }
  }

  const std::uint64_t digest = static_cast<std::uint64_t>(
      accumulator % static_cast<std::int64_t>(1000000007));
  if (digest != 0x2284c2c0) {
    return 1;
  }
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:100000000\n";
  return 0;
}
