#include <cstdint>

extern "C" std::int32_t gti_test_is_aligned(std::int32_t *value,
                                            std::uint64_t alignment) {
  return value != nullptr && alignment != 0 &&
                 reinterpret_cast<std::uintptr_t>(value) % alignment == 0
             ? 1
             : 0;
}
