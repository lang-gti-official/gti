#include "gti/runtime_failure.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static unsigned drops, trace;
static bool started;

static bool bounds_failure(void) {
  const char *mode = getenv("GTI_O13_PROCESS_FAILURE");
  return mode && strcmp(mode, "array-bounds") == 0;
}

static void complete(void) {
  assert(started && drops == 2);
  assert(trace == (bounds_failure() ? 21U : 12U));
}

void gti_test_mutex_array_begin(void) {
  assert(!started);
  started = true;
  assert(atexit(complete) == 0);
}

int32_t gti_test_mutex_array_index(void) {
  assert(started && drops == 0);
  return bounds_failure() ? 2 : 1;
}

void gti_test_mutex_array_drop(int32_t value) {
  assert(started && ++drops <= 2 && (value == 1 || value == 2));
  trace = trace * 10 + (unsigned)value;
}

_Noreturn void
gti_test_real_failure_terminate_v2(const gti_failure_record_v2 *,
                                   const gti_failure_artifact_descriptor_v1 *,
                                   gti_failure_observer_v2, void *);

_Noreturn void
gti_rt_failure_terminate_v2(const gti_failure_record_v2 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v2 observer, void *context) {
  assert(bounds_failure() && artifact &&
         record->abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
         record->code == GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1 &&
         record->detail == GTI_FAILURE_DETAIL_FIXED_ARRAY_V1);
  complete();
  gti_test_real_failure_terminate_v2(record, artifact, observer, context);
}
