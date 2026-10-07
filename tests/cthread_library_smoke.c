#include "gti/cthread.h"

static uint64_t worker_identity = 0;
static int initialized = 0;
static void initialize(void) {
  gti_rt_once_enter_v1(initialize);
  initialized = 1;
  gti_rt_once_leave_v1(initialize);
}

static int32_t work(void *context) {
  gti_rt_cthread_enter_v1(work);
  worker_identity = gti_rt_cthread_current_v1();
  *(int32_t *)context = 91;
  gti_rt_cthread_leave_v1(work);
  return 42;
}

int main(void) {
  uint64_t mutex = 0;
  if (gti_rt_mtx_init_v1(&mutex, 3) != 0 || gti_rt_mtx_lock_v1(mutex) != 0 ||
      gti_rt_mtx_trylock_v1(mutex) != 0 ||
      gti_rt_mtx_timedlock_v1(mutex, 0, 0) != 0 ||
      gti_rt_mtx_unlock_v1(mutex) != 0 || gti_rt_mtx_unlock_v1(mutex) != 0 ||
      gti_rt_mtx_unlock_v1(mutex) != 0) {
    return 4;
  }
  gti_rt_mtx_destroy_v1(mutex);
  uint64_t condition = 0;
  if (gti_rt_cnd_init_v1(&condition) != 0 ||
      gti_rt_mtx_init_v1(&mutex, 0) != 0 ||
      gti_rt_cnd_signal_v1(condition) != 0 ||
      gti_rt_cnd_broadcast_v1(condition) != 0 ||
      gti_rt_cnd_wait_v1(condition, mutex) != 1 ||
      gti_rt_mtx_lock_v1(mutex) != 0 ||
      gti_rt_cnd_timedwait_v1(condition, mutex, 0, 0) != 4 ||
      gti_rt_mtx_unlock_v1(mutex) != 0) {
    return 5;
  }
  gti_rt_cnd_destroy_v1(condition);
  gti_rt_mtx_destroy_v1(mutex);
  int64_t seconds_left = 7;
  int64_t nanos_left = 9;
  if (gti_rt_cthread_sleep_v1(0, 1, &seconds_left, &nanos_left) != 0 ||
      gti_rt_cthread_sleep_v1(-1, 0, &seconds_left, &nanos_left) != -2 ||
      seconds_left != 7 || nanos_left != 9) {
    return 3;
  }
  const uint64_t initial = gti_rt_cthread_current_v1();
  gti_rt_cthread_yield_v1();
  gti_rt_cthread_begin_v1();
  gti_rt_once_invoke_v1(initialize);
  uint64_t handle = 0;
  int32_t observed = 0;
  int32_t result = 0;
  if (gti_rt_cthread_create_v1(&handle, work, &observed) != 0 ||
      gti_rt_cthread_join_v1(handle, &result) != 0) {
    return 1;
  }
  gti_rt_cthread_end_v1();
  return initialized == 1 && observed == 91 && result == 42 && initial != 0 &&
                 initial == gti_rt_cthread_current_v1() && initial != handle &&
                 worker_identity == handle
             ? 0
             : 2;
}
