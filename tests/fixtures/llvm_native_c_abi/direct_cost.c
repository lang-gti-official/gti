#include <gti/c_abi.h>
#include <stdint.h>

static uint64_t scalar_calls;
static uint64_t text_calls;

uint64_t native_cost_scalar(uint64_t value) {
  ++scalar_calls;
  return value & UINT64_C(255);
}

uint64_t native_cost_text(gti_c_string_view value) {
  ++text_calls;
  return value.length == 3 && value.data[0] == 'A' && value.data[1] == 0 &&
                 value.data[2] == 'B'
             ? UINT64_C(3)
             : UINT64_MAX;
}

int32_t native_cost_finish(uint64_t scalar, uint64_t text) {
  return scalar_calls == UINT64_C(20000000) &&
                 text_calls == UINT64_C(20000000) &&
                 scalar == UINT64_C(2550000000) && text == UINT64_C(60000000)
             ? 0
             : 1;
}
