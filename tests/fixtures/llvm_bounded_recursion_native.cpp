#include <cstdint>
#include <cstdlib>
extern "C" std::uint64_t recursion_input() {
  return std::strtoull(std::getenv("GTI_RECURSION_INPUT"), nullptr, 10);
}
extern "C" std::int32_t recursion_depth() {
  return std::atoi(std::getenv("GTI_RECURSION_DEPTH"));
}
