#include <stdint.h>
#include <stdio.h>
typedef int32_t (*NativeUnary)(int32_t);
int32_t unrelated_getter(void) { return 37; }
double unrelated_math(double left, double right) { return 2 * left + right; }
static int32_t apply(int32_t value) { return value + 11; }
NativeUnary unrelated_callback_getter(void) { return apply; }
extern int32_t bridge_getter(void);
extern double bridge_math(double, double);
extern int32_t bridge_callback(int32_t);
#ifndef GTI_SOURCE_HOST
int main(void) {
  if (bridge_getter() != 37 || bridge_math(1.25, 3) != 5.5 ||
      bridge_callback(7) != 18)
    return 1;
  puts("exact native C declaration contracts");
  return 0;
}
#endif
