#include <stdint.h>
#include <stddef.h>
typedef struct { uint64_t a, b; } CallbackPair;
typedef struct { uint64_t values[3]; } CallbackLarge;
typedef struct { double values[4]; } CallbackHfa;
typedef struct { const char *data; size_t size; } View;
typedef int32_t (*Scalar)(int32_t);
typedef CallbackPair (*PairOperation)(CallbackPair);
typedef CallbackLarge (*LargeOperation)(CallbackLarge);
typedef CallbackHfa (*HfaOperation)(CallbackHfa);
typedef uint64_t (*ViewOperation)(View);
typedef Scalar (*Selector)(Scalar);
typedef struct { PairOperation pair; Scalar scalar; } CallbackTable;
CallbackTable native_callback_slots;
PairOperation native_callback_record_slot;
int32_t native_callback_scalar(Scalar operation, int32_t value) { return operation(value); }
int32_t native_callback_pair(PairOperation operation) {
  CallbackPair result = operation((CallbackPair){17, 31});
  return result.a == 17 && result.b == 31;
}
int32_t native_callback_large(LargeOperation operation) {
  CallbackLarge result = operation((CallbackLarge){{17, 31, 49}});
  return result.values[0] == 17 && result.values[1] == 31 && result.values[2] == 49;
}
int32_t native_callback_hfa(HfaOperation operation) {
  CallbackHfa result = operation((CallbackHfa){{1.25, 3.5, 7.75, 11.5}});
  return result.values[0] == 1.25 && result.values[1] == 3.5 &&
         result.values[2] == 7.75 && result.values[3] == 11.5;
}
int32_t native_callback_view(ViewOperation operation) {
  return operation((View){"abc\0def", 7}) == 7;
}
PairOperation native_callback_pair_identity(PairOperation operation) { return operation; }
static Scalar select_operation(Scalar operation) { return operation; }
Selector native_callback_selector(void) { return select_operation; }
CallbackPair native_callback_pair_make(void) { return (CallbackPair){17, 31}; }
CallbackTable native_callback_table(PairOperation pair, Scalar scalar) {
  return (CallbackTable){pair, scalar};
}
int32_t native_callback_table_check(CallbackTable value) {
  return native_callback_pair(value.pair) && value.scalar(35) == 42;
}
