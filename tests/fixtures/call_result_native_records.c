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

typedef struct Address {
  int32_t *pointer;
  int32_t number;
} Address;

static int32_t pointee = 37;
Address gti_call_result_address(void) { return (Address){&pointee, 41}; }
Address gti_call_result_address_transform(Address value) {
  value.number += 1;
  return value;
}
int32_t gti_call_result_address_matches(Address value) {
  return value.pointer == &pointee && value.number == 42;
}
