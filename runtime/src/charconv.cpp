#include "gti/charconv.h"

#include <charconv>
#include <cstddef>
#include <limits>
#include <string>
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

template <typename T>
uint32_t formatValue(T value, uint32_t format, int32_t precision,
                     uint32_t flags, uint32_t fill, uint32_t alignment,
                     uint32_t sign, uint32_t zeroPad, uint64_t width,
                     uint8_t *output, uint64_t capacity, uint64_t *size) {
  if (size == nullptr || (output == nullptr && capacity != 0) || format > 3 ||
      precision < -1 || flags > 7 || fill > 255 || alignment > 255 ||
      sign > 255 || zeroPad > 1 ||
      (alignment != 0 && alignment != '<' && alignment != '>' &&
       alignment != '^') ||
      (sign != '-' && sign != '+' && sign != ' ') ||
      capacity > static_cast<uint64_t>(std::numeric_limits<ptrdiff_t>::max())) {
    return 1;
  }
  const auto style = format == 1   ? std::chars_format::fixed
                     : format == 2 ? std::chars_format::scientific
                     : format == 3 ? std::chars_format::hex
                                   : std::chars_format::general;
  try {
    char native[4096]{};
    const auto result =
        precision < 0
            ? std::to_chars(native, native + sizeof(native), value, style)
            : std::to_chars(native, native + sizeof(native), value, style,
                            precision);
    if (result.ec == std::errc::value_too_large) {
      return 2;
    }
    if (result.ec != std::errc{}) {
      return 1;
    }

    std::string digits(native, result.ptr);
    char prefix = '\0';
    if (!digits.empty() && digits.front() == '-') {
      prefix = '-';
      digits.erase(digits.begin());
    } else if (sign == '+' || sign == ' ') {
      prefix = static_cast<char>(sign);
    }

    const bool uppercase = (flags & 1U) != 0;
    if (uppercase) {
      for (char &current : digits) {
        if ((current >= 'a' && current <= 'f') || current == 'i' ||
            current == 'n' || current == 'p') {
          current = static_cast<char>(current - ('a' - 'A'));
        }
      }
    }

    const bool alternate = (flags & 2U) != 0;
    const bool general = (flags & 4U) != 0;
    const bool special =
        !digits.empty() && (digits.front() == 'i' || digits.front() == 'I' ||
                            digits.front() == 'n' || digits.front() == 'N');
    if (alternate && !special) {
      auto exponent = digits.find_first_of("eEpP");
      if (exponent == std::string::npos) {
        exponent = digits.size();
      }
      if (digits.find('.', 0) >= exponent) {
        digits.insert(exponent, 1, '.');
        ++exponent;
      }
      if (general && precision >= 0) {
        std::size_t significantDigits = 0;
        bool significant = false;
        for (std::size_t index = 0; index < exponent; ++index) {
          const char current = digits[index];
          if (current < '0' || current > '9') {
            continue;
          }
          if (current != '0' || significant) {
            significant = true;
            ++significantDigits;
          }
        }
        if (!significant) {
          significantDigits = 1;
        }
        const std::size_t desired = static_cast<std::size_t>(precision);
        while (significantDigits < desired) {
          digits.insert(exponent, 1, '0');
          ++exponent;
          ++significantDigits;
        }
      }
    }

    const uint64_t content = static_cast<uint64_t>(digits.size()) +
                             static_cast<uint64_t>(prefix != '\0');
    const uint64_t padding = width > content ? width - content : 0;
    if (content > capacity || padding > capacity - content) {
      return 2;
    }
    const char fillCharacter = static_cast<char>(fill);
    const char effectiveAlignment =
        alignment == 0 ? '>' : static_cast<char>(alignment);
    // C++ numeric zero padding does not apply to infinity or NaN. Those
    // spellings retain ordinary right alignment with the active fill, while
    // finite signed values keep zeros between the sign and magnitude.
    const bool numericZeroPad = zeroPad != 0 && alignment == 0 && !special;
    uint64_t left = 0;
    uint64_t right = 0;
    if (numericZeroPad) {
      left = 0;
    } else if (effectiveAlignment == '<') {
      right = padding;
    } else if (effectiveAlignment == '^') {
      left = padding / 2;
      right = padding - left;
    } else {
      left = padding;
    }

    uint64_t position = 0;
    const auto append = [&](char value) {
      output[position] = static_cast<uint8_t>(value);
      ++position;
    };
    if (!numericZeroPad) {
      for (uint64_t index = 0; index < left; ++index) {
        append(fillCharacter);
      }
    }
    if (prefix != '\0') {
      append(prefix);
    }
    if (numericZeroPad) {
      for (uint64_t index = 0; index < padding; ++index) {
        append('0');
      }
    }
    for (const char current : digits) {
      append(current);
    }
    for (uint64_t index = 0; index < right; ++index) {
      append(fillCharacter);
    }
    *size = position;
    return 0;
  } catch (...) {
    return 2;
  }
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

extern "C" uint32_t gti_rt_format_f32_v1(float value, uint32_t format,
                                         int32_t precision, uint32_t flags,
                                         uint32_t fill, uint32_t alignment,
                                         uint32_t sign, uint32_t zeroPad,
                                         uint64_t width, uint8_t *output,
                                         uint64_t capacity, uint64_t *size) {
  return formatValue(value, format, precision, flags, fill, alignment, sign,
                     zeroPad, width, output, capacity, size);
}

extern "C" uint32_t gti_rt_format_f64_v1(double value, uint32_t format,
                                         int32_t precision, uint32_t flags,
                                         uint32_t fill, uint32_t alignment,
                                         uint32_t sign, uint32_t zeroPad,
                                         uint64_t width, uint8_t *output,
                                         uint64_t capacity, uint64_t *size) {
  return formatValue(value, format, precision, flags, fill, alignment, sign,
                     zeroPad, width, output, capacity, size);
}
