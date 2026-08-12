#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

constexpr std::size_t kElementCount = 5000;
constexpr std::size_t kRoundCount = 20000;
constexpr std::uint64_t kDigestModulus = 1000000007;
constexpr std::uint64_t kExpectedDigest = 347609426;

void scale(std::vector<std::int32_t> &values, std::int32_t multiplier) {
  for (std::int32_t &value : values) {
    // Fixed workload inputs keep value * multiplier in [-17, 17].
    value *= multiplier;
  }
}

std::int32_t sum(const std::vector<std::int32_t> &values) {
  std::int32_t total = 0;
  for (const std::int32_t value : values) {
    // Every partial sum is in [-44,985, 44,985].
    total += value;
  }
  return total;
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:200000000\n";
}

} // namespace

int main() {
  std::vector<std::int32_t> values(kElementCount);
  for (std::size_t index = 0; index < values.size(); ++index) {
    values[index] = static_cast<std::int32_t>(index % 17) + 1;
  }

  std::uint64_t digest = 146959810;
  std::int32_t final_sum = 0;
  for (std::size_t round_index = 0; round_index < kRoundCount; ++round_index) {
    const std::int32_t multiplier = round_index % 2 == 0 ? -1 : 1;
    scale(values, multiplier);
    final_sum = sum(values);

    const std::uint64_t normalized_sum = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(final_sum) + 100000);
    // Each operand is bounded below uint64_t overflow before the modulo.
    digest = (digest * 131 + normalized_sum) % kDigestModulus;
    digest = (digest * 131 + round_index % 997) % kDigestModulus;
  }

  if (final_sum != 44985 || digest != kExpectedDigest) {
    return 1;
  }

  print_record(digest);
  return 0;
}
