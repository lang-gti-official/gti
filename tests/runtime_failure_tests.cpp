#include "gti/runtime_failure.h"

#include "failure_internal.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <type_traits>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct PartialWriter {
  std::uint8_t bytes[16]{};
  std::size_t size = 0;
  std::size_t calls = 0;
};

gti::runtime::detail::WriteAttempt
partialWrite(void *opaque, const std::uint8_t *data, std::size_t size) {
  auto &writer = *static_cast<PartialWriter *>(opaque);
  ++writer.calls;
  if (writer.calls == 1) {
    return {.count = -1, .error = EINTR};
  }
  const std::size_t count = size > 2 ? 2 : size;
  std::memcpy(writer.bytes + writer.size, data, count);
  writer.size += count;
  return {.count = static_cast<std::int64_t>(count), .error = 0};
}

gti::runtime::detail::WriteAttempt zeroWrite(void *, const std::uint8_t *,
                                             std::size_t) {
  return {.count = 0, .error = 0};
}

gti::runtime::detail::WriteAttempt failedWrite(void *, const std::uint8_t *,
                                               std::size_t) {
  return {.count = -1, .error = EIO};
}

void testWriteAll() {
  constexpr std::uint8_t input[] = {'a', 'b', 'c', 'd', 'e'};
  PartialWriter writer;
  expect(gti::runtime::detail::writeAll(partialWrite, &writer, input,
                                        sizeof(input)),
         "writeAll should retry EINTR and complete partial writes");
  expect(writer.calls == 4 && writer.size == sizeof(input) &&
             std::memcmp(writer.bytes, input, sizeof(input)) == 0,
         "writeAll should advance by each accepted byte count exactly once");
  expect(
      !gti::runtime::detail::writeAll(zeroWrite, nullptr, input, sizeof(input)),
      "writeAll should reject a zero-byte write before completion");
  expect(!gti::runtime::detail::writeAll(failedWrite, nullptr, input,
                                         sizeof(input)),
         "writeAll should stop on a non-EINTR write error");
  expect(gti::runtime::detail::writeAll(partialWrite, &writer, nullptr, 0),
         "writeAll should accept an empty write without touching the sink");
}

void testUnicodeTableBoundaries() {
  using gti::runtime::detail::unicode15_1ReportSafe;
  using gti::runtime::detail::unicode15_1ReportSafeInterval;
  using gti::runtime::detail::unicode15_1ReportSafeIntervalCount;

  const std::size_t count = unicode15_1ReportSafeIntervalCount();
  expect(count > 600, "Unicode 15.1 should retain the complete interval set");
  std::uint32_t previousLast = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const auto interval = unicode15_1ReportSafeInterval(index);
    expect(interval.first <= interval.last,
           "every generated Unicode interval should be nonempty");
    expect(index == 0 || interval.first > previousLast + 1,
           "generated Unicode intervals should be sorted and maximally merged");
    expect(unicode15_1ReportSafe(interval.first) &&
               unicode15_1ReportSafe(interval.last),
           "both inclusive interval boundaries should be accepted");
    if (interval.first != 0) {
      expect(!unicode15_1ReportSafe(interval.first - 1),
             "the scalar immediately before an interval should be rejected");
    }
    if (interval.last != 0x10FFFF) {
      expect(!unicode15_1ReportSafe(interval.last + 1),
             "the scalar immediately after an interval should be rejected");
    }
    previousLast = interval.last;
  }

  expect(
      unicode15_1ReportSafe(0x20) && unicode15_1ReportSafe(0x7E) &&
          unicode15_1ReportSafe(0x00A0) && unicode15_1ReportSafe(0x0301) &&
          unicode15_1ReportSafe(0x1F600),
      "ASCII space, graphic text, Zs, marks, and symbols should be accepted");
  expect(!unicode15_1ReportSafe(0x7F) && !unicode15_1ReportSafe(0x85) &&
             !unicode15_1ReportSafe(0x2028) && !unicode15_1ReportSafe(0x2029) &&
             !unicode15_1ReportSafe(0x202E) && !unicode15_1ReportSafe(0x110000),
         "controls, physical-line separators, bidi controls, and nonscalars "
         "should be rejected");
}

