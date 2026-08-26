#include <cstdint>
#include <iomanip>
#include <iostream>

namespace {

void print_record(std::uint64_t digest) {
  std::cout << "GTI-BENCH-1 digest:" << std::hex << std::nouppercase
            << std::setw(16) << std::setfill('0') << digest << std::dec
            << " work-units:200000000\n";
}

} // namespace

int main(int argc, char **) {
  std::int32_t value = argc;
  for (std::int32_t round = 0; round < 50000000; ++round) {
    value = (value + argc) % 1000003;
    value = (value + argc) % 1000003;
    value = (value + argc) % 1000003;
    value = (value + argc) % 1000003;
  }
  const auto digest = static_cast<std::uint64_t>(value);
  print_record(digest);
  return digest == 999404 ? 0 : 1;
}
