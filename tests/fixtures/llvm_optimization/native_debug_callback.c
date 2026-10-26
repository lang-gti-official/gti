#include <stdint.h>
#include <stdio.h>

static volatile int32_t input = 4;
int32_t debug_input(void) { return input; }
void debug_checkpoint(int32_t value) { input = value; }
int32_t debug_invoke(int32_t (*operation)(int32_t), int32_t value) {
  return operation(value) + 1;
}
void debug_cleanup(void) { fputc('D', stdout); }
