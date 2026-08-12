#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

namespace {

constexpr std::size_t kElementCount = 5000;
constexpr std::size_t kRoundCount = 20000;
constexpr std::uint64_t kDigestModulus = 1000000007;
constexpr std::uint64_t kExpectedDigest = 347609426;

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::size_t checked_add_size(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    checked_failure();
  }
  return left + right;
}

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

std::int64_t checked_add_i64(std::int64_t left, std::int64_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
    checked_failure();
  }
  return left + right;
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

std::int32_t checked_to_i32(std::size_t value) {
  if (value >
      static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    checked_failure();
  }
  return static_cast<std::int32_t>(value);
}

std::uint64_t checked_to_u64(std::int64_t value) {
  if (value < 0) {
    checked_failure();
  }
  return static_cast<std::uint64_t>(value);
}

std::int32_t &checked_at(std::vector<std::int32_t> &values, std::size_t index) {
  if (index >= values.size()) {
    checked_failure();
  }
  return values[index];
}

const std::int32_t &checked_at(const std::vector<std::int32_t> &values,
                               std::size_t index) {
  if (index >= values.size()) {
    checked_failure();
  }
  return values[index];
}

void scale(std::vector<std::int32_t> &values, std::int32_t multiplier) {
  for (std::size_t index = 0; index < values.size();
       index = checked_add_size(index, 1)) {
    std::int32_t &value = checked_at(values, index);
    value = checked_multiply_i32(value, multiplier);
  }
}

std::int32_t sum(const std::vector<std::int32_t> &values) {
  std::int32_t total = 0;
  for (std::size_t index = 0; index < values.size();
       index = checked_add_size(index, 1)) {
    total = checked_add_i32(total, checked_at(values, index));
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
  // The size constructor establishes 5,000 initialized logical elements,
  // matching GTI's source-defined vector size constructor.
  std::vector<std::int32_t> values(kElementCount, std::int32_t{0});
  for (std::size_t index = 0; index < kElementCount;
       index = checked_add_size(index, 1)) {
    checked_at(values, index) = checked_add_i32(checked_to_i32(index % 17), 1);
  }

  std::uint64_t digest = 146959810;
  std::int32_t final_sum = 0;
  for (std::size_t round_index = 0; round_index < kRoundCount;
       round_index = checked_add_size(round_index, 1)) {
    const std::int32_t multiplier = round_index % 2 == 0 ? -1 : 1;
    scale(values, multiplier);
    final_sum = sum(values);

    const std::int64_t shifted_sum =
        checked_add_i64(static_cast<std::int64_t>(final_sum), 100000);
    const std::uint64_t normalized_sum = checked_to_u64(shifted_sum);
    digest =
        checked_add_u64(checked_multiply_u64(digest, 131), normalized_sum) %
        kDigestModulus;
    digest =
        checked_add_u64(checked_multiply_u64(digest, 131), round_index % 997) %
        kDigestModulus;
  }

  if (final_sum != 44985 || digest != kExpectedDigest) {
    return 1;
  }

  print_record(digest);
  return 0;
}
