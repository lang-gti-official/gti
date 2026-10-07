#include <stdint.h>

_Static_assert(_Generic(0, int: 1, default: 0),
               "This fixture must compile as C17");

struct NativeAddress {
  int32_t stamp;
};
struct NativeAddress empty_native(void) { return (struct NativeAddress){0}; }
