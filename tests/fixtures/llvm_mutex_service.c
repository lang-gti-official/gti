#include "gti/runtime_mutex.h"

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

extern void gti_test_mutex_initialize(void *, int32_t);
extern void gti_test_mutex_destroy(void *);
extern void gti_test_mutex_token(void *);
extern void gti_test_mutex_move_token(void *, void *);
extern void gti_test_mutex_transfer(void *, void *);
extern void gti_test_mutex_cleanup(void *);
extern int32_t gti_test_mutex_lock(void *, void *);
extern void gti_test_mutex_unlock(void *, void *);
extern bool gti_test_mutex_active(void *);
extern int32_t *gti_test_mutex_read(void *, void *);
extern int32_t *gti_test_mutex_read_mut(void *, void *);
extern uint32_t gti_test_mutex_real_lock_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_initialize_v1(gti_mutex_slot_v1 *);
extern void gti_test_mutex_real_destroy_v1(gti_mutex_slot_v1 *);

static unsigned initialized, destroyed;
static gti_mutex_slot_v1 *live_shells[8];
void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) {
  unsigned empty = 8;
  for (unsigned index = 0; index != 8; ++index) {
    assert(live_shells[index] != slot);
    if (!live_shells[index])
      empty = index;
  }
  assert(empty != 8);
  gti_test_mutex_real_initialize_v1(slot);
  live_shells[empty] = slot;
  ++initialized;
}
void gti_rt_mutex_destroy_v1(gti_mutex_slot_v1 *slot) {
  unsigned found = 8;
  for (unsigned index = 0; index != 8; ++index)
    if (live_shells[index] == slot)
      found = index;
  assert(found != 8);
  gti_test_mutex_real_destroy_v1(slot);
  live_shells[found] = 0;
  ++destroyed;
}

static int fail_lock;
uint32_t gti_rt_mutex_lock_v1(gti_mutex_slot_v1 *slot) {
  return fail_lock ? 1 : gti_test_mutex_real_lock_v1(slot);
}

static _Alignas(8) unsigned char storage[72];

static void *worker(void *unused) {
  (void)unused;
  void *token;
  gti_test_mutex_token(&token);
  for (unsigned index = 0; index < 10000; ++index) {
    assert(gti_test_mutex_lock(storage, &token) == 0);
    assert(gti_test_mutex_active(&token));
    ++*gti_test_mutex_read_mut(storage, &token);
    gti_test_mutex_unlock(storage, &token);
    assert(!gti_test_mutex_active(&token));
  }
  return 0;
}

int main(int argc, char **argv) {
  void *token, *moved;
  gti_test_mutex_initialize(storage, 0);
  gti_test_mutex_token(&token);
  gti_test_mutex_token(&moved);
  assert(!gti_test_mutex_active(&token));
  fail_lock = 1;
  assert(gti_test_mutex_lock(storage, &token) == 1);
  assert(!gti_test_mutex_active(&token));
  fail_lock = 0;
  assert(gti_test_mutex_lock(storage, &token) == 0);
  if (argc == 2 && strcmp(argv[1], "active-lock") == 0)
    return gti_test_mutex_lock(storage, &token);
  if (argc == 2 && strcmp(argv[1], "wrong-owner") == 0) {
    _Alignas(8) unsigned char other[72];
    gti_test_mutex_initialize(other, 7);
    gti_test_mutex_unlock(other, &token);
    return 99;
  }
  gti_test_mutex_move_token(&moved, &token);
  assert(!gti_test_mutex_active(&token) && gti_test_mutex_active(&moved));
  assert(*gti_test_mutex_read(storage, &moved) == 0);
  if (argc == 2 && strcmp(argv[1], "inactive-read") == 0)
    return *gti_test_mutex_read(storage, &token);
  gti_test_mutex_unlock(storage, &moved);
  pthread_t first, second;
  assert(pthread_create(&first, 0, worker, 0) == 0);
  assert(pthread_create(&second, 0, worker, 0) == 0);
  assert(pthread_join(first, 0) == 0 && pthread_join(second, 0) == 0);
  assert(gti_test_mutex_lock(storage, &token) == 0);
  assert(*gti_test_mutex_read(storage, &token) == 20000);
  gti_test_mutex_unlock(storage, &token);
  _Alignas(8) unsigned char destination[72];
  const unsigned before_init = initialized, before_destroy = destroyed;
  gti_test_mutex_transfer(destination, storage);
  assert(initialized == before_init + 1 && destroyed == before_destroy + 1);
  assert(storage[68] == 0 && destination[68] == 3);
  if (argc == 2 && strcmp(argv[1], "retired-lock") == 0)
    return gti_test_mutex_lock(storage, &token);
  gti_test_mutex_cleanup(storage);
  assert(destroyed == before_destroy + 1);
  assert(gti_test_mutex_lock(destination, &token) == 0);
  assert(*gti_test_mutex_read(destination, &token) == 20000);
  gti_test_mutex_unlock(destination, &token);
  gti_test_mutex_cleanup(destination);
  gti_test_mutex_cleanup(destination);
  assert(destroyed == before_destroy + 2);
  gti_test_mutex_initialize(storage, 42);
  assert(gti_test_mutex_lock(storage, &token) == 0);
  assert(*gti_test_mutex_read(storage, &token) == 42);
  gti_test_mutex_unlock(storage, &token);
  gti_test_mutex_destroy(storage);
  assert(initialized == destroyed);
  return 0;
}
