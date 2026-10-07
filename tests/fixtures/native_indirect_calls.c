#include "native_indirect_calls.h"

static int32_t trace;
static int32_t combine(int32_t left, int32_t right) {
  trace = trace * 10 + 4;
  return left * 10 + right;
}
static int32_t increment(int32_t value) { return value + 1; }
static Unary factory(void) { return increment; }
static void write_value(int32_t *value, int32_t input) { *value = input; }
static Point transform_point(Point value) {
  value.x += 2;
  value.y *= 2;
  return value;
}
static int32_t view_length(gti_c_string_view value) {
  return (int32_t)value.length;
}
static long long native_scalars(long value, char code, _Bool flag) {
  return (long long)value + code + flag;
}
static void write_slot(int32_t **slot, int32_t *value) { *slot = value; }
static int32_t read_slot(int32_t *const *slot) { return **slot; }

Binary obtain_binary(void) {
  trace = trace * 10 + 1;
  return combine;
}
Factory obtain_factory(void) { return factory; }
Dispatch obtain_dispatch(void) {
  Dispatch result = {combine, factory, write_value};
  return result;
}
Point obtain_point(void) {
  Point result = {3, 4};
  return result;
}
PointTransform obtain_point_transform(void) { return transform_point; }
ViewLength obtain_view_length(void) { return view_length; }
NativeScalars obtain_native_scalars(void) { return native_scalars; }
SlotWriter obtain_slot_writer(void) { return write_slot; }
SlotReader obtain_slot_reader(void) { return read_slot; }
int32_t next_argument(int32_t digit) {
  trace = trace * 10 + digit;
  return digit;
}
int32_t trace_value(void) { return trace; }
void clear_trace(void) { trace = 0; }
