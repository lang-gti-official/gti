#include "gti/runtime.h"

#include <stdint.h>

_Static_assert(sizeof(((gti_c_string_view *)0)->length) == sizeof(uint64_t),
               "the GTI C ABI string-view length must remain uint64_t");
_Static_assert(
    _Generic(&gti_rt_write_stdout,
        int32_t (*)(gti_c_string_view): 1,
        default: 0),
    "the stdout runtime entry must consume the public counted-text record");
_Static_assert(
    _Generic(&gti_rt_open_file_read,
        int64_t (*)(gti_c_string_view): 1,
        default: 0),
    "the file-open runtime entry must consume the public counted-text record");
_Static_assert(_Generic(&gti_rt_read_stdin_byte,
                   int32_t (*)(void): 1,
                   default: 0),
               "the stdin runtime entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_read_file_byte,
                   int32_t (*)(int64_t): 1,
                   default: 0),
               "the file-read runtime entry must retain its C prototype");
_Static_assert(_Generic(&gti_rt_close_file,
                   int32_t (*)(int64_t): 1,
                   default: 0),
               "the file-close runtime entry must retain its C prototype");

int main(void) {
  static const char text[] = "gti";
  const gti_c_string_view view = {.data = text, .length = 3};
  return view.data == text && view.length == UINT64_C(3) ? 0 : 1;
}
