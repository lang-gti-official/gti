#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

constexpr std::size_t kExtent = 64;
constexpr std::int32_t kRoundBase = 599999;
constexpr std::uint64_t kDigestModulus = 1000000007;
constexpr std::uint64_t kExpectedDigest = 618920753;

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::int32_t checked_add_i32(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int32_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int32_t>::min() - right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left + right);
}

std::int32_t checked_multiply_i32(std::int32_t left, std::int32_t right) {
  const std::int64_t product =
      static_cast<std::int64_t>(left) * static_cast<std::int64_t>(right);
  if (product < std::numeric_limits<std::int32_t>::min() ||
      product > std::numeric_limits<std::int32_t>::max()) {
    checked_failure();
  }
  return static_cast<std::int32_t>(product);
}

std::int32_t checked_remainder_i32(std::int32_t left, std::int32_t right) {
  if (right == 0 ||
      (left == std::numeric_limits<std::int32_t>::min() && right == -1)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left % right);
}

std::uint64_t checked_add_u64(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    checked_failure();
  }
  return left + right;
}

std::uint64_t checked_multiply_u64(std::uint64_t left, std::uint64_t right) {
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
    checked_failure();
  }
  return left * right;
}

std::uint64_t checked_to_u64(std::int32_t value) {
  if (value < 0) {
    checked_failure();
  }
  return static_cast<std::uint64_t>(value);
}

std::int32_t &checked_at(std::array<std::int32_t, kExtent> &values,
                         std::int32_t index) {
  if (index < 0 || static_cast<std::uint64_t>(index) >= values.size()) {
    checked_failure();
  }
  return values[static_cast<std::size_t>(index)];
}

std::uint64_t scan(std::array<std::int32_t, kExtent> values,
                   std::int32_t rounds) {
  std::uint64_t digest = 146959810;
  for (std::int32_t round = 0; round < rounds;
       round = checked_add_i32(round, 1)) {
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(kExtent);
         index = checked_add_i32(index, 1)) {
      const std::int32_t mixed = checked_remainder_i32(
          checked_add_i32(checked_add_i32(checked_multiply_i32(
                                              checked_at(values, index), 17),
                                          checked_remainder_i32(round, 251)),
                          index),
          251);
      checked_at(values, index) = mixed;
      digest = checked_add_u64(checked_multiply_u64(digest, 131),
                               checked_to_u64(mixed)) %
               kDigestModulus;
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
       index = checked_add_i32(index, 1)) {
    checked_at(values, index) = checked_remainder_i32(
        checked_add_i32(checked_add_i32(checked_multiply_i32(index, 17), argc),
                        2),
        251);
  }

  const std::uint64_t digest = scan(values, checked_add_i32(kRoundBase, argc));
  if (digest != kExpectedDigest) {
    return 1;
  }
  print_record(digest);
  return 0;
}
