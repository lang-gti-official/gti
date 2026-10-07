#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Native ABI facts, not allocator policy. The source provider validates every
// request before narrowing uint64_t to the platform's actual size_t.
uint64_t gti_rt_native_size_max_v1(void);

#if defined(_WIN32)
// Preconditions: bytes > 0; alignment is a nonzero power of two; both fit
// native size_t. This only forwards to the matching CRT allocation family.
void *gti_rt_aligned_malloc_v1(uint64_t bytes, uint64_t alignment);
void gti_rt_aligned_free_v1(void *address);
#else
uint64_t gti_rt_posix_pointer_size_v1(void);
int32_t gti_rt_posix_enomem_v1(void);
int32_t gti_rt_posix_einval_v1(void);
// Preconditions: bytes > 0; alignment is a power of two and a multiple of
// sizeof(void*); both fit size_t; status points to a writable int32_t. Preserve
// the POSIX status unchanged. GTI cannot yet declare this size_t/void** ABI.
void *gti_rt_posix_memalign_v1(uint64_t bytes, uint64_t alignment,
                               int32_t *status);
// Avoid claiming an application's ordinary free declaration while compatible
// repeated extern-C declarations are not yet supported by GTI.
void gti_rt_posix_free_v1(void *address);
#endif

#ifdef __cplusplus
}
#endif
