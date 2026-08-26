#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

struct Vector3 {
  double x;
  double y;
  double z;

  double dot(const Vector3 &other) const {
    return x * other.x + y * other.y + z * other.z;
  }
};

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::int32_t checked_add_i32(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int32_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int32_t>::min() - right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left + right);
}

std::uint64_t checked_add_u64(std::uint64_t left, std::uint64_t right) {
  if (left > std::numeric_limits<std::uint64_t>::max() - right) {
    checked_failure();
  }
  return left + right;
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:30000000\n";
}

} // namespace

int main() {
  Vector3 left{1.0, 2.0, 3.0};
  const Vector3 right{0.5, 0.25, 0.125};
  double accumulator = 0.0;
  for (std::int32_t index = 0; index < 30000000;
       index = checked_add_i32(index, 1)) {
    left.x = left.x * 0.9999 + 0.0001;
    left.y = left.y * 0.9998 + 0.0002;
    left.z = left.z * 0.9997 + 0.0003;
    accumulator = accumulator + left.dot(right);
  }

  std::uint64_t digest = static_cast<std::uint64_t>(accumulator);
  digest =
      checked_add_u64(digest, static_cast<std::uint64_t>(left.x * 1000000.0)) %
      1000000007;
  digest =
      checked_add_u64(digest, static_cast<std::uint64_t>(left.y * 1000000.0)) %
      1000000007;
  digest =
      checked_add_u64(digest, static_cast<std::uint64_t>(left.z * 1000000.0)) %
      1000000007;
  if (digest != 0x1be59f2) {
    return 1;
  }
  print_record(digest);
  return 0;
}
