#include "storage_cost.h"
int main(void) {
  uint64_t checksum = 0, index = 0;
  while (index < UINT64_C(20000000)) {
    native_cost_counter = index;
    uint64_t result = native_cost_storage_step(index);
    if (__builtin_add_overflow(checksum, native_cost_counter, &checksum))
      return 70;
    if (__builtin_add_overflow(checksum, result, &checksum))
      return 70;
    if (__builtin_add_overflow(index, UINT64_C(1), &index))
      return 70;
  }
  return native_cost_storage_finish(checksum);
}
