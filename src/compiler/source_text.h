#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace lang::source_text {

inline constexpr std::string_view utf8Bom = "\xEF\xBB\xBF";

struct Utf8Scalar {
  std::uint32_t value = 0;
  std::size_t length = 1;
  bool valid = false;
};

[[nodiscard]] inline Utf8Scalar decodeUtf8(std::string_view source,
                                           std::size_t offset) {
  if (offset >= source.size()) {
    return {};
  }

  const auto byte = [&](std::size_t index) {
    return static_cast<unsigned char>(source[index]);
  };
  const unsigned char first = byte(offset);
  if (first <= 0x7FU) {
    return {.value = first, .length = 1, .valid = true};
  }

  std::size_t length = 0;
  std::uint32_t value = 0;
  if (first >= 0xC2U && first <= 0xDFU) {
    length = 2;
    value = first & 0x1FU;
  } else if (first >= 0xE0U && first <= 0xEFU) {
    length = 3;
    value = first & 0x0FU;
  } else if (first >= 0xF0U && first <= 0xF4U) {
    length = 4;
    value = first & 0x07U;
  } else {
    return {};
  }

  if (offset + length > source.size()) {
    return {};
  }
  for (std::size_t index = 1; index < length; ++index) {
    const unsigned char continuation = byte(offset + index);
    if ((continuation & 0xC0U) != 0x80U) {
      return {};
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }

  const unsigned char second = byte(offset + 1);
  if ((first == 0xE0U && second < 0xA0U) ||
      (first == 0xEDU && second > 0x9FU) ||
      (first == 0xF0U && second < 0x90U) ||
      (first == 0xF4U && second > 0x8FU)) {
    return {};
  }

  return {.value = value, .length = length, .valid = true};
}

[[nodiscard]] inline std::optional<std::size_t>
firstInvalidUtf8(std::string_view source) {
  for (std::size_t offset = 0; offset < source.size();) {
    const Utf8Scalar scalar = decodeUtf8(source, offset);
    if (!scalar.valid) {
      return offset;
    }
    offset += scalar.length;
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::size_t>
firstLoneCarriageReturn(std::string_view source) {
  for (std::size_t offset = 0; offset < source.size(); ++offset) {
    if (source[offset] == '\r' &&
        (offset + 1 >= source.size() || source[offset + 1] != '\n')) {
      return offset;
    }
  }
  return std::nullopt;
}

[[nodiscard]] inline bool hasUtf8Bom(std::string_view source) {
  return source.starts_with(utf8Bom);
}

} // namespace lang::source_text
