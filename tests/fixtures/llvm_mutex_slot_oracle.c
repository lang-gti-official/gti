#include "llvm_mutex_slot_oracle.h"

#include <stddef.h>

static gti_test_mutex_slot_v1 slots[2];

uint64_t gti_test_mutex_c_size(void) { return sizeof(gti_test_mutex_slot_v1); }
uint64_t gti_test_mutex_c_alignment(void) {
  return _Alignof(gti_test_mutex_slot_v1);
}
uint64_t gti_test_mutex_c_stride(void) {
  return (unsigned char *)&slots[1] - (unsigned char *)&slots[0];
}
uint64_t gti_test_mutex_c_embedding_offset(void) {
  return offsetof(gti_test_mutex_embedding_v1, slot);
}
uint64_t gti_test_mutex_c_embedding_size(void) {
  return sizeof(gti_test_mutex_embedding_v1);
}
gti_test_mutex_slot_v1 *gti_test_mutex_c_slots(void) { return slots; }

int32_t gti_test_mutex_c_roundtrip(gti_test_mutex_slot_v1 *slot) {
  int32_t status = gti_test_mutex_slot_lock_v1(slot);
  if (status == 0)
    gti_test_mutex_slot_unlock_v1(slot);
  return status;
}
