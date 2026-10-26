#include "module.h"
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define LOAD(variable, name)                                                   \
  do {                                                                         \
    void *address = dlsym(image, name);                                        \
    assert(address && sizeof address == sizeof variable);                      \
    memcpy(&variable, &address, sizeof variable);                              \
  } while (0)

int main(int argc, char **argv) {
  assert(argc == 2);
  for (int cycle = 0; cycle < 3; ++cycle) {
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image) {
      fprintf(stderr, "%s\n", dlerror());
      return 1;
    }
    gti_embed_status_v1 (*create)(uint32_t, gti_failure_observer_v1, void *,
                                  gti_context_v1 **, gti_failure_record_v1 *);
    gti_embed_status_v1 (*destroy)(gti_context_v1 *, gti_failure_record_v1 *);
    gti_embed_status_v1 (*exchange)(gti_context_v1 *, uint64_t, uint64_t *,
                                    gti_failure_record_v1 *);
    gti_embed_status_v1 (*echo)(gti_context_v1 *, Pair, Pair *,
                                gti_failure_record_v1 *);
    LOAD(create, "gti_context_create_v1");
    LOAD(destroy, "gti_context_destroy_v1");
    LOAD(exchange, "sample_exchange");
    LOAD(echo, "sample_echo");
    gti_context_v1 *context = NULL;
    gti_failure_record_v1 failure;
    assert(create(1, NULL, NULL, &context, &failure) == 0);
    uint64_t previous = 0;
    assert(exchange(context, 42, &previous, &failure) == 0 && previous == 17);
    assert(exchange(context, 99, &previous, &failure) == 0 && previous == 42);
    Pair value = {-7, 3.25}, result;
    assert(echo(context, value, &result, &failure) == 0);
    assert(result.first == -7 && result.second == 3.25);
    assert(destroy(context, &failure) == 0);
    assert(dlclose(image) == 0);
  }
  return 0;
}
