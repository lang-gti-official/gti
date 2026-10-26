#include <stdint.h>
#include <stdio.h>
typedef int32_t (*Transform)(int32_t);
static int32_t increment(int32_t value) {
  int32_t result = value + 11;
  printf("result:%d\n", (int)result);
  return result;
}
static int32_t replaced(int32_t value) {
  puts("wrong-target");
  return value;
}
static Transform selected = increment;
Transform native_example_select(void) {
  puts("selected");
  return selected;
}
int32_t native_example_argument(int32_t value) {
  printf("argument:%d\n", (int)value);
  selected = replaced;
  return value;
}
void native_example_cleanup(void) { puts("cleanup"); }
