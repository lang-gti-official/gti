// Test-only per-toolchain codegen witnesses for the reviewed lane extension.
#include "llvm_atomic_wait_lane_contract.h"

#define GTI_TEST_LANE(Bits, Type)                                              \
  extern "C" std::uint64_t gti_test_lane_load##Bits##_relaxed(                 \
      const void *address) noexcept {                                          \
    return gti_test_wait_lane::load<Type>(address,                             \
                                          gti_test_wait_lane::relaxed);        \
  }                                                                            \
  extern "C" std::uint64_t gti_test_lane_load##Bits##_acquire(                 \
      const void *address) noexcept {                                          \
    return gti_test_wait_lane::load<Type>(address,                             \
                                          gti_test_wait_lane::acquire);        \
  }                                                                            \
  extern "C" std::uint64_t gti_test_lane_load##Bits##_sequential(              \
      const void *address) noexcept {                                          \
    return gti_test_wait_lane::load<Type>(address,                             \
                                          gti_test_wait_lane::sequential);     \
  }
GTI_TEST_LANE(8, std::uint8_t)
GTI_TEST_LANE(16, std::uint16_t)
GTI_TEST_LANE(32, std::uint32_t)
GTI_TEST_LANE(64, std::uint64_t)
#undef GTI_TEST_LANE
