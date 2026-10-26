#include <gti/c_abi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef struct {
  int8_t value;
} NativeByte;
typedef struct {
  uint8_t a;
  uint8_t b;
  uint8_t c;
} NativeTiny;
typedef struct {
  int8_t tag;
  uint16_t extent;
  int32_t count;
  double ratio;
} NativePacket;
typedef struct {
  uint64_t values[3];
} NativeLarge;
typedef struct {
  float values[4];
} NativeHfa;
typedef union {
  uint64_t bits;
  double real;
} NativeUnion;
typedef struct {
  NativeUnion selected;
  uint32_t tag;
} NativeBox;
typedef struct {
  NativePacket packet;
  NativeHfa hfa;
} NativeNested;
typedef struct {
  int32_t *address;
  uint32_t tag;
} NativePointer;
typedef struct {
  long rank;
  unsigned long flags;
  bool ready;
} NativeAliases;
NativeByte native_byte_make(int8_t value) { return (NativeByte){value}; }
NativeByte native_byte_step(NativeByte value) {
  ++value.value;
  return value;
}
NativeTiny native_tiny_make(void) { return (NativeTiny){1, 127, 255}; }
NativeTiny native_tiny_step(NativeTiny value) {
  value.c ^= 1;
  return value;
}
int32_t native_record_counted(NativePacket value, gti_c_string_view text) {
  return value.count == 41 && text.length == 6 && text.data &&
         text.data[0] == 'n' && text.data[5] == 'e';
}
int32_t native_record_mixed(NativePacket value, gti_c_string_view text,
                            int32_t last) {
  printf("entered-view:%d,%llu,%d\n", (int)value.count,
         (unsigned long long)text.length, (int)last);
  return value.count == 1 && text.length == 6 && text.data &&
         text.data[0] == 'n' && text.data[5] == 'e' && last == 3;
}
typedef int32_t (*NativeUnary)(int32_t);
typedef struct {
  NativeUnary operation;
} NativeHook;
static int32_t native_hook_target(int32_t value) { return value + 11; }
NativeHook native_unadmitted_hook(void) {
  return (NativeHook){native_hook_target};
}
NativeUnary native_record_unary(void) { return native_hook_target; }
int32_t native_record_with_callback(NativePacket value, NativeUnary operation) {
  return value.count == 41 && operation && operation(31) == 42;
}
typedef struct {
  int32_t value;
} LegacyRecord;
static LegacyRecord legacy = {41};
LegacyRecord obtain_record(void) { return legacy; }
LegacyRecord *obtain_record_pointer(void) { return &legacy; }
int32_t consume_record(LegacyRecord value) { return value.value; }
NativePacket native_packet_make(int32_t count) {
  return (NativePacket){-7, 54321, count, 2.5};
}
NativePacket native_packet_step(NativePacket value) {
  ++value.count;
  value.ratio += 1.25;
  return value;
}
int32_t native_packet_check(NativePacket value, int32_t count) {
  return value.tag == -7 && value.extent == 54321 && value.count == count &&
         value.ratio == 2.5;
}
int32_t native_packet_pressure(int64_t a, int64_t b, int64_t c, int64_t d,
                               int64_t e, int64_t f, NativePacket value,
                               double suffix) {
  return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 &&
         value.tag == -7 && value.extent == 54321 && value.count == 42 &&
         value.ratio == 3.75 && suffix == 9.5;
}
NativeLarge native_large_make(uint64_t first) {
  return (NativeLarge){{first, first + 1, first + 2}};
}
NativeLarge native_large_step(NativeLarge value) {
  for (unsigned i = 0; i < 3; ++i)
    value.values[i] += 7;
  return value;
}
int32_t native_large_pressure(int64_t a, int64_t b, int64_t c, int64_t d,
                              int64_t e, int64_t f, NativeLarge value,
                              int64_t suffix) {
  return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 &&
         value.values[0] == 24 && value.values[1] == 25 &&
         value.values[2] == 26 && suffix == 9;
}
NativeHfa native_hfa_make(void) {
  return (NativeHfa){{1.25f, 2.5f, 3.75f, 5.0f}};
}
NativeHfa native_hfa_step(NativeHfa value) {
  for (unsigned i = 0; i < 4; ++i)
    value.values[i] += 1.0f;
  return value;
}
int32_t native_hfa_pressure(double a, double b, double c, double d, double e,
                            double f, double g, double h, NativeHfa value,
                            double suffix) {
  return a == 1 && b == 2 && c == 3 && d == 4 && e == 5 && f == 6 && g == 7 &&
         h == 8 && value.values[0] == 2.25f && value.values[3] == 6.0f &&
         suffix == 9.5;
}
NativeUnion native_union_make(void) { return (NativeUnion){.bits = 1234567}; }
NativeUnion native_union_step(NativeUnion value) {
  ++value.bits;
  return value;
}
NativeBox native_box_make(void) { return (NativeBox){{.bits = 1234567}, 99}; }
NativeBox native_box_step(NativeBox value) {
  ++value.selected.bits;
  ++value.tag;
  return value;
}
NativeNested native_nested_make(void) {
  return (NativeNested){native_packet_make(41), native_hfa_make()};
}
NativeNested native_nested_step(NativeNested value) {
  value.packet = native_packet_step(value.packet);
  value.hfa = native_hfa_step(value.hfa);
  return value;
}
static int32_t pointed = 73;
NativePointer native_pointer_make(void) {
  return (NativePointer){&pointed, 99};
}
int32_t native_pointer_check(NativePointer value) {
  return value.address == &pointed && *value.address == 73 && value.tag == 99;
}
NativeAliases native_alias_make(void) {
  return (NativeAliases){-7, 54321, true};
}
int32_t native_alias_check(NativeAliases value) {
  return value.rank == -7 && value.flags == 54321 && value.ready;
}
NativePacket native_record_mark_packet(int32_t count) {
  printf("record:%d\n", (int)count);
  return native_packet_make(count);
}
void native_record_consume_pair(NativePacket first, NativePacket second) {
  printf("entered:%d,%d\n", (int)first.count, (int)second.count);
}
int32_t native_record_mark(int32_t value) {
  printf("mark:%d\n", (int)value);
  return value;
}
int32_t native_record_zero(void) { return 0; }
