#include <stdint.h>

static volatile int32_t input = 4;

int32_t debug_input(void) { return input; }
void debug_checkpoint(int32_t value) { input = value; }
