#include "gti/runtime_mutex.h"
#include "gti/runtime_failure.h"
#include "gti/runtime_thread.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern void gti_test_mutex_real_initialize_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_destroy_v1(gti_mutex_slot_v1 *);
extern uint32_t gti_test_mutex_real_lock_v1(gti_mutex_slot_v1 *);

static gti_mutex_slot_v1 *live[16];
static unsigned initialized, destroyed, live_count;
static bool ready;
static bool tls_drained;
static unsigned constructors;
extern void gti_test_quarantine_worker(void (*)(void *), void *);
extern void gti_test_quarantined_worker_progress(void);

static void observe_live_globals(void *unused) {
  (void)unused;
  assert(live_count == 2 && ready);
  assert(gti_test_mutex_real_lock_v1(live[0]) == 0);
  gti_rt_mutex_unlock_v1(live[0]);
}

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
  assert(ready && tls_drained && live_count == 0 && initialized == destroyed);
}

void gti_test_program_tls_observe(void) {
  assert(ready && !tls_drained && live_count == 2);
  tls_drained = true;
}

void gti_test_program_ready(void) {
  assert(!ready && live_count == 2 && initialized >= 2);
  ready = true;
  assert(atexit(after_hosted_retirement) == 0);
}

int32_t gti_test_program_gate(int32_t stage) {
  const char *mode = getenv("GTI_O13_PROCESS_FAILURE");
  if (stage == 1)
    ++constructors;
  if (!mode)
    return 1;
  if (stage == 3 && strcmp(mode, "quarantine") == 0) {
    gti_test_quarantine_worker(observe_live_globals, NULL);
    return 0;
  }
  const bool fail =
      (stage == 0 && strcmp(mode, "before") == 0) ||
      (stage == 1 && constructors == 1 && strcmp(mode, "partial-first") == 0) ||
      (stage == 1 && constructors == 2 && strcmp(mode, "partial-last") == 0) ||
      (stage == 2 && strcmp(mode, "after") == 0) ||
      (stage == 3 && strcmp(mode, "entry") == 0);
  return fail ? 0 : 1;
}

_Noreturn void gti_test_real_failure_terminate_v2(
    const gti_failure_record_v2 *, const gti_failure_artifact_descriptor_v1 *,
    gti_failure_observer_v2, void *);

_Noreturn void gti_rt_failure_terminate_v2(
    const gti_failure_record_v2 *record,
    const gti_failure_artifact_descriptor_v1 *artifact,
    gti_failure_observer_v2 observer, void *context) {
  const char *mode = getenv("GTI_O13_PROCESS_FAILURE");
  assert(mode);
  if (strcmp(mode, "quarantine") == 0) {
    assert(live_count == 2 && initialized == destroyed + 2 &&
           !gti_rt_thread_program_storage_retirement_ready_v1());
    gti_test_quarantined_worker_progress();
  } else {
    assert(live_count == 0 && initialized == destroyed);
  }
  assert(!ready || tls_drained);
  assert(record->abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
         record->code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 &&
         record->detail == GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1 && artifact);
  gti_test_real_failure_terminate_v2(record, artifact, observer, context);
}
