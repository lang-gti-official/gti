#include "native.h"

#ifdef __cplusplus
using foreign::gti_external_apply;
using foreign::gti_external_length;
using foreign::gti_external_length_factory;
#endif

int main(void) {
  Length callback = gti_external_length_factory();
  return gti_external_length("header") == UINT64_C(6) &&
                 gti_external_apply(callback, "header") == UINT64_C(6)
             ? 0
             : 1;
}
