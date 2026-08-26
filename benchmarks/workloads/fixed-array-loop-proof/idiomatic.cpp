#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

constexpr std::size_t kExtent = 64;
constexpr std::int32_t kRoundBase = 599999;
constexpr std::uint64_t kDigestModulus = 1000000007;
constexpr std::uint64_t kExpectedDigest = 618920753;

std::uint64_t scan(std::array<std::int32_t, kExtent> values,
                   std::int32_t rounds) {
  std::uint64_t digest = 146959810;
  for (std::int32_t round = 0; round < rounds; ++round) {
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(kExtent);
         ++index) {
      const std::int32_t mixed =
          (values[static_cast<std::size_t>(index)] * 17 + round % 251 + index) %
          251;
      values[static_cast<std::size_t>(index)] = mixed;
      digest =
          (digest * 131 + static_cast<std::uint64_t>(mixed)) % kDigestModulus;
    }
  }
  return digest;
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:38400000\n";
}

} // namespace

int main(int argc, char **) {
  std::array<std::int32_t, kExtent> values{};
  for (std::int32_t index = 0; index < static_cast<std::int32_t>(kExtent);
       ++index) {
    values[static_cast<std::size_t>(index)] = (index * 17 + argc + 2) % 251;
  }

  const std::uint64_t digest = scan(values, kRoundBase + argc);
  if (digest != kExpectedDigest) {
    return 1;
  }
  print_record(digest);
  return 0;
}
