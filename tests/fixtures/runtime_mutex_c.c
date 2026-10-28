#include "gti/runtime_mutex.h"

#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(sizeof(gti_mutex_slot_v1) == 64, "C slot size");
_Static_assert(_Alignof(gti_mutex_slot_v1) == 8, "C slot alignment");
struct Embedded {
  char prefix;
  gti_mutex_slot_v1 slot;
  char suffix;
};
_Static_assert(offsetof(struct Embedded, slot) == 8, "C embedded offset");
_Static_assert(sizeof(struct Embedded) == 80, "C embedded size");

static gti_mutex_slot_v1 slots[2];
static int value;
static const char *fault = "";

int gti_test_mutex_init(pthread_mutex_t *mutex,
                        const pthread_mutexattr_t *attributes) {
  return strcmp(fault, "init") == 0 ? 12
                                    : pthread_mutex_init(mutex, attributes);
}
int gti_test_mutex_destroy(pthread_mutex_t *mutex) {
  return strcmp(fault, "destroy") == 0 ? 22 : pthread_mutex_destroy(mutex);
}
int gti_test_mutex_lock(pthread_mutex_t *mutex) {
  return strcmp(fault, "lock") == 0 ? 22 : pthread_mutex_lock(mutex);
}
int gti_test_mutex_unlock(pthread_mutex_t *mutex) {
  return strcmp(fault, "unlock") == 0 ? 22 : pthread_mutex_unlock(mutex);
}

static void *increment(void *unused) {
  (void)unused;
  for (unsigned i = 0; i != 10000; ++i) {
    assert(gti_rt_mutex_lock_v1(&slots[1]) == 0);
    ++value;
    gti_rt_mutex_unlock_v1(&slots[1]);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc > 1)
    fault = argv[1];
  void (*initialize)(gti_mutex_slot_v1 *) = gti_rt_mutex_initialize_v1;
  void (*destroy)(gti_mutex_slot_v1 *) = gti_rt_mutex_destroy_v1;
  uint32_t (*lock)(gti_mutex_slot_v1 *) = gti_rt_mutex_lock_v1;
  void (*unlock)(gti_mutex_slot_v1 *) = gti_rt_mutex_unlock_v1;
  initialize(&slots[0]);
  if (strcmp(fault, "lock") == 0) {
    assert(lock(&slots[0]) == 1);
    destroy(&slots[0]);
    return 0;
  }
  assert(lock(&slots[0]) == 0);
  initialize(&slots[1]);
  assert(lock(&slots[1]) == 0);
  unlock(&slots[1]);
  unlock(&slots[0]);
  pthread_t workers[2];
  for (unsigned i = 0; i != 2; ++i)
    assert(pthread_create(&workers[i], 0, increment, 0) == 0);
  for (unsigned i = 0; i != 2; ++i)
    assert(pthread_join(workers[i], 0) == 0);
  assert(value == 20000);
  destroy(&slots[0]);
  destroy(&slots[1]);
  // New native lifetime at reused bytes, without copying a live native mutex.
  initialize(&slots[0]);
  assert(lock(&slots[0]) == 0);
  unlock(&slots[0]);
  destroy(&slots[0]);
  return 0;
}
