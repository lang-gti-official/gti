#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>

namespace {

[[noreturn]] void checked_failure() noexcept { std::abort(); }

std::int32_t checked_add(std::int32_t left, std::int32_t right) {
  if ((right > 0 && left > std::numeric_limits<std::int32_t>::max() - right) ||
      (right < 0 && left < std::numeric_limits<std::int32_t>::min() - right)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left + right);
}

std::int32_t checked_increment(std::int32_t value) {
  return checked_add(value, std::int32_t{1});
}

std::int32_t checked_remainder(std::int32_t left, std::int32_t right) {
  if (right == 0 ||
      (left == std::numeric_limits<std::int32_t>::min() && right == -1)) {
    checked_failure();
  }
  return static_cast<std::int32_t>(left % right);
}

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:200000000\n";
}

} // namespace

int main(int argc, char **) {
  std::int32_t value = argc;
  for (std::int32_t round = 0; round < 50000000;
       round = checked_increment(round)) {
    value = checked_remainder(checked_add(value, argc), 1000003);
    value = checked_remainder(checked_add(value, argc), 1000003);
    value = checked_remainder(checked_add(value, argc), 1000003);
    value = checked_remainder(checked_add(value, argc), 1000003);
  }
  const auto digest = static_cast<std::uint64_t>(value);
  print_record(digest);
  return digest == 999404 ? 0 : 1;
}
