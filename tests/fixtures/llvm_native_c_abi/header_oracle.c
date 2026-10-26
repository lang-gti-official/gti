#include "gti.native.h"
#include <stddef.h>

_Static_assert(sizeof(Pair) == 16 && _Alignof(Pair) == 8, "Pair");
_Static_assert(sizeof(Nested) == 24 && offsetof(Nested, weight) == 16,
               "Nested");
_Static_assert(sizeof(FloatArray) == 16 && _Alignof(FloatArray) == 4,
               "float array");
_Static_assert(sizeof(DoubleArray) == 32 && _Alignof(DoubleArray) == 8,
               "double HFA");
_Static_assert(sizeof(Boolean) == 4 && offsetof(Boolean, code) == 2,
               "bool storage");
_Static_assert(sizeof(NativeUnion) == 8 && offsetof(NativeUnion, floating) == 0,
               "union");
_Static_assert(sizeof(UnionBox) == 16 && offsetof(UnionBox, tag) == 8,
               "nested union");
_Static_assert(sizeof(CallBox) == 16 && offsetof(CallBox, cookie) == 8,
               "function field");

int native_header_oracle(void) {
  Pair (*pair)(Pair) = native_pair_step;
  Nested (*nested)(Nested) = native_nested_step;
  int32_t (*(*operation)(void))(int32_t) = native_obtain_operation;
  long long (*ranked)(long long) = native_long_long;
  unsigned long (*native_long)(unsigned long) = native_unsigned_long;
  return pair != 0 && nested != 0 && operation != 0 && ranked != 0 &&
         native_long != 0;
}
