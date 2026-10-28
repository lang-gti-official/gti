#pragma once

#include <stdint.h>

#define GTI_ATOMIC_WAIT_BUCKET_COUNT_V1 256
#define GTI_ATOMIC_WAIT_BUCKET_BYTES_V1 128
#define GTI_ATOMIC_WAIT_BUCKET_ALIGNMENT_V1 64
#ifdef __cplusplus
#define GTI_ATOMIC_WAIT_ALIGNAS(n) alignas(n)
#else
#define GTI_ATOMIC_WAIT_ALIGNAS(n) _Alignas(n)
#endif

/* Compiler/runtime-private metadata, not source atomic representation. The
 * final-image provider owns exactly one domain. Only its reviewed atomic-lane
 * profile may access these words concurrently. Native bytes are opaque and
 * lazily initialized; generated code must never access them. Bucket selection
 * is ((uintptr_t(lane) >> 3) ^ width_bytes) % 256. On x86-64, notify performs
 * an acq_rel fetch_and(~1) on wake_needed; an old set bit requires the
 * registered notification service below. On arm64, notify uses the existing
 * seq_cst fence followed by a relaxed waiters load. A nonzero count requires
 * that service. Neither path supplies synchronization on the source payload. */
typedef struct gti_atomic_wait_bucket_v1 {
  GTI_ATOMIC_WAIT_ALIGNAS(GTI_ATOMIC_WAIT_BUCKET_ALIGNMENT_V1)
  uint32_t wake_needed;
  uint32_t waiters;
  uint8_t native_storage[120];
} gti_atomic_wait_bucket_v1;

#undef GTI_ATOMIC_WAIT_ALIGNAS

#ifdef __cplusplus
extern "C" {
#define GTI_ATOMIC_WAIT_NOEXCEPT noexcept
#else
#define GTI_ATOMIC_WAIT_NOEXCEPT
#endif

extern gti_atomic_wait_bucket_v1
    gti_rt_atomic_wait_domain_v1[GTI_ATOMIC_WAIT_BUCKET_COUNT_V1];

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
                           uint32_t width_bytes,
                           uint32_t load_order) GTI_ATOMIC_WAIT_NOEXCEPT;

/* all is 0 (at least one eligible waiter) or 1 (all eligible waiters). Internal
 * extra wakes are permitted. Notifications carry neither credit nor a memory
 * order; the wait's atomic load supplies any acquire synchronization. */
void gti_rt_atomic_notify_v1(const void *lane, uint32_t width_bytes,
                             uint32_t all) GTI_ATOMIC_WAIT_NOEXCEPT;

/* Slow half of the typed metadata protocol above. It does not consume another
 * wake bit. It validates inputs and safely initializes the native domain even
 * for a direct C caller. Spurious broadcasts remain permitted. */
void gti_rt_atomic_notify_registered_v1(const void *lane, uint32_t width_bytes,
                                        uint32_t all) GTI_ATOMIC_WAIT_NOEXCEPT;

#undef GTI_ATOMIC_WAIT_NOEXCEPT
#ifdef __cplusplus
}
#endif
