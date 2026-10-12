#include "gti/memory.h"

// GTI source owns scanning, copying, allocation and error policy. Keep this
// representation bridge separate from the replaceable allocation provider.
extern "C" const uint8_t *gti_rt_c_string_bytes_v1(const char *text) {
  return reinterpret_cast<const uint8_t *>(text);
}
