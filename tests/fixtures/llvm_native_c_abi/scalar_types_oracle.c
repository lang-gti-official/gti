#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool scalar_bool(bool value) { return !value; }
int8_t scalar_i8(int8_t value) { return (int8_t)(value + 1); }
uint8_t scalar_u8(uint8_t value) { return (uint8_t)(value + 1); }
int16_t scalar_i16(int16_t value) { return (int16_t)(value + 1); }
uint16_t scalar_u16(uint16_t value) { return (uint16_t)(value + 1); }
int32_t scalar_i32(int32_t value) { return value + 1; }
uint32_t scalar_u32(uint32_t value) { return value + 1; }
int64_t scalar_i64(int64_t value) { return value + 1; }
uint64_t scalar_u64(uint64_t value) { return value + 1; }
float scalar_f32(float value) { return value + 0.25f; }
double scalar_f64(double value) { return value + 0.25; }
char scalar_char(char value) { return (char)(value + 1); }
long long scalar_long_long(long long value) { return value + 1; }
unsigned long scalar_unsigned_long(unsigned long value) { return value + 1; }
const char *scalar_string(const char *value) { return value; }
int32_t scalar_string_check(const char *value) {
  return strcmp(value, "native scalar") == 0;
}
static int32_t native_value = 19;
int32_t *scalar_pointer(void) { return &native_value; }
int32_t scalar_pointer_check(int32_t *value) { return value == &native_value; }
static int32_t touched;
void scalar_touch(int32_t value) { touched = value; }
int32_t scalar_touched(void) { return touched; }
