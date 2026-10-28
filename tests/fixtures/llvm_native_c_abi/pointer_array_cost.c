#include <stdint.h>

uint64_t native_array_read(uint64_t **slots, uint64_t index) {
  return *slots[index & UINT64_C(1)];
}
