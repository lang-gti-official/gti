#include <stdio.h>
#include <stdlib.h>

void coverage_unbuffered_output(void) {
  if (setvbuf(stdout, NULL, _IONBF, 0) != 0) {
    exit(2);
  }
}
