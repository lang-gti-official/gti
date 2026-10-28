#include "gti/runtime_failure.h"
#include "gti/runtime_mutex.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

extern void gti_test_mutex_real_initialize_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_destroy_v1(gti_mutex_slot_v1 *);

static gti_mutex_slot_v1 *live[32];
static unsigned live_count, initialized, destroyed, payload_drops;
static int32_t trace[3];
static bool started;

void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) {
  assert(live_count < 32);
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
  gti_test_mutex_real_destroy_v1(slot);
  for (unsigned index = found + 1; index < live_count; ++index)
    live[index - 1] = live[index];
  --live_count;
  ++destroyed;
}

int32_t gti_test_active_payload_value(int32_t field) {
  const char *mode = getenv("GTI_O13_PROCESS_FAILURE");
  if (mode && ((field == 1 && strcmp(mode, "cell-first") == 0) ||
               (field == 2 && strcmp(mode, "cell-last") == 0) ||
               (field == 3 && strcmp(mode, "nested") == 0) ||
               (field != 3 && strcmp(mode, "double") == 0)))
    return 0;
  return field;
}

void gti_test_record_payload(int32_t value) {
  assert(started && payload_drops < 3);
  trace[payload_drops++] = value;
}

static void check_trace(bool emergency) {
  assert(started && payload_drops == 3);
  assert(trace[0] == gti_test_active_payload_value(3) &&
         trace[1] == gti_test_active_payload_value(2) &&
         trace[2] == gti_test_active_payload_value(1));
#ifndef GTI_CPP_ORACLE
  if (emergency)
    assert(live_count == 1 && initialized == destroyed + 1);
  else
    assert(live_count == 0 && initialized == destroyed && initialized >= 4);
#else
  (void)emergency;
  assert(initialized == 0 && destroyed == 0 && live_count == 0);
#endif
}

static void normal_exit(void) { check_trace(false); }

void gti_test_active_begin(void) {
  assert(!started);
  started = true;
  assert(atexit(normal_exit) == 0);
}

_Noreturn void
gti_test_real_failure_terminate_v2(const gti_failure_record_v2 *,
                                   const gti_failure_artifact_descriptor_v1 *,
                                   gti_failure_observer_v2, void *);
_Noreturn void gti_test_real_failure_terminate_cleanup_v2(
    const gti_failure_emergency_v2 *,
    const gti_failure_artifact_descriptor_v1 *,
    const gti_failure_artifact_descriptor_v1 *);

_Noreturn void
gti_rt_failure_terminate_v2(const gti_failure_record_v2 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v2 observer, void *context) {
  assert(getenv("GTI_O13_PROCESS_FAILURE") &&
         record->code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 && artifact);
  check_trace(false);
  gti_test_real_failure_terminate_v2(record, artifact, observer, context);
}

_Noreturn void gti_rt_failure_terminate_cleanup_v2(
    const gti_failure_emergency_v2 *failure,
    const gti_failure_artifact_descriptor_v1 *primary,
    const gti_failure_artifact_descriptor_v1 *secondary) {
  assert(getenv("GTI_O13_PROCESS_FAILURE") &&
         failure->abi_version == GTI_FAILURE_ABI_VERSION_V2 && primary &&
         secondary &&
         failure->primary.code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 &&
         failure->secondary.code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1);
  check_trace(true);
  gti_test_real_failure_terminate_cleanup_v2(failure, primary, secondary);
}
