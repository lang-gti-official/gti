#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
typedef int32_t (*NativeOperation)(int32_t);
typedef struct { int32_t count; double weight; } StorageCell;
typedef union { uint64_t number; double real; } StorageBits;
typedef struct { StorageCell cells[2]; StorageBits bits; } StorageState;
typedef struct StorageToken { int32_t value; } StorageToken;
int32_t native_counter;
int32_t native_external_epoch;
const int32_t native_limit = 17;
unsigned int native_flags;
bool native_enabled;
float native_scale;
static int32_t target = 91;
int32_t *native_pointer;
static StorageToken token = {123};
StorageToken *native_token;
StorageState native_state;
static int32_t original(int32_t value) { printf("entered-original:%d\n", value); return value + 11; }
static int32_t replaced(int32_t value) { printf("entered-replaced:%d\n", value); return value + 29; }
NativeOperation native_operation;
void native_storage_reset(void) {
  native_external_epoch = 43; native_counter = 10; native_flags = 9; native_enabled = false;
  native_scale = 1; native_pointer = &target; native_token = &token;
  native_operation = original;
  native_state = (StorageState){{{4, 1.25}, {5, 2.5}}, {.number = 6}};
}
void native_storage_bump(void) { native_counter += 5; }
int32_t native_storage_check(int32_t *address) {
  return address == &native_counter && native_counter == 28;
}
int32_t native_storage_swap(int32_t value) {
  puts("swap"); native_operation = replaced; return value;
}
StorageCell native_storage_cell(int32_t value) { return (StorageCell){value, 3.5}; }
StorageBits native_storage_bits(uint64_t value) { return (StorageBits){.number = value}; }
int32_t native_storage_token(StorageToken *value) { return value == &token && value->value == 123; }
int32_t native_storage_zero(void) { puts("zero"); return 0; }
void native_storage_cleanup(void) { puts("storage-cleanup"); }
int32_t native_storage_verify(void) {
  return native_external_epoch == 44 && native_counter == 28 && native_flags == 12 && native_enabled &&
         native_scale == 2 && native_pointer == &native_counter &&
         native_state.cells[0].count == 41 && native_state.cells[0].weight == 1.25 &&
         native_state.cells[1].count == 73 && native_state.cells[1].weight == 3.5 &&
         native_state.bits.number == 81 && native_operation == replaced;
}
