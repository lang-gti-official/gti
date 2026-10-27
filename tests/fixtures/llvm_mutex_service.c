#include "gti/runtime_failure.h"
#include "gti/runtime_mutex.h"
#include "gti/runtime_thread.h"

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
static bool process_lifecycles;
static gti_mutex_slot_v1 *process_shells[2];
static unsigned process_live;
extern void gti_test_program_init_prefix(int32_t count);
extern void gti_test_program_retire(void);
extern void gti_test_program_unload(void);
extern void gti_test_quarantine_worker(void (*)(void *), void *);
extern void gti_test_quarantined_worker_progress(void);

static void observe_process_shell(void *unused) {
  (void)unused;
  assert(gti_rt_mutex_lock_v1(process_shells[0]) == 0);
  gti_rt_mutex_unlock_v1(process_shells[0]);
}

static void reject_quarantined_unload(void) {
  process_lifecycles = true;
  gti_test_program_init_prefix(2);
  const unsigned before = destroyed;
  gti_test_quarantine_worker(observe_process_shell, NULL);
  gti_test_program_retire();
  assert(process_live == 2 && destroyed == before);
  gti_test_quarantined_worker_progress();
  gti_test_program_retire();
  assert(process_live == 2 && destroyed == before);
  gti_test_program_unload();
}

static void process_retirement(void) {
  const unsigned before = initialized;
  process_lifecycles = true;
  gti_test_program_retire();
  for (int32_t prefix = 1; prefix != 3; ++prefix) {
    gti_test_program_init_prefix(prefix);
    assert(process_live == (unsigned)prefix);
    gti_test_program_retire();
    assert(process_live == 0 && initialized == destroyed);
    gti_test_program_retire();
    assert(initialized == destroyed);
  }
  assert(initialized == before + 3);
  process_lifecycles = false;
}

struct failure_channel {
  gti_failure_record_v2 record;
  const gti_failure_artifact_descriptor_v1 *artifact;
};
extern void gti_test_mutex_active_initialize(void *, int32_t, int32_t);
extern void gti_test_mutex_active_transfer(void *, void *);
extern uint32_t gti_test_mutex_active_cleanup(void *, struct failure_channel *);
extern void gti_test_mutex_cell_transfer(void *, void *);
extern uint32_t gti_test_mutex_cell_cleanup(void *, struct failure_channel *);

static unsigned payload_drops;
static int32_t payload_trace;
static const gti_failure_artifact_descriptor_v1 payload_artifact = {
    .abi_version = 1};

/* Deliberate test double for the selected source destructor, not generated GTI
 * body evidence or a C++ comparator. Its ABI is checked by the emitter test. */
bool gti_test_mutex_payload_drop(struct failure_channel *failure,
                                 void *payload) {
  int32_t value;
  memcpy(&value, payload, sizeof(value));
  ++payload_drops;
  payload_trace = payload_trace * 10 + value;
  if (value != 0)
    return true;
  memset(failure, 0, sizeof(*failure));
  failure->record.abi_version = GTI_FAILURE_ABI_VERSION_V2;
  failure->record.code = GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1;
  failure->record.detail = GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1;
  failure->record.site_index = 17;
  failure->artifact = &payload_artifact;
  return false;
}

static void active_lifecycles(void) {
  _Alignas(8) unsigned char first[80], second[80], replacement[80];
  struct failure_channel failure, unchanged;
  memset(&failure, 0xa5, sizeof(failure));
  memcpy(&unchanged, &failure, sizeof(failure));
  unsigned start_init = initialized, start_destroy = destroyed;
  /* A completed shell with no published payload rolls back the shell only. */
  gti_test_mutex_active_initialize(first, 9, 0);
  assert(gti_test_mutex_active_cleanup(first, &failure) == 1);
  assert(gti_test_mutex_active_cleanup(first, &failure) == 1);
  assert(destroyed == start_destroy + 1 && payload_drops == 0);
  assert(memcmp(&failure, &unchanged, sizeof(failure)) == 0);

  gti_test_mutex_active_initialize(first, 3, 1);
  gti_test_mutex_active_transfer(second, first);
  assert(initialized == start_init + 3 && destroyed == start_destroy + 2);
  assert(first[72] == 0 && second[72] == 3 && payload_drops == 0);
  assert(gti_test_mutex_active_cleanup(first, &failure) == 1);
  assert(payload_drops == 0 && destroyed == start_destroy + 2);
  gti_test_mutex_active_initialize(replacement, 4, 1);
  assert(gti_test_mutex_active_cleanup(second, &failure) == 1);
  assert(payload_trace == 3 && payload_drops == 1);
  gti_test_mutex_active_transfer(second, replacement);
  assert(gti_test_mutex_active_cleanup(replacement, &failure) == 1);
  assert(gti_test_mutex_active_cleanup(second, &failure) == 1);
  assert(gti_test_mutex_active_cleanup(second, &failure) == 1);
  assert(payload_trace == 34 && payload_drops == 2);
  assert(memcmp(&failure, &unchanged, sizeof(failure)) == 0);
  assert(initialized == destroyed);

  /* Wrapper transfer recursively creates fresh native identities. Reverse
   * source field cleanup continues after the first payload failure. */
  _Alignas(8) unsigned char cell[160], moved[160];
  payload_trace = 0;
  gti_test_mutex_active_initialize(cell, 5, 1);
  gti_test_mutex_active_initialize(cell + 80, 0, 1);
  start_init = initialized;
  start_destroy = destroyed;
  gti_test_mutex_cell_transfer(moved, cell);
  assert(initialized == start_init + 2 && destroyed == start_destroy + 2);
  assert(gti_test_mutex_cell_cleanup(cell, &failure) == 1);
  assert(payload_drops == 2);
  assert(gti_test_mutex_cell_cleanup(moved, &failure) == 0);
  assert(payload_drops == 4 && payload_trace == 5);
  assert(failure.record.abi_version == GTI_FAILURE_ABI_VERSION_V2 &&
         failure.record.code == GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1 &&
         failure.record.site_index == 17 &&
         failure.artifact == &payload_artifact);
  memcpy(&unchanged, &failure, sizeof(failure));
  assert(gti_test_mutex_cell_cleanup(moved, &failure) == 1);
  assert(payload_drops == 4 && initialized == destroyed);
  assert(memcmp(&failure, &unchanged, sizeof(failure)) == 0);
}
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
  if (process_lifecycles) {
    assert(process_live < 2);
    process_shells[process_live++] = slot;
  }
  ++initialized;
}
void gti_rt_mutex_destroy_v1(gti_mutex_slot_v1 *slot) {
  if (process_lifecycles) {
    assert(process_live != 0 && process_shells[--process_live] == slot);
  }
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
  active_lifecycles();
  process_retirement();
  if (argc == 2 && strcmp(argv[1], "quarantined-unload") == 0)
    reject_quarantined_unload();
  return 0;
}
