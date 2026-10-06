#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Nonthrowing host allocation for source-defined resources. `status` is a
// writable output: 0 success, 1 invalid alignment, 2 size overflow, 3
// exhaustion. Alignment is a nonzero power of two. Zero bytes succeeds with a
// null address. The caller retains exclusive release authority and must pass
// the original address and requested alignment to release, once, after all
// objects end.
void *gti_rt_memory_allocate_v1(uint64_t bytes, uint64_t alignment,
                                int32_t *status);
void gti_rt_memory_release_v1(void *address, uint64_t alignment);

#ifdef __cplusplus
}
#endif
