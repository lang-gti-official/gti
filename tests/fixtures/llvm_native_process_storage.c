#include "gti/runtime_mutex.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

extern void gti_test_mutex_real_initialize_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_destroy_v1(gti_mutex_slot_v1 *);

static gti_mutex_slot_v1 *live[16];
static unsigned initialized, destroyed, live_count;
static bool ready;

void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) {
  assert(live_count < 16);
  for (unsigned index = 0; index < live_count; ++index)
    assert(live[index] != slot);
  gti_test_mutex_real_initialize_v1(slot);
  live[live_count++] = slot;
  ++initialized;
}

void gti_rt_mutex_destroy_v1(gti_mutex_slot_v1 *slot) {
  unsigned found = live_count;
  for (unsigned index = 0; index < live_count; ++index)
    if (live[index] == slot)
      found = index;
  assert(found != live_count);
  if (ready)
    assert(found == live_count - 1);
  gti_test_mutex_real_destroy_v1(slot);
  for (unsigned index = found + 1; index < live_count; ++index)
    live[index - 1] = live[index];
  --live_count;
  ++destroyed;
}

static void after_hosted_retirement(void) {
  assert(ready && live_count == 0 && initialized == destroyed);
}

void gti_test_program_ready(void) {
  assert(!ready && live_count == 2 && initialized >= 2);
  ready = true;
  assert(atexit(after_hosted_retirement) == 0);
}
