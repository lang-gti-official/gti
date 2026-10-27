#include "gti/embedding.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LOAD(variable, symbol)                                                 \
  do {                                                                         \
    void *address = dlsym(library, symbol);                                    \
    assert(address && sizeof address == sizeof variable);                      \
    memcpy(&variable, &address, sizeof variable);                              \
  } while (0)

int main(int argc, char **argv) {
  assert(argc == 2);
  void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  assert(library);
  gti_embed_status_v1 (*create)(uint32_t, gti_failure_observer_v1, void *,
                                gti_context_v1 **, gti_failure_record_v1 *);
  gti_embed_status_v1 (*destroy)(gti_context_v1 *, gti_failure_record_v1 *);
  gti_embed_status_v1 (*mix)(gti_context_v1 *, uint64_t, uint64_t *,
                             gti_failure_record_v1 *);
  LOAD(create, "gti_context_create_v1");
  LOAD(destroy, "gti_context_destroy_v1");
  LOAD(mix, "embed_mix");
  gti_context_v1 *context = NULL;
  gti_failure_record_v1 failure;
  assert(create(1, NULL, NULL, &context, &failure) == 0);
  struct timespec start, end;
  assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
  uint64_t checksum = 0;
  for (uint64_t i = 0; i != 2000001; ++i) {
    uint64_t output = 0;
    assert(mix(context, i, &output, &failure) == 0);
    checksum ^= output;
  }
  assert(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
  assert(checksum == (UINT64_C(2000000) ^ UINT64_C(91)));
  printf("%.9f\n", (double)(end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000.0);
  assert(destroy(context, &failure) == 0);
  assert(dlclose(library) == 0);
  return 0;
}
