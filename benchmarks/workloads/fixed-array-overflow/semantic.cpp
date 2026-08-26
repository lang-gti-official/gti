#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

constexpr std::size_t kExtent = 4096;

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

std::int32_t &checked_at(std::array<std::int32_t, kExtent> &cells,
                         std::int32_t index) {
  if (index < 0 || static_cast<std::size_t>(index) >= cells.size()) {
    checked_failure();
  }
  return cells[static_cast<std::size_t>(index)];
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:327680000\n";
}

} // namespace

int main() {
  std::array<std::int32_t, kExtent> cells{};
  for (std::int32_t index = 0; index < static_cast<std::int32_t>(kExtent);
       index = checked_add_i32(index, 1)) {
    checked_at(cells, index) = checked_remainder_i32(index, 97);
  }

  std::int64_t accumulator = 0;
  for (std::int32_t round = 0; round < 80000;
       round = checked_add_i32(round, 1)) {
    for (std::int32_t index = 0; index < static_cast<std::int32_t>(kExtent);
         index = checked_add_i32(index, 1)) {
      std::int32_t &cell = checked_at(cells, index);
      cell = checked_remainder_i32(
          checked_add_i32(checked_multiply_i32(cell, 5), 1), 8191);
      accumulator = checked_add_i64(accumulator, cell);
    }
  }

  const std::uint64_t digest = static_cast<std::uint64_t>(
      accumulator % static_cast<std::int64_t>(1000000007));
  if (digest != 0x34322aaf) {
    return 1;
  }
  print_record(digest);
  return 0;
}
