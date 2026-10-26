#include <gti/c_abi.h>
#include <stdint.h>

extern uint64_t native_cost_scalar(uint64_t);
extern uint64_t native_cost_text(gti_c_string_view);
extern int32_t native_cost_finish(uint64_t, uint64_t);

int main(void) {
  uint64_t scalar = 0;
  uint64_t text = 0;
  for (uint64_t index = 0; index < UINT64_C(20000000); ++index) {
    if (__builtin_add_overflow(scalar, native_cost_scalar(index), &scalar) ||
        __builtin_add_overflow(
            text, native_cost_text((gti_c_string_view){"A\0B", 3}), &text))
      return 2;
  }
  return native_cost_finish(scalar, text);
}
