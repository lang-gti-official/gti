#pragma once

#include <stdint.h>

/* Native service storage, not a GTI payload or std::mutex representation.
 * A slot is address-stable from initialize to destroy. It is never byte-copied
 * while live. Moving a GTI mutex constructs a fresh destination native slot;
 * the source slot stays live until its own explicit destruction. */
typedef struct gti_mutex_slot_v1 {
#ifdef __cplusplus
  alignas(8) unsigned char bytes[64];
#else
  _Alignas(8) unsigned char bytes[64];
#endif
} gti_mutex_slot_v1;

#ifdef __cplusplus
extern "C" {
#define GTI_MUTEX_NOEXCEPT noexcept
#else
#define GTI_MUTEX_NOEXCEPT
#endif

/* Callers prove exclusive initialization/destruction, quiescence and unlocked
 * destruction. Native initialization/destruction/unlock errors terminate. */
void gti_rt_mutex_initialize_v1(gti_mutex_slot_v1 *slot) GTI_MUTEX_NOEXCEPT;
void gti_rt_mutex_destroy_v1(gti_mutex_slot_v1 *slot) GTI_MUTEX_NOEXCEPT;

/* Lock returns 0 only after acquiring, otherwise 1 without acquiring. Its
 * failure is mapped to GTI's existing defined-failure operation by the caller.
 * Unlock requires ownership on the same thread. No exception crosses any of
 * these C boundaries; no callback or payload lifetime is managed here. */
uint32_t gti_rt_mutex_lock_v1(gti_mutex_slot_v1 *slot) GTI_MUTEX_NOEXCEPT;
void gti_rt_mutex_unlock_v1(gti_mutex_slot_v1 *slot) GTI_MUTEX_NOEXCEPT;

#undef GTI_MUTEX_NOEXCEPT
#ifdef __cplusplus
}
#endif
