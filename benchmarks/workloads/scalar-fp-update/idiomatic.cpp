#include <cstdint>
#include <iomanip>
#include <iostream>

struct Vector3 {
  double x;
  double y;
  double z;

  double dot(const Vector3 &other) const {
    return x * other.x + y * other.y + z * other.z;
  }
};

int main() {
  Vector3 left{1.0, 2.0, 3.0};
  const Vector3 right{0.5, 0.25, 0.125};
  double accumulator = 0.0;
  for (std::int32_t index = 0; index < 30000000; ++index) {
    left.x = left.x * 0.9999 + 0.0001;
    left.y = left.y * 0.9998 + 0.0002;
    left.z = left.z * 0.9997 + 0.0003;
    accumulator = accumulator + left.dot(right);
  }

  std::uint64_t digest = static_cast<std::uint64_t>(accumulator);
  digest =
      (digest + static_cast<std::uint64_t>(left.x * 1000000.0)) % 1000000007;
  digest =
      (digest + static_cast<std::uint64_t>(left.y * 1000000.0)) % 1000000007;
  digest =
      (digest + static_cast<std::uint64_t>(left.z * 1000000.0)) % 1000000007;
  if (digest != 0x1be59f2) {
    return 1;
  }
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:30000000\n";
  return 0;
}
