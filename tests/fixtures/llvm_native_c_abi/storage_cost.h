#ifndef GTI_STORAGE_COST_H
#define GTI_STORAGE_COST_H
#include <stdint.h>
extern uint64_t native_cost_counter;
uint64_t native_cost_storage_step(uint64_t value);
int32_t native_cost_storage_finish(uint64_t checksum);
#endif
