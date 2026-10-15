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

union Payload payload = {.number = 7};
union Payload second_payload = {.number = 0};
const union Payload frozen_payload = {.number = 41};
struct Packet packet = {1, {.number = 3}};

struct Snapshot make_snapshot(int value) {
  struct Snapshot result = {value, &counter, double_value};
  return result;
}
union Payload make_payload(int value) {
  union Payload result = {.number = value};
  return result;
}
struct Coordinates make_coordinates(int x, int y) {
  struct Coordinates result = {x, y};
  return result;
}
int observe_position(void) {
  return payload.position.x == 10 && payload.position.y == 20;
}
void switch_payload(void) { payload.position.x = 10; payload.position.y = 20; }
int observe_payload(union Payload value) { return value.number == 42; }
int observe_packet(struct Packet value) {
  return value.tag == 1 && value.data.number == 43;
}