void testDescriptorOutcomeValidation() {
  constexpr char source[] = "unit.gti";
  constexpr std::uint8_t canonical[] = {1};
  gti_failure_outcome_descriptor_v1 outcomes[] = {{
      .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
      .detail = GTI_FAILURE_DETAIL_SUBTRACTION_V1,
      .reserved = 0,
  }};
  gti_failure_site_descriptor_v1 sites[] = {{
      .logical_source = {.data = source, .length = sizeof(source) - 1},
      .line = 1,
      .start = 0,
      .end = 1,
      .outcomes = outcomes,
      .outcome_count = 1,
      .reserved = 0,
  }};
  gti_failure_artifact_descriptor_v1 artifact{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .reserved = 0,
      .artifact_identity = {1},
      .sites = sites,
      .site_count = 1,
      .sites_reserved = 0,
      .canonical_descriptor = canonical,
      .canonical_descriptor_size = sizeof(canonical),
  };
  gti_failure_record_v1 record{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
      .detail = GTI_FAILURE_DETAIL_ADDITION_V1,
      .site_index = 1,
      .reserved = 0,
      .artifact_identity = {1},
  };

  expect(gti_rt_failure_write_report_v1(&record, &artifact) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "a site must reject a globally valid outcome it does not admit");
  artifact.artifact_identity[0] = 2;
  expect(gti_rt_failure_write_report_v1(&record, &artifact) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "a record and descriptor with unequal identities should be rejected");
  expect(gti_rt_failure_write_report_v1(&record, nullptr) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "a nonzero artifact record should require its descriptor");

  artifact.artifact_identity[0] = 1;
  outcomes[0].detail = GTI_FAILURE_DETAIL_ADDITION_V1;
  sites[0].logical_source.length = 0;
  expect(gti_rt_failure_write_report_v1(&record, &artifact) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "a non-runtime site should require a nonempty logical source name");

  gti_failure_record_v1 runtime = record;
  runtime.site_index = 0;
  std::memset(runtime.artifact_identity, 0, sizeof(runtime.artifact_identity));
  expect(gti_rt_failure_write_report_v1(&runtime, &artifact) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "the runtime sentinel should reject an attached descriptor");
  runtime.site_index = 1;
  expect(gti_rt_failure_write_report_v1(&runtime, nullptr) ==
             GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1,
         "a zero artifact identity should require site zero");
}

} // namespace

static_assert(sizeof(gti_failure_record_v1) == 48);
static_assert(alignof(gti_failure_record_v1) == 4);
static_assert(offsetof(gti_failure_record_v1, abi_version) == 0);
static_assert(offsetof(gti_failure_record_v1, code) == 4);
static_assert(offsetof(gti_failure_record_v1, detail) == 6);
static_assert(offsetof(gti_failure_record_v1, site_index) == 8);
static_assert(offsetof(gti_failure_record_v1, reserved) == 12);
static_assert(offsetof(gti_failure_record_v1, artifact_identity) == 16);
static_assert(sizeof(gti_failure_outcome_descriptor_v1) == 8);
static_assert(offsetof(gti_failure_outcome_descriptor_v1, reserved) == 4);
static_assert(sizeof(gti_failure_site_descriptor_v1) == 56);
static_assert(offsetof(gti_failure_site_descriptor_v1, line) == 16);
static_assert(offsetof(gti_failure_site_descriptor_v1, outcomes) == 40);
static_assert(offsetof(gti_failure_site_descriptor_v1, outcome_count) == 48);
static_assert(sizeof(gti_failure_artifact_descriptor_v1) == 72);
static_assert(offsetof(gti_failure_artifact_descriptor_v1, artifact_identity) ==
              8);
static_assert(offsetof(gti_failure_artifact_descriptor_v1, sites) == 40);
static_assert(offsetof(gti_failure_artifact_descriptor_v1, site_count) == 48);
static_assert(offsetof(gti_failure_artifact_descriptor_v1,
                       canonical_descriptor) == 56);
static_assert(sizeof(gti_failure_emergency_v1) == 104);
static_assert(offsetof(gti_failure_emergency_v1, primary) == 8);
static_assert(offsetof(gti_failure_emergency_v1, secondary) == 56);
static_assert(std::is_standard_layout_v<gti_failure_record_v1> &&
              std::is_trivially_copyable_v<gti_failure_record_v1>);
static_assert(std::is_standard_layout_v<gti_failure_artifact_descriptor_v1> &&
              std::is_trivially_copyable_v<gti_failure_artifact_descriptor_v1>);
static_assert(std::is_same_v<decltype(&gti_rt_failure_write_report_v1),
                             gti_failure_report_result_v1 (*)(
                                 const gti_failure_record_v1 *,
                                 const gti_failure_artifact_descriptor_v1 *)>);
static_assert(
    std::is_same_v<decltype(&gti_rt_failure_terminate_v1),
                   void (*)(const gti_failure_record_v1 *,
                            const gti_failure_artifact_descriptor_v1 *,
                            gti_failure_observer_v1, void *)>);

int main() {
  testWriteAll();
  testUnicodeTableBoundaries();
  testDescriptorOutcomeValidation();
  return failures == 0 ? 0 : 1;
}
