#include "gti/charconv.h"

#include <charconv>
#include <cstddef>
#include <limits>
#include <system_error>

namespace {
template <typename T>
uint32_t parse(gti_c_string_view input, uint64_t offset, uint32_t format,
               T *output, uint64_t *position) {
  if (output == nullptr || position == nullptr || offset > input.length ||
      input.length >
          static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max()) ||
      (input.data == nullptr && input.length != 0) || format > 3) {
    return 1;
  }
  const auto style = format == 1   ? std::chars_format::fixed
                     : format == 2 ? std::chars_format::scientific
                     : format == 3 ? std::chars_format::hex
                                   : std::chars_format::general;
  // Even zero-offset pointer arithmetic requires a nonnull base.
  const char *base = input.data == nullptr ? "" : input.data;
  T value{};
  const auto result =
      std::from_chars(base + offset, base + input.length, value, style);
  *position = static_cast<uint64_t>(result.ptr - base);
  if (result.ec == std::errc::invalid_argument) {
    return 1;
  }
  if (result.ec == std::errc::result_out_of_range) {
    return 2;
  }
  if (result.ec != std::errc{}) {
    return 1;
  }
  *output = value;
  return 0;
}
} // namespace

extern "C" uint32_t gti_rt_parse_f32_v1(gti_c_string_view input,
                                        uint64_t offset, uint32_t format,
                                        float *output, uint64_t *position) {
  return parse(input, offset, format, output, position);
}
extern "C" uint32_t gti_rt_parse_f64_v1(gti_c_string_view input,
                                        uint64_t offset, uint32_t format,
                                        double *output, uint64_t *position) {
  return parse(input, offset, format, output, position);
}
