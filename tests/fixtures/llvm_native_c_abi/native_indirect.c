#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef int32_t (*Unary)(int32_t);
typedef int16_t (*Narrow)(int8_t, uint16_t);
typedef double (*Floating)(float, double);
typedef int32_t *(*Pointer)(int32_t *);
typedef long long (*NativeScalar)(long, char, bool);
typedef void (*Selected)(int32_t, int32_t);
static int32_t unary(int32_t value) { return value + 11; }
Unary native_unary(void) { return unary; }
Unary native_absent(void) { return NULL; }
static int16_t narrow(int8_t value, uint16_t check) {
  return check == UINT16_C(54321) ? (int16_t)(value + 1) : 0;
}
Narrow native_narrow(void) { return narrow; }
static double floating(float first, double second) { return first + second; }
Floating native_floating(void) { return floating; }
static int32_t *pointer(int32_t *value) { return value; }
Pointer native_pointer_operation(void) { return pointer; }
static int32_t pointed = 73;
int32_t *native_pointer_value(void) { return &pointed; }
static long long native_scalar(long value, char tag, bool enabled) {
  return tag == 97 && enabled ? (long long)value + 1 : 0;
}
NativeScalar native_scalar_operation(void) { return native_scalar; }
static void original(int32_t first, int32_t second) {
  printf("entered:%d,%d\n", (int)first, (int)second);
}
static void replaced(int32_t first, int32_t second) {
  printf("wrong-target:%d,%d\n", (int)first, (int)second);
}
static Selected selected = original;
Selected native_select_operation(void) {
  puts("selected");
  return selected;
}
int32_t native_mark(int32_t value) {
  printf("mark:%d\n", (int)value);
  if (value == 1)
    selected = replaced;
  return value;
}
int32_t native_zero(void) { return 0; }
typedef struct NativeRecord {
  int32_t value;
} NativeRecord;
int32_t consume_record(NativeRecord value) { return value.value; }
typedef NativeRecord (*RecordOperation)(NativeRecord);
static NativeRecord record_operation(NativeRecord value) {
  value.value += 1;
  return value;
}
RecordOperation obtain_operation(void) { return record_operation; }
static NativeRecord record_value = {1};
NativeRecord *obtain_record_pointer(void) { return &record_value; }
