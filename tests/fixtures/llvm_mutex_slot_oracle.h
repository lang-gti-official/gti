#pragma once

#include <stdint.h>

#ifdef __cplusplus
#define GTI_TEST_SLOT_ALIGN alignas(8)
#else
#define GTI_TEST_SLOT_ALIGN _Alignas(8)
#endif

/* Test-only candidate ABI, NOT an installed runtime interface. unsigned-char
 * array storage stays live while a native object is constructed inside it;
 * neither this slot nor its bytes may be copied/accessed while that object is
 * live. Size/alignment are explicit versioned obligations, not host guesses. */
typedef struct gti_test_mutex_slot_v1 {
  GTI_TEST_SLOT_ALIGN unsigned char bytes[64];
} gti_test_mutex_slot_v1;
#undef GTI_TEST_SLOT_ALIGN

typedef struct gti_test_mutex_embedding_v1 {
  uint8_t prefix;
  gti_test_mutex_slot_v1 slot;
  uint8_t suffix;
} gti_test_mutex_embedding_v1;

#ifdef __cplusplus
extern "C" {
#endif
uint64_t gti_test_mutex_c_size(void);
uint64_t gti_test_mutex_c_alignment(void);
uint64_t gti_test_mutex_c_stride(void);
uint64_t gti_test_mutex_c_embedding_offset(void);
uint64_t gti_test_mutex_c_embedding_size(void);
gti_test_mutex_slot_v1 *gti_test_mutex_c_slots(void);
void gti_test_mutex_slot_initialize_v1(gti_test_mutex_slot_v1 *slot);
void gti_test_mutex_slot_destroy_v1(gti_test_mutex_slot_v1 *slot);
int32_t gti_test_mutex_slot_lock_v1(gti_test_mutex_slot_v1 *slot);
void gti_test_mutex_slot_unlock_v1(gti_test_mutex_slot_v1 *slot);
int32_t gti_test_mutex_c_roundtrip(gti_test_mutex_slot_v1 *slot);
#ifdef __cplusplus
}
#endif
