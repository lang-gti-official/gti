#include "native_calls.h"
#include <stdio.h>
extern int32_t llvm_storage_roundtrip(int32_t value);
extern int32_t llvm_storage_apply(int32_t value);
extern int32_t llvm_storage_pointer(void);
extern int32_t llvm_storage_limit(void);
static int32_t minus_nine(int32_t value) { return value - 9; }
int main(void) {
  if (llvm_storage_roundtrip(17) != 24 || native_counter != 24 ||
      llvm_storage_pointer() != 24 || llvm_storage_limit() != 73)
    return 1;
  if (llvm_storage_apply(7) != 18)
    return 2;
  native_operation = minus_nine;
  if (llvm_storage_apply(7) != -2)
    return 3;
  puts("native C foreign storage contracts");
  return 0;
}
