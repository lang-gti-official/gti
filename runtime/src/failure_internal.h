#pragma once

#include <cstddef>
#include <cstdint>

namespace gti::runtime::detail {

struct UnicodeInterval {
  std::uint32_t first;
  std::uint32_t last;
};

struct WriteAttempt {
  std::int64_t count;
  int error;
};

using WriteFunction = WriteAttempt (*)(void *context, const std::uint8_t *data,
                                       std::size_t size);

[[nodiscard]] bool writeAll(WriteFunction write, void *context,
                            const std::uint8_t *data, std::size_t size);

[[nodiscard]] bool unicode15_1ReportSafe(std::uint32_t scalar);
[[nodiscard]] std::size_t unicode15_1ReportSafeIntervalCount();
[[nodiscard]] UnicodeInterval unicode15_1ReportSafeInterval(std::size_t index);

} // namespace gti::runtime::detail
