#include "gti/environment.h"

int main(void) {
  int64_t snapshot = 42;
  const gti_c_string_view invalid = {0, 0};
  if (gti_rt_environment_get_v1(invalid, &snapshot) !=
          GTI_ENVIRONMENT_INVALID_NAME_V1 ||
      snapshot != 0)
    return 1;
  gti_rt_environment_snapshot_destroy_v1(snapshot);
  return 0;
}
