#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::int32_t checked_add_i32(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int32_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int32_t>::min() - right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left + right);
}

std::int64_t checked_add_i64(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    checked_failure();
  }
  return left + right;
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

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:100000000\n";
}

} // namespace

int main() {
  constexpr std::int32_t round_count = 20000;
  constexpr std::int32_t width_count = 5000;
  std::int64_t accumulator = 0;

  for (std::int32_t round = 0; round < round_count;
       round = checked_add_i32(round, 1)) {
    std::int32_t value = checked_remainder_i32(round, 13);
    for (std::int32_t index = 0; index < width_count;
         index = checked_add_i32(index, 1)) {
      value = checked_remainder_i32(
          checked_add_i32(checked_multiply_i32(value, 3), index), 1000003);
      accumulator = checked_add_i64(accumulator, value);
    }
  }

  const std::uint64_t digest = static_cast<std::uint64_t>(
      accumulator % static_cast<std::int64_t>(1000000007));
  if (digest != 0x2284c2c0) {
    return 1;
  }
  print_record(digest);
  return 0;
}
