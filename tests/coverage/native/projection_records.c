#include <stddef.h>
#include <stdint.h>

struct Samples {
  int32_t values[4];
  int32_t count;
};

_Static_assert(sizeof(struct Samples) == 20, "Samples C ABI size");
_Static_assert(offsetof(struct Samples, count) == 16, "Samples C ABI field offset");
_Static_assert(_Generic(((struct Samples *)0)->values[0], int32_t: 1, default: 0),
               "Samples element is a C int32_t");

struct Samples *native_samples(void) {
  static struct Samples samples = {{4, 0, 7, 8}, 4};
  return &samples;
}

static int32_t constructor_count;

void mark_constructor(void) { ++constructor_count; }
int32_t constructed_count(void) { return constructor_count; }
