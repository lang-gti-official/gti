#include <cstdint>
#include <limits>

extern "C" void opaque_wide_store(std::uint64_t *target) {
  *target = std::numeric_limits<std::uint64_t>::max();
}
