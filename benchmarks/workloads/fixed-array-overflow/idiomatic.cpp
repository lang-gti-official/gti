#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

int main() {
  std::array<std::int32_t, 4096> cells{};
  for (std::int32_t index = 0; index < 4096; ++index) {
    cells[static_cast<std::size_t>(index)] = index % 97;
  }

  std::int64_t accumulator = 0;
  for (std::int32_t round = 0; round < 80000; ++round) {
    for (std::int32_t index = 0; index < 4096; ++index) {
      std::int32_t &cell = cells[static_cast<std::size_t>(index)];
      cell = (cell * 5 + 1) % 8191;
      accumulator += cell;
    }
  }

  const std::uint64_t digest = static_cast<std::uint64_t>(
      accumulator % static_cast<std::int64_t>(1000000007));
  if (digest != 0x34322aaf) {
    return 1;
  }
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:327680000\n";
  return 0;
}
