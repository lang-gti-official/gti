#pragma once

#include "gti/runtime.h"

#include <string>

namespace gti_internal::runtime {

inline void write_stdout(const std::string &value) {
  (void)gti_rt_write_stdout(value.data(), value.size());
}

} // namespace gti_internal::runtime
