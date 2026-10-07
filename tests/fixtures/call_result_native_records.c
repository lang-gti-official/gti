#include <stdint.h>

typedef struct Point {
  int32_t x;
  int32_t y;
} Point;

static int32_t calls;

Point gti_call_result_make(void) {
  ++calls;
  return (Point){10, 20};
}

Point gti_call_result_transform(Point value) {
  ++calls;
  return (Point){value.x + 1, value.y + 2};
}

int32_t gti_call_result_calls(void) { return calls; }
