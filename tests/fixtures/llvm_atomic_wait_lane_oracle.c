/* Test-only raw lane oracle. This is not a public atomic object ABI. */
#include <stdint.h>
#include <stdlib.h>

struct lanes {
  int8_t i8;
  uint8_t u8;
  int16_t i16;
  uint16_t u16;
  int32_t i32;
  uint32_t u32;
  int64_t i64;
  uint64_t u64;
  uint8_t boolean_byte;
};

static struct lanes objects;

void *gti_test_lane_initialize(unsigned index, uint64_t bits) {
  switch (index) {
  case 0:
    __atomic_store_n(&objects.i8, (int8_t)bits, __ATOMIC_RELEASE);
    return &objects.i8;
  case 1:
    __atomic_store_n(&objects.u8, (uint8_t)bits, __ATOMIC_RELEASE);
    return &objects.u8;
  case 2:
    __atomic_store_n(&objects.i16, (int16_t)bits, __ATOMIC_RELEASE);
    return &objects.i16;
  case 3:
    __atomic_store_n(&objects.u16, (uint16_t)bits, __ATOMIC_RELEASE);
    return &objects.u16;
  case 4:
    __atomic_store_n(&objects.i32, (int32_t)bits, __ATOMIC_RELEASE);
    return &objects.i32;
  case 5:
    __atomic_store_n(&objects.u32, (uint32_t)bits, __ATOMIC_RELEASE);
    return &objects.u32;
  case 6:
    __atomic_store_n(&objects.i64, (int64_t)bits, __ATOMIC_RELEASE);
    return &objects.i64;
  case 7:
    __atomic_store_n(&objects.u64, (uint64_t)bits, __ATOMIC_RELEASE);
    return &objects.u64;
  case 8:
    __atomic_store_n(&objects.boolean_byte, (uint8_t)bits, __ATOMIC_RELEASE);
    return &objects.boolean_byte;
  default:
    abort();
  }
}

uint64_t gti_test_lane_read(unsigned index) {
  switch (index) {
  case 0:
    return (uint8_t)__atomic_load_n(&objects.i8, __ATOMIC_ACQUIRE);
  case 1:
    return __atomic_load_n(&objects.u8, __ATOMIC_ACQUIRE);
  case 2:
    return (uint16_t)__atomic_load_n(&objects.i16, __ATOMIC_ACQUIRE);
  case 3:
    return __atomic_load_n(&objects.u16, __ATOMIC_ACQUIRE);
  case 4:
    return (uint32_t)__atomic_load_n(&objects.i32, __ATOMIC_ACQUIRE);
  case 5:
    return __atomic_load_n(&objects.u32, __ATOMIC_ACQUIRE);
  case 6:
    return (uint64_t)__atomic_load_n(&objects.i64, __ATOMIC_ACQUIRE);
  case 7:
    return __atomic_load_n(&objects.u64, __ATOMIC_ACQUIRE);
  case 8:
    return __atomic_load_n(&objects.boolean_byte, __ATOMIC_ACQUIRE);
  default:
    abort();
  }
}
