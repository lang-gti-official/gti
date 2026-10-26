#include <stdint.h>
#include <stdio.h>
typedef struct {
  uint32_t id;
  double weight;
} Sample;
typedef struct {
  Sample entries[2];
} Samples;
Sample native_sample_make(uint32_t id) { return (Sample){id, 1.25}; }
Sample native_sample_adjust(Sample value) {
  ++value.id;
  value.weight += 2.5;
  return value;
}
Samples native_sample_pair(Sample first, Sample second) {
  return (Samples){{first, second}};
}
int32_t native_sample_check(Samples values) {
  printf("records:%u,%u\n", (unsigned)values.entries[0].id,
         (unsigned)values.entries[1].id);
  return values.entries[0].id == 7 && values.entries[0].weight == 1.25 &&
         values.entries[1].id == 8 && values.entries[1].weight == 3.75;
}
void native_sample_cleanup(void) { puts("cleanup"); }
