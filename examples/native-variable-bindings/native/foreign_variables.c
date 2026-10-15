#include "foreign_variables.h"

static int increment(int value) { return value + 1; }
static int double_value(int value) { return value * 2; }

int counter = 3;
const int limit = 41;
int *value_pointer = &counter;
const int *readonly_pointer = &limit;
Operation operation = increment;
Operation const readonly_operation = double_value;
struct Snapshot snapshot = {7, &counter, increment};
int foreign_epoch = 13;

int *counter_address(void) { return &counter; }
const int *limit_address(void) { return &limit; }
void mutate_counter(int value) {
  counter = value;
  operation = double_value;
}
int observe_snapshot(void) {
  return snapshot.value == 42 && snapshot.counter == &counter &&
         snapshot.operation(21) == 42;
}
