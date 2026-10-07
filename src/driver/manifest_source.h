#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace lang::driver::detail {
// toml++ positions count Unicode scalars, while GTI spans/edits count bytes.
// Parsing guarantees valid UTF-8 before either manifest consumer uses this.
inline std::size_t manifestSourceOffset(std::string_view source,
                                        std::uint32_t line,
                                        std::uint32_t column) {
  if (!line || !column) {
    return 0;
  }
  std::size_t offset = 0;
  for (std::uint32_t current = 1; current < line; ++current) {
    const auto newline = source.find('\n', offset);
    if (newline == source.npos) {
      return source.size();
    }
    offset = newline + 1;
  }
  for (std::uint32_t current = 1;
       current < column && offset < source.size() && source[offset] != '\n';
       ++current) {
    const auto byte = static_cast<unsigned char>(source[offset]);
    const std::size_t length = byte < 0x80               ? 1
                               : (byte & 0xe0U) == 0xc0U ? 2
                               : (byte & 0xf0U) == 0xe0U ? 3
                               : (byte & 0xf8U) == 0xf0U ? 4
                                                         : 1;
    offset += std::min(length, source.size() - offset);
  }
  return offset;
}
} // namespace lang::driver::detail
