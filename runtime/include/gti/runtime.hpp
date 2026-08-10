#pragma once

#include "gti/runtime.h"

#include <cstdint>
#include <string_view>

namespace gti_internal::runtime {

inline void write_stdout(std::string_view value) {
  (void)gti_rt_write_stdout(gti_c_string_view{
      value.data(), static_cast<std::uint64_t>(value.size())});
}

inline std::int32_t read_stdin_byte() { return gti_rt_read_stdin_byte(); }

inline std::int64_t open_file_read(std::string_view path) {
  return gti_rt_open_file_read(
      gti_c_string_view{path.data(), static_cast<std::uint64_t>(path.size())});
}

inline std::int32_t read_file_byte(std::int64_t descriptor) {
  return gti_rt_read_file_byte(descriptor);
}

inline std::int32_t close_file(std::int64_t descriptor) {
  return gti_rt_close_file(descriptor);
}

} // namespace gti_internal::runtime
