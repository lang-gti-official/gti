#include "gti/runtime_failure.h"
#include "gti/runtime_mutex.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern void gti_test_mutex_real_initialize_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_destroy_v1(gti_mutex_slot_v1 *);
extern uint32_t gti_test_mutex_real_lock_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_unlock_v1(gti_mutex_slot_v1 *);

static gti_mutex_slot_v1 *live[32], *locked;
static unsigned live_count, initialized, destroyed, attempts, locks, unlocks,
    drops;
static bool started;

static bool mode(const char *expected) {
  const char *value = getenv("GTI_O13_PROCESS_FAILURE");
  return value && strcmp(value, expected) == 0;
}

void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) {
  assert(started && live_count < 32);
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
  assert(found != live_count && locked != slot);
  gti_test_mutex_real_destroy_v1(slot);
  live[found] = live[--live_count];
  ++destroyed;
}

uint32_t gti_rt_mutex_lock_v1(gti_mutex_slot_v1 *slot) {
  assert(!locked && attempts++ == 0 && live_count == 1 && live[0] == slot);
  if (mode("guard-lock"))
    return 1;
  const uint32_t status = gti_test_mutex_real_lock_v1(slot);
  assert(status == 0);
  locked = slot;
  ++locks;
  return status;
}

void gti_rt_mutex_unlock_v1(gti_mutex_slot_v1 *slot) {
  assert(locked == slot && locks == 1 && unlocks == 0 && drops == 0);
  gti_test_mutex_real_unlock_v1(slot);
  locked = NULL;
  ++unlocks;
}

void gti_test_guard_payload_drop(int32_t value) {
  assert(started && value == 7 && drops++ == 0 && !locked && live_count == 1);
  assert(locks == unlocks && locks == (mode("guard-lock") ? 0U : 1U));
}

int32_t gti_test_guard_divisor(void) {
  assert(locked && locks == 1 && unlocks == 0);
  return mode("guard-body") ? 0 : 1;
}

static void complete(void) {
  assert(started && drops == 1 && attempts == 1 && locks == unlocks &&
         !locked && live_count == 0 && initialized == destroyed);
}

void gti_test_guard_begin(void) {
  assert(!started);
  started = true;
  assert(atexit(complete) == 0);
}

_Noreturn void
gti_test_real_failure_terminate_v2(const gti_failure_record_v2 *,
                                   const gti_failure_artifact_descriptor_v1 *,
                                   gti_failure_observer_v2, void *);

_Noreturn void
gti_rt_failure_terminate_v2(const gti_failure_record_v2 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v2 observer, void *context) {
  complete();
  assert(record->abi_version == GTI_FAILURE_ABI_VERSION_V2 && artifact);
  if (mode("guard-lock")) {
    assert(record->code ==
               GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1 &&
           record->detail == GTI_FAILURE_DETAIL_MUTEX_LOCK_V1 && locks == 0);
  } else {
    assert(mode("guard-body") && locks == 1 &&
           record->code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 &&
           record->detail == GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1);
  }
  gti_test_real_failure_terminate_v2(record, artifact, observer, context);
}
