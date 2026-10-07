#include <stdint.h>

struct NativeAddress {
  int32_t stamp;
};
struct NativeAddress empty_native(void) { return (struct NativeAddress){0}; }
