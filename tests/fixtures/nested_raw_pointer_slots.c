#include "native.h"

static int32_t **retained;

SlotRecord make_record(void) {
  SlotRecord value = {0};
  return value;
}

void retain_slot(int32_t **slot) { retained = slot; }
void write_retained(int32_t *value) { *retained = value; }
int32_t write_aliases(int32_t **first, int32_t **second, int32_t *value) {
  if (first != second)
    return 1;
  *first = value;
  return *second == value ? 0 : 2;
}
void write_array(int32_t **slots, int32_t *first, int32_t *second) {
  slots[0] = first;
  slots[1] = second;
}
void write_triple(int32_t ***slot, int32_t *value) { **slot = value; }
int32_t read_slot(int32_t *const *slot) { return **slot; }
void write_record(SlotRecord *record, int32_t *value) {
  *record->saved = value;
}
