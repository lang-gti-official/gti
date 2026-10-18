#include "raw_record_containers.h"

extern "C" NativeAddress empty_native() { return {nullptr, 0}; }

static int calls = 0;
extern "C" NativeAddress counted_native(int stamp) {
  ++calls;
  return {nullptr, stamp};
}
extern "C" int native_call_count() { return calls; }
