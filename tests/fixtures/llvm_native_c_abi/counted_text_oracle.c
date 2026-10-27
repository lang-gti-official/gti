#include <gti/c_abi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(void *) == 8 && sizeof(gti_c_string_view) == 16,
               "accepted counted C record extent");
_Static_assert(_Alignof(gti_c_string_view) == 8 &&
                   offsetof(gti_c_string_view, data) == 0 &&
                   offsetof(gti_c_string_view, length) == 8,
               "accepted counted C record layout");

int32_t consume_text(gti_c_string_view value) {
  if (value.length == 0)
    return 100;
  if (!value.data)
    return -1;
  if (value.length == 5 && memcmp(value.data, "plain", 5) == 0)
    return 101;
  if (value.length == 3 && memcmp(value.data, "A\0B", 3) == 0)
    return 102;
  if (value.length == 4 && memcmp(value.data, "\xc3\xa9\xce\xbb", 4) == 0)
    return 103;
  return -2;
}
int32_t consume_text_pressure(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                              uint64_t e, gti_c_string_view value,
                              uint64_t tail) {
  return consume_text(value) + (int32_t)(a + b + c + d + e + tail);
}
int32_t close_counted_stdout(void) {
  if (setvbuf(stdout, NULL, _IONBF, 0) != 0)
    return -1;
  return close(STDOUT_FILENO);
}
void counted_cleanup(void) { fputs("counted-cleanup\n", stderr); }

#ifndef GTI_SOURCE_HOST
int32_t bridge_consume_text(gti_c_string_view);
int32_t bridge_consume_text_pressure(uint64_t, uint64_t, uint64_t, uint64_t,
                                     uint64_t, gti_c_string_view, uint64_t);
int32_t bridge_gti_rt_write_stdout(gti_c_string_view);
int main(int argc, char **argv) {
  const gti_c_string_view values[] = {
      {NULL, 0}, {"plain", 5}, {"A\0B", 3}, {"\xc3\xa9\xce\xbb", 4}};
  if (argc == 2 && strcmp(argv[1], "write-failure") == 0) {
    if (close_counted_stdout() != 0)
      return 20;
    return bridge_gti_rt_write_stdout(values[1]) != 0 ? 0 : 21;
  }
  for (unsigned i = 0; i < 4; ++i)
    if (bridge_consume_text(values[i]) != (int32_t)(100 + i))
      return 1;
  if (bridge_consume_text_pressure(1, 2, 3, 4, 5, values[2], 17) != 134)
    return 2;
  if (bridge_gti_rt_write_stdout(values[0]) != 0 ||
      bridge_gti_rt_write_stdout(values[2]) != 0 ||
      bridge_gti_rt_write_stdout(values[3]) != 0)
    return 3;
  return 0;
}
#endif
