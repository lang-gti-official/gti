#include "gti/runtime_thread.h"

static void write_value(void *context) { *(int *)context = 42; }

int main(void) {
  int value = 0;
  const gti_thread_scoped_task_v1 task = {write_value, &value};
  gti_thread_state_v1 *scratch = 0;
  const gti_thread_scope_result_v1 result =
      gti_rt_thread_scope_v1(&task, &scratch, 1);
  return result.abi_version == GTI_THREAD_ABI_VERSION_V1 &&
                 result.spawn_status == GTI_THREAD_SPAWN_OK_V1 &&
                 result.started == 1 && result.failed_worker == UINT64_MAX &&
                 result.worker.status == GTI_THREAD_JOIN_OK_V1 &&
                 scratch == 0 && value == 42
             ? 0
             : 1;
}
