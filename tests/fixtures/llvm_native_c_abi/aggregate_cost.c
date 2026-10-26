#include "aggregate_cost.h"
CostPair native_cost_pair_seed(void) { return (CostPair){1, 2}; }
CostLarge native_cost_large_seed(void) { return (CostLarge){{3, 4, 5}}; }
CostPair native_cost_pair_step(CostPair value) {
  ++value.first;
  value.second += 2;
  return value;
}
CostLarge native_cost_large_step(CostLarge value) {
  value.values[0] += 3;
  value.values[1] += 4;
  value.values[2] += 5;
  return value;
}
int32_t native_cost_aggregate_finish(uint64_t checksum, CostPair small,
                                     CostLarge large) {
  return checksum != UINT64_C(1200000180000000) ||
         small.first != UINT64_C(20000001) ||
         small.second != UINT64_C(40000002) ||
         large.values[0] != UINT64_C(60000003) ||
         large.values[1] != UINT64_C(80000004) ||
         large.values[2] != UINT64_C(100000005);
}
