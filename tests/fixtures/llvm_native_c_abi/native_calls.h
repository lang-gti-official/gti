#ifndef GTI_LLVM_NATIVE_C_CALLS_H
#define GTI_LLVM_NATIVE_C_CALLS_H

#include "gti/c_abi.h"
#include <stdbool.h>
#include <stdint.h>

typedef int32_t (*NativeOp)(int32_t);
typedef struct Pair {
  uint64_t a, b;
} Pair;
typedef struct Nested {
  Pair pair;
  double weight;
} Nested;
typedef struct FloatArray {
  float values[4];
} FloatArray;
typedef struct DoubleArray {
  double values[4];
} DoubleArray;
typedef struct Boolean {
  bool active;
  uint16_t code;
} Boolean;
typedef union NativeUnion {
  uint64_t bits;
  double floating;
} NativeUnion;
typedef struct UnionBox {
  NativeUnion value;
  uint32_t tag;
} UnionBox;
typedef struct CallBox {
  NativeOp apply;
  int32_t cookie;
} CallBox;

extern int32_t native_counter;
extern const int32_t native_limit;
extern NativeOp native_operation;
extern Nested native_state;
extern int32_t *native_pointer;
Pair native_pair(uint64_t, uint64_t);
Pair native_pair_step(Pair);
int32_t native_pair_check(Pair, uint64_t, uint64_t);
Nested native_nested(uint64_t, double);
Nested native_nested_step(Nested);
int32_t native_nested_check(Nested, uint64_t, double);
FloatArray native_float_array(void);
FloatArray native_float_step(FloatArray);
int32_t native_float_check(FloatArray);
DoubleArray native_double_array(void);
DoubleArray native_double_step(DoubleArray);
int32_t native_double_check(DoubleArray);
Boolean native_boolean(bool, uint16_t);
Boolean native_boolean_step(Boolean);
NativeUnion native_union(uint64_t);
NativeUnion native_union_step(NativeUnion);
int32_t native_union_check(NativeUnion, uint64_t);
UnionBox native_union_box(uint64_t);
UnionBox native_union_box_step(UnionBox);
int32_t native_union_box_check(UnionBox, uint64_t);
NativeOp native_obtain_operation(void);
int32_t native_replace_operation(int32_t);
CallBox native_call_box(void);
int32_t native_call_box_check(CallBox);
void native_bump(void);
int32_t native_state_check(void);
uint64_t native_text(gti_c_string_view);
uint32_t native_cstring(const char *);
long long native_long_long(long long);
unsigned long native_unsigned_long(unsigned long);
char native_char(char);
int32_t native_mark(int32_t);
int32_t native_zero(void);
void native_enter(int32_t, int32_t);

#endif
