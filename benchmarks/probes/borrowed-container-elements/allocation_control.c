#include <stdint.h>
#include <stdlib.h>
void bench_begin(void);
void bench_end(uint64_t);
int main(void) {
  bench_begin();
  volatile unsigned char *p = malloc(123);
  if (!p)
    return 1;
  p[0] = 7;
  uint64_t digest = p[0];
  free((void *)p);
  bench_end(digest);
}
