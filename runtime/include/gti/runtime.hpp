#pragma once

#include "gti/runtime.h"

#include <string_view>

namespace gti_internal::runtime {

inline void write_stdout(std::string_view value) {
  (void)gti_rt_write_stdout(value.data(), value.size());
}

} // namespace gti_internal::runtime
