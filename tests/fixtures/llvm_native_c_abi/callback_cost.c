#include <stdint.h>
typedef uint64_t (*Operation)(uint64_t);
int32_t native_callback_cost(Operation operation) {
  uint64_t sum = 0;
  for (uint64_t i = 0; i < UINT64_C(20000000); ++i)
    sum += operation(i);
  return sum == UINT64_C(199999990000000) ? 0 : 1;
}
