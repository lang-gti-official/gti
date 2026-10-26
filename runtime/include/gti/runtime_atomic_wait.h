#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#define GTI_ATOMIC_WAIT_NOEXCEPT noexcept
#else
#define GTI_ATOMIC_WAIT_NOEXCEPT
#endif

/* One final-image provider owns the parking domain. All calls for a lane must
 * use that provider; independent copies in unloadable modules are unsupported.
 * The caller supplies a live naturally aligned atomic lane of width 1/2/4/8,
 * keeps its storage alive and unmoved through the call, and uses atomic access
 * for every concurrent observer. No address is retained after return.
 *
 * old_bits is the zero-extended same-width representation, including signed
 * lanes. load_order is 0 relaxed, 1 acquire, or 2 sequentially consistent.
 * Wait returns only after an atomic observation differs from old_bits. Native
 * errors and invalid ABI inputs terminate; no exception crosses this boundary.
 * No GTI allocation, callback, TLS attachment or per-object state is required.
 */
void gti_rt_atomic_wait_v1(const void *lane, uint64_t old_bits,
                           uint32_t width_bytes, uint32_t load_order)
    GTI_ATOMIC_WAIT_NOEXCEPT;

/* all is 0 (at least one eligible waiter) or 1 (all eligible waiters). Internal
 * extra wakes are permitted. Notifications carry neither credit nor a memory
 * order; the wait's atomic load supplies any acquire synchronization. */
void gti_rt_atomic_notify_v1(const void *lane, uint32_t width_bytes, uint32_t all)
    GTI_ATOMIC_WAIT_NOEXCEPT;

#undef GTI_ATOMIC_WAIT_NOEXCEPT
#ifdef __cplusplus
}
#endif
