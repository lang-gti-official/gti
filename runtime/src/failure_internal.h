#pragma once

#include "gti/runtime_failure.h"

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

// A legacy native join result cannot represent a v2 panic. This is an explicit
// terminal compatibility boundary, not a source failure edge: never hand off
// from it to another managed worker and bypass that caller's cleanup protocol.
[[noreturn]] void
terminateLegacyFailure(const gti_failure_record_v2 &record,
                       const gti_failure_artifact_descriptor_v1 *artifact);
[[noreturn]] void terminateLegacyFailure(
    const gti_failure_emergency_v2 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact);

} // namespace gti::runtime::detail
