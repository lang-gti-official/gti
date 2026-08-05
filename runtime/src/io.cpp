#include "gti/runtime.h"

#include <cstdio>

extern "C" int gti_rt_write_stdout(const char *data, size_t length) {
  if (data == nullptr && length != 0) {
    return 1;
  }
  return std::fwrite(data, 1, length, stdout) == length ? 0 : 1;
}
