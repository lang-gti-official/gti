#include "gti/runtime_atomic_wait.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

_Static_assert(sizeof(gti_atomic_wait_bucket_v1) == 128, "wait domain v1 stride");
_Static_assert(_Alignof(gti_atomic_wait_bucket_v1) == 64, "wait domain v1 alignment");
_Static_assert(offsetof(gti_atomic_wait_bucket_v1, wake_needed) == 0, "wake word");
_Static_assert(offsetof(gti_atomic_wait_bucket_v1, waiters) == 4, "waiter count");
_Static_assert(offsetof(gti_atomic_wait_bucket_v1, native_storage) == 8, "opaque parking");

static uint64_t lane;
static int published;

static void *consume(void *unused) {
  (void)unused;
  gti_rt_atomic_wait_v1(&lane, 0, 8, 1);
  assert(__atomic_load_n(&lane, __ATOMIC_ACQUIRE) == UINT64_MAX);
  assert(published == 42);
  return 0;
}

int main(void) {
  void (*wait_signature)(const void *, uint64_t, uint32_t, uint32_t) =
      gti_rt_atomic_wait_v1;
  void (*notify_signature)(const void *, uint32_t, uint32_t) =
      gti_rt_atomic_notify_v1;
  void (*registered_signature)(const void *, uint32_t, uint32_t) =
      gti_rt_atomic_notify_registered_v1;
  registered_signature(&lane, 8, 0);
  wait_signature(&lane, 1, 8, 0);
  pthread_t worker;
  assert(pthread_create(&worker, 0, consume, 0) == 0);
  published = 42;
  __atomic_store_n(&lane, UINT64_MAX, __ATOMIC_RELEASE);
  notify_signature(&lane, 8, 1);
  assert(pthread_join(worker, 0) == 0);
  return 0;
}
