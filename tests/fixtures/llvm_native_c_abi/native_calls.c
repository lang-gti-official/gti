#include "native_calls.h"
#include <stdio.h>

static int32_t plus11(int32_t value) { return value + 11; }
static int32_t plus29(int32_t value) { return value + 29; }
int32_t native_counter = 5;
const int32_t native_limit = 73;
NativeOp native_operation = plus11;
Nested native_state = {{7, 9}, 1.25};
int32_t *native_pointer = &native_counter;
Pair native_pair(uint64_t a, uint64_t b) { return (Pair){a, b}; }
Pair native_pair_step(Pair value) {
  value.a += 3;
  value.b += 7;
  return value;
}
int32_t native_pair_check(Pair value, uint64_t a, uint64_t b) {
  return value.a == a && value.b == b;
}
Nested native_nested(uint64_t a, double weight) {
  return (Nested){{a, a + 1}, weight};
}
Nested native_nested_step(Nested value) {
  value.pair.a += 3;
  value.pair.b += 7;
  value.weight += 2.5;
  return value;
}
int32_t native_nested_check(Nested value, uint64_t a, double weight) {
  return value.pair.a == a && value.pair.b == a + 5 && value.weight == weight;
}
FloatArray native_float_array(void) {
  return (FloatArray){{1.25f, 2.5f, 4.75f, 9.0f}};
}
FloatArray native_float_step(FloatArray value) {
  for (unsigned i = 0; i < 4; ++i)
    value.values[i] += (float)(i + 1);
  return value;
}
int32_t native_float_check(FloatArray value) {
  return value.values[0] == 2.25f && value.values[1] == 4.5f &&
         value.values[2] == 7.75f && value.values[3] == 13.0f;
}
DoubleArray native_double_array(void) {
  return (DoubleArray){{1.25, 2.5, 4.75, 9.0}};
}
DoubleArray native_double_step(DoubleArray value) {
  for (unsigned i = 0; i < 4; ++i)
    value.values[i] += (double)(i + 1);
  return value;
}
int32_t native_double_check(DoubleArray value) {
  return value.values[0] == 2.25 && value.values[1] == 4.5 &&
         value.values[2] == 7.75 && value.values[3] == 13.0;
}
Boolean native_boolean(bool active, uint16_t code) {
  return (Boolean){active, code};
}
Boolean native_boolean_step(Boolean value) {
  value.active = !value.active;
  value.code += 3;
  return value;
}
NativeUnion native_union(uint64_t bits) {
  NativeUnion value;
  value.bits = bits;
  return value;
}
NativeUnion native_union_step(NativeUnion value) {
  value.bits += 13;
  return value;
}
int32_t native_union_check(NativeUnion value, uint64_t bits) {
  return value.bits == bits;
}
UnionBox native_union_box(uint64_t bits) {
  return (UnionBox){native_union(bits), 17};
}
UnionBox native_union_box_step(UnionBox value) {
  value.value.bits += 19;
  value.tag += 3;
  return value;
}
int32_t native_union_box_check(UnionBox value, uint64_t bits) {
  return value.value.bits == bits && value.tag == 20;
}
NativeOp native_obtain_operation(void) { return plus11; }
int32_t native_replace_operation(int32_t value) {
  native_operation = plus29;
  return value;
}
CallBox native_call_box(void) { return (CallBox){plus11, 37}; }
int32_t native_call_box_check(CallBox value) {
  return value.apply(7) == 18 && value.cookie == 37;
}
void native_bump(void) { native_counter += 7; }
int32_t native_state_check(void) {
  return native_counter == 13 && native_state.pair.a == 10 &&
         native_state.pair.b == 15 && native_state.weight == 3.75;
}
uint64_t native_text(gti_c_string_view value) {
  return value.length == 5 && value.data[0] == 'h' && value.data[4] == 'o';
}
uint32_t native_cstring(const char *value) {
  return value[0] == 'a' && value[1] == 'b' && value[2] == 'c' && value[3] == 0;
}
long long native_long_long(long long value) { return value + 17; }
unsigned long native_unsigned_long(unsigned long value) { return value + 19; }
char native_char(char value) { return (char)(value + 1); }
int32_t native_mark(int32_t value) {
  printf("mark:%d\n", value);
  fflush(stdout);
  return value;
}
int32_t native_zero(void) { return 0; }
void native_enter(int32_t a, int32_t b) {
  printf("entered:%d,%d\n", a, b);
  fflush(stdout);
}
