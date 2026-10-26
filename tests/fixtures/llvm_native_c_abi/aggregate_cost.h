#pragma once
#include <stdint.h>
typedef struct {
  uint64_t first;
  uint64_t second;
} CostPair;
typedef struct {
  uint64_t values[3];
} CostLarge;
CostPair native_cost_pair_seed(void);
CostLarge native_cost_large_seed(void);
CostPair native_cost_pair_step(CostPair value);
CostLarge native_cost_large_step(CostLarge value);
int32_t native_cost_aggregate_finish(uint64_t checksum, CostPair small,
                                     CostLarge large);
