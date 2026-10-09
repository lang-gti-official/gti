#include <cstdint>
#include <limits>

extern "C" void opaque_pointer_effect(std::int32_t *pointer) {
  *pointer = std::numeric_limits<std::int32_t>::max();
}
