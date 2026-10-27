#include <stdint.h>
#include <stdio.h>
int32_t library_counter;
const int32_t library_limit = 17;
static int32_t first(int32_t value) { return value + 11; }
static int32_t second(int32_t value) { return value + 29; }
int32_t (*library_operation)(int32_t) = first;
int32_t library_replace(int32_t value) { library_operation = second; return value; }
void library_report(int32_t a, int32_t b) { printf("native-state:%d,%d\n", a, b); }
void library_cleanup(void) { puts("cleanup"); }
