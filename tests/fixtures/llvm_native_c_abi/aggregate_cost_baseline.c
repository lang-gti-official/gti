#include "aggregate_cost.h"
int main(void) {
  CostPair small = native_cost_pair_seed();
  CostLarge large = native_cost_large_seed();
  uint64_t checksum = 0, index = 0;
  while (index < UINT64_C(20000000)) {
    small = native_cost_pair_step(small);
    large = native_cost_large_step(large);
    if (__builtin_add_overflow(checksum, small.first, &checksum))
      return 70;
    if (__builtin_add_overflow(checksum, large.values[2], &checksum))
      return 70;
    if (__builtin_add_overflow(index, UINT64_C(1), &index))
      return 70;
  }
  return native_cost_aggregate_finish(checksum, small, large);
}
