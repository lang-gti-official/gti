#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Scheduling hint only: no synchronization edge, minimum delay, or fairness.
// Thread-safe, allocation-free, and callable from the initial or managed
// thread. The operation acquires no GTI ownership and raises no defined
// failure.
void gti_rt_cthread_yield_v1(void);

// Stable, nonzero, never-reused process identity for the calling native thread.
// Available without begin/end, including on initial, managed and foreign
// threads. A manual worker observes its create result. No synchronization,
// join obligation, or GTI callback permission is granted. Exhaustion
// terminates.
uint64_t gti_rt_cthread_current_v1(void);

// Relative sleep: seconds >= 0, 0 <= nanoseconds < 1,000,000,000. Returns
// 0 elapsed, -1 interrupted, -2 invalid input/unrepresentable time/host error.
// Only -1 publishes normalized remaining time, never more than the request.
// Output pointers must be either both null or both non-null and distinct.
// No synchronization, thread creation, callback attachment, or GTI failure.
int32_t gti_rt_cthread_sleep_v1(int64_t seconds, int64_t nanoseconds,
                                int64_t *remaining_seconds,
                                int64_t *remaining_nanoseconds);

typedef int32_t (*gti_cthread_entry_v1)(void *);
typedef void (*gti_once_initializer_v1)(void);

// Compiler-private same-thread initializer invocation. Flag arbitration and
// release/acquire publication are ordinary GTI atomic operations. A generated
// GTI adapter must claim this exact invocation once and leave after cleanup.
// Null callbacks, native exceptions, and incomplete adapter entry terminate.
void gti_rt_once_invoke_v1(gti_once_initializer_v1 initializer);
void gti_rt_once_enter_v1(gti_once_initializer_v1 initializer);
void gti_rt_once_leave_v1(gti_once_initializer_v1 initializer);

// Manual mutexes: flags plain=0, recursive=1, timed=2 (or recursive|timed).
// Status success=0, error=1, no-memory=2, busy=3, timed-out=4. Init preserves
// output on failure. Handles are never reused; copying grants no new lifetime.
// Successful acquire orders after a prior final release on the same mutex.
// Timed lock takes an absolute Unix/UTC timestamp, normalized nanoseconds;
// timestamps must fit a signed 64-bit nanosecond count. No fairness promise.
// Destroy requires an unlocked mutex with no calls in flight; violations
// terminate, rather than destroying live native synchronization state.
int32_t gti_rt_mtx_init_v1(uint64_t *output, int32_t flags);
void gti_rt_mtx_destroy_v1(uint64_t handle);
int32_t gti_rt_mtx_lock_v1(uint64_t handle);
int32_t gti_rt_mtx_trylock_v1(uint64_t handle);
int32_t gti_rt_mtx_timedlock_v1(uint64_t handle, int64_t seconds,
                                int64_t nanoseconds);
int32_t gti_rt_mtx_unlock_v1(uint64_t handle);

// Manual conditions: wait requires exactly one current-thread mutex lock.
// Enrollment and release are atomic with respect to notification. Success,
// timeout (4), or a recoverable wait error (1) returns with the mutex relocked.
// Invalid arguments leave ownership unchanged. Reacquisition failure
// terminates. Timed wait uses the same absolute UTC range as timed mutex lock;
// the mutex itself need not have the timed flag. Notifications aren't saved for
// future waiters. Destroy requires no calls in flight; violations terminate.
int32_t gti_rt_cnd_init_v1(uint64_t *output);
void gti_rt_cnd_destroy_v1(uint64_t handle);
int32_t gti_rt_cnd_signal_v1(uint64_t handle);
int32_t gti_rt_cnd_broadcast_v1(uint64_t handle);
int32_t gti_rt_cnd_wait_v1(uint64_t condition, uint64_t mutex);
int32_t gti_rt_cnd_timedwait_v1(uint64_t condition, uint64_t mutex,
                                int64_t seconds, int64_t nanoseconds);

// Statuses: success=0, error=1, no-memory=2, busy=3. Context ownership
// stays with the caller through successful join. Handles are never reused.
int32_t gti_rt_cthread_create_v1(uint64_t *output, gti_cthread_entry_v1 entry,
                                 void *context);
int32_t gti_rt_cthread_join_v1(uint64_t handle, int32_t *result);

// Compiler-generated hosted/entry boundaries. A lifecycle or attachment
// contract violation terminates; it is not a recoverable GTI failure.
void gti_rt_cthread_begin_v1(void);
void gti_rt_cthread_end_v1(void);
void gti_rt_cthread_enter_v1(gti_cthread_entry_v1 entry);
void gti_rt_cthread_leave_v1(gti_cthread_entry_v1 entry);

#ifdef __cplusplus
}
#endif
