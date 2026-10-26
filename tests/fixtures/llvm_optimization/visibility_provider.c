#include <stdio.h>
#include <stdlib.h>

int gti_visibility_select(void) {
  const char *mode = getenv("GTI_VISIBILITY_FAIL");
  return mode && mode[0] == '1';
}

void gti_visibility_drop(int id) { fprintf(stderr, "drop:%d\n", id); }
