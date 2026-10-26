#include <stdint.h>

extern uint64_t native_array_read(uint64_t**, uint64_t);
typedef struct { uint64_t* slots[2]; } PointerArray;
static uint64_t snapshot(PointerArray value, uint64_t* next, uint64_t index) {
  value.slots[0] = next;
  return native_array_read(value.slots, index);
}
int main(void) {
  uint64_t first = 3, second = 7;
  PointerArray slots = {{&first, &second}};
  uint64_t total = 0;
  for (uint64_t index = 0; index < UINT64_C(20000000); ++index) {
    if (__builtin_add_overflow(total, snapshot(slots, &second, index), &total))
      return 2;
  }
  return total == UINT64_C(140000000) && slots.slots[0] == &first ? 0 : 1;
}
