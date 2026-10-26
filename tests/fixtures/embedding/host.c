#include "module.h"
#include <assert.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static gti_embed_status_v1 (*exchange_fn)(gti_context_v1 *, uint64_t,
                                          uint64_t *, gti_failure_record_v1 *);
static gti_embed_status_v1 (*destroy_fn)(gti_context_v1 *,
                                         gti_failure_record_v1 *);
static gti_context_v1 *active_context;

#define LOAD(variable, symbol)                                                 \
  do {                                                                         \
    void *address = dlsym(library, symbol);                                    \
    assert(address && sizeof address == sizeof variable);                      \
    memcpy(&variable, &address, sizeof variable);                              \
  } while (0)

static void *wrong_thread(void *unused) {
  (void)unused;
  gti_failure_record_v1 failure;
  uint64_t value = 999;
  assert(exchange_fn(active_context, 55, &value, &failure) ==
         GTI_EMBED_STATUS_WRONG_THREAD_V1);
  assert(value == 999);
  assert(destroy_fn(active_context, &failure) ==
         GTI_EMBED_STATUS_WRONG_THREAD_V1);
  return NULL;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  uint8_t identity[32];
  for (int cycle = 0; cycle < 3; ++cycle) {
    void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      fprintf(stderr, "%s\n", dlerror());
      return 1;
    }
    const gti_module_descriptor_v1 *(*descriptor_fn)(void);
    gti_embed_status_v1 (*create_fn)(uint32_t, gti_failure_observer_v1, void *,
                                     gti_context_v1 **,
                                     gti_failure_record_v1 *);
    gti_embed_status_v1 (*pair_fn)(gti_context_v1 *, Pair, Pair *,
                                   gti_failure_record_v1 *);
    gti_embed_status_v1 (*large_fn)(gti_context_v1 *, Large, Large *,
                                    gti_failure_record_v1 *);
    gti_embed_status_v1 (*hfa_fn)(gti_context_v1 *, Hfa, Hfa *,
                                  gti_failure_record_v1 *);
    gti_embed_status_v1 (*engine_fn)(gti_context_v1 *, Engine *, Engine **,
                                     gti_failure_record_v1 *);
    gti_embed_status_v1 (*record_fn)(gti_context_v1 *, Pair *, Pair **,
                                     gti_failure_record_v1 *);
    gti_embed_status_v1 (*nothing_fn)(gti_context_v1 *,
                                      gti_failure_record_v1 *);
    gti_embed_status_v1 (*tiny_fn)(gti_context_v1 *, int8_t, int8_t *,
                                   gti_failure_record_v1 *);
    gti_embed_status_v1 (*small_fn)(gti_context_v1 *, uint16_t, uint16_t *,
                                    gti_failure_record_v1 *);
    gti_embed_status_v1 (*real_fn)(gti_context_v1 *, double, double *,
                                   gti_failure_record_v1 *);
    gti_embed_status_v1 (*fixed_fn)(gti_context_v1 *, Fixed, Fixed *,
                                    gti_failure_record_v1 *);
    gti_embed_status_v1 (*signed_short_fn)(gti_context_v1 *, int16_t, int16_t *,
                                           gti_failure_record_v1 *);
    gti_embed_status_v1 (*signed_word_fn)(gti_context_v1 *, int32_t, int32_t *,
                                          gti_failure_record_v1 *);
    gti_embed_status_v1 (*signed_long_fn)(gti_context_v1 *, int64_t, int64_t *,
                                          gti_failure_record_v1 *);
    gti_embed_status_v1 (*unsigned_byte_fn)(gti_context_v1 *, uint8_t,
                                            uint8_t *, gti_failure_record_v1 *);
    gti_embed_status_v1 (*unsigned_word_fn)(
        gti_context_v1 *, uint32_t, uint32_t *, gti_failure_record_v1 *);
    gti_embed_status_v1 (*single_fn)(gti_context_v1 *, float, float *,
                                     gti_failure_record_v1 *);
    LOAD(descriptor_fn, "gti_get_module_descriptor_v1");
    LOAD(create_fn, "gti_context_create_v1");
    LOAD(destroy_fn, "gti_context_destroy_v1");
    LOAD(exchange_fn, "embed_exchange");
    LOAD(pair_fn, "embed_pair");
    LOAD(large_fn, "embed_large");
    LOAD(hfa_fn, "embed_hfa");
    LOAD(engine_fn, "embed_engine");
    LOAD(record_fn, "embed_record");
    LOAD(nothing_fn, "embed_nothing");
    LOAD(tiny_fn, "embed_tiny");
    LOAD(small_fn, "embed_small");
    LOAD(real_fn, "embed_real");
    LOAD(fixed_fn, "embed_fixed");
    LOAD(signed_short_fn, "embed_signed_short");
    LOAD(signed_word_fn, "embed_signed_word");
    LOAD(signed_long_fn, "embed_signed_long");
    LOAD(unsigned_byte_fn, "embed_unsigned_byte");
    LOAD(unsigned_word_fn, "embed_unsigned_word");
    LOAD(single_fn, "embed_single");
    const gti_module_descriptor_v1 *descriptor = descriptor_fn();
    assert(descriptor->abi_version == 1 &&
           descriptor->struct_size == sizeof *descriptor);
    assert(descriptor->reserved == 0 && descriptor->export_count == 18);
    assert(descriptor->failure_artifact && descriptor->exports);
    if (!cycle)
      memcpy(identity, descriptor->module_identity, 32);
    else
      assert(memcmp(identity, descriptor->module_identity, 32) == 0);
    assert(memcmp(descriptor->exports[0].symbol.data, "embed_engine", 12) == 0);
    assert(descriptor->exports[0].symbol.length == 12);
    gti_failure_record_v1 failure, original;
    memset(&failure, 0x65, sizeof failure);
    original = failure;
    active_context = NULL;
    assert(create_fn(2, NULL, NULL, &active_context, &failure) == 3 &&
           !active_context);
    assert(create_fn(1, NULL, NULL, NULL, &failure) == 2);
    assert(create_fn(1, NULL, NULL, &active_context, &failure) == 0 &&
           active_context);
    gti_context_v1 *duplicate = NULL;
    assert(create_fn(1, NULL, NULL, &duplicate, &failure) == 4 && !duplicate);
    uint64_t previous = 999;
    assert(exchange_fn(NULL, 42, &previous, &failure) == 2 && previous == 999);
    assert(exchange_fn(active_context, 42, NULL, &failure) == 2);
    assert(exchange_fn(active_context, 42, &previous, NULL) == 2 &&
           previous == 999);
    assert(exchange_fn(active_context, 42, &previous, &failure) == 0 &&
           previous == 17);
    assert(exchange_fn(active_context, 73, &previous, &failure) == 0 &&
           previous == 42);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, wrong_thread, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(exchange_fn(active_context, 88, &previous, &failure) == 0 &&
           previous == 73);
    Pair pair = {-19, 2.25}, pair_output;
    Large large = {1, -4, 97}, large_output;
    Hfa hfa = {1.5f, -3.0f, 4.0f, 8.0f}, hfa_output;
    assert(pair_fn(active_context, pair, &pair_output, &failure) == 0);
    assert(pair_output.first == pair.first &&
           pair_output.second == pair.second);
    assert(large_fn(active_context, large, &large_output, &failure) == 0);
    assert(large_output.a == 1 && large_output.b == -4 && large_output.c == 97);
    assert(hfa_fn(active_context, hfa, &hfa_output, &failure) == 0);
    assert(hfa_output.a == 1.5f && hfa_output.b == -3.0f &&
           hfa_output.c == 4.0f && hfa_output.d == 8.0f);
    Engine *engine = (Engine *)&pair, *engine_output = NULL;
    Pair *record_output = NULL;
    assert(engine_fn(active_context, engine, &engine_output, &failure) == 0 &&
           engine_output == engine);
    assert(record_fn(active_context, &pair, &record_output, &failure) == 0 &&
           record_output == &pair);
    assert(engine_fn(active_context, NULL, &engine_output, &failure) == 0 &&
           !engine_output);
    int8_t tiny;
    uint16_t small;
    double real;
    assert(tiny_fn(active_context, -120, &tiny, &failure) == 0 && tiny == -120);
    assert(small_fn(active_context, 65530, &small, &failure) == 0 &&
           small == 65530);
    assert(real_fn(active_context, -9.75, &real, &failure) == 0 &&
           real == -9.75);
    Fixed fixed = {{3, -91, 48}}, fixed_output;
    assert(fixed_fn(active_context, fixed, &fixed_output, &failure) == 0);
    assert(fixed_output.values[0] == 3 && fixed_output.values[1] == -91 &&
           fixed_output.values[2] == 48);
    int16_t s16;
    int32_t s32;
    int64_t s64;
    uint8_t u8;
    uint32_t u32;
    float f32;
    assert(signed_short_fn(active_context, -32000, &s16, &failure) == 0 &&
           s16 == -32000);
    assert(signed_word_fn(active_context, INT32_MIN, &s32, &failure) == 0 &&
           s32 == INT32_MIN);
    assert(signed_long_fn(active_context, INT64_MIN, &s64, &failure) == 0 &&
           s64 == INT64_MIN);
    assert(unsigned_byte_fn(active_context, 255, &u8, &failure) == 0 &&
           u8 == 255);
    assert(unsigned_word_fn(active_context, UINT32_MAX, &u32, &failure) == 0 &&
           u32 == UINT32_MAX);
    assert(single_fn(active_context, -2.5f, &f32, &failure) == 0 &&
           f32 == -2.5f);
    assert(nothing_fn(active_context, &failure) == 0);
    assert(destroy_fn(active_context, &failure) == 0);
    assert(exchange_fn(active_context, 1, &previous, &failure) == 7 &&
           previous == 73);
    assert(destroy_fn(active_context, &failure) == 7);
    assert(memcmp(&failure, &original, sizeof failure) == 0);
    assert(dlclose(library) == 0);
  }
  return 0;
}
