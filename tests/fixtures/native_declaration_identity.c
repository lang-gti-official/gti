#include <stdint.h>
#include <string.h>

typedef uint64_t (*length_callback)(const char *);

uint64_t gti_external_length(const char *text) {
  return (uint64_t)strlen(text);
}

length_callback gti_external_length_factory(void) {
  return &gti_external_length;
}

uint64_t gti_external_apply(length_callback callback, const char *text) {
  return callback(text);
}
