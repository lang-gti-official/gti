#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

constexpr std::int32_t kImageWidth = 1000;
constexpr std::int32_t kImageHeight = 700;
constexpr std::int32_t kMaximumIterations = 600;
constexpr std::uint64_t kExpectedChecksum = 10183699723;

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

std::uint64_t checked_multiply_u64(std::uint64_t left, std::uint64_t right) {
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right) {
    checked_failure();
  }
  return left * right;
}

std::int32_t escape_iterations(double coordinate_real,
                               double coordinate_imaginary) {
  double value_real = 0.0;
  double value_imaginary = 0.0;
  std::int32_t iteration = 0;
  while (iteration < kMaximumIterations) {
    const double real_squared = value_real * value_real;
    const double imaginary_squared = value_imaginary * value_imaginary;
    if (real_squared + imaginary_squared > 4.0) {
      break;
    }
    const double next_imaginary =
        2.0 * value_real * value_imaginary + coordinate_imaginary;
    value_real = real_squared - imaginary_squared + coordinate_real;
    value_imaginary = next_imaginary;
    iteration = checked_add_i32(iteration, 1);
  }
  return iteration;
}

std::uint64_t render_checksum() {
  std::uint64_t checksum = 0;
  for (std::int32_t pixel_y = 0; pixel_y < kImageHeight;
       pixel_y = checked_add_i32(pixel_y, 1)) {
    const double coordinate_imaginary =
        (static_cast<double>(pixel_y) / static_cast<double>(kImageHeight)) *
            2.4 -
        1.2;
    for (std::int32_t pixel_x = 0; pixel_x < kImageWidth;
         pixel_x = checked_add_i32(pixel_x, 1)) {
      const double coordinate_real =
          (static_cast<double>(pixel_x) / static_cast<double>(kImageWidth)) *
              3.5 -
          2.5;
      const std::int32_t iterations =
          escape_iterations(coordinate_real, coordinate_imaginary);
      const std::uint64_t column_weight =
          static_cast<std::uint64_t>(pixel_x % 251 + 1);
      const std::uint64_t row_tag =
          static_cast<std::uint64_t>(pixel_y % 257 + 1);
      checksum = checked_add_u64(
          checksum, checked_add_u64(checked_multiply_u64(
                                        static_cast<std::uint64_t>(iterations),
                                        column_weight),
                                    row_tag));
    }
  }
  return checksum;
}

} // namespace

int main() {
  const std::uint64_t checksum = render_checksum();
  if (checksum != kExpectedChecksum) {
    return 1;
  }
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << checksum << std::dec
            << " work-units:700000\n";
  return 0;
}
