#pragma once

#include "gti/random.h"
#include "gti/runtime.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace gti_internal::runtime {

inline void write_stdout(std::string_view value) {
  (void)gti_rt_write_stdout(
      gti_c_string_view{.data = value.data(),
                        .length = static_cast<std::uint64_t>(value.size())});
}

inline std::int32_t write_stdout_byte(std::uint8_t value) {
  return gti_rt_write_stdout_byte(value);
}

inline std::int32_t read_stdin_byte() { return gti_rt_read_stdin_byte(); }

inline std::int64_t open_file_read(std::string_view path) {
  return gti_rt_open_file_read(gti_c_string_view{
      .data = path.data(), .length = static_cast<std::uint64_t>(path.size())});
}

inline std::int32_t read_file_byte(std::int64_t descriptor) {
  return gti_rt_read_file_byte(descriptor);
}

inline std::int32_t close_file(std::int64_t descriptor) {
  return gti_rt_close_file(descriptor);
}

// RANDOM
inline std::int32_t random_bytes(uint8_t *out, uint64_t count) {
  return gti_rt_random_bytes(out, count);
}
} // namespace gti_internal::runtime
