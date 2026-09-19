#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Scheduling hint only: no synchronization edge, minimum delay, or fairness.
// Thread-safe, allocation-free, and callable from the initial or managed
// thread. The operation acquires no GTI ownership and raises no defined
// failure.
void gti_rt_cthread_yield_v1(void);

#ifdef __cplusplus
}
#endif
