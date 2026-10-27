#include "storage_cost.h"
uint64_t native_cost_counter;
static uint64_t calls;
uint64_t native_cost_storage_step(uint64_t value) {
  if (native_cost_counter != value) return UINT64_MAX;
  ++calls;
  native_cost_counter = value + 3;
  return value + 5;
}
int32_t native_cost_storage_finish(uint64_t checksum) {
  return checksum != UINT64_C(400000140000000) ||
         native_cost_counter != UINT64_C(20000002) ||
         calls != UINT64_C(20000000);
}
