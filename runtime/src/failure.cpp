#include "gti/runtime_failure.h"

#include "failure_internal.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace gti::runtime::detail {

#include "failure_unicode_15_1.inc"

bool writeAll(WriteFunction write, void *context, const std::uint8_t *data,
              std::size_t size) {
  if (write == nullptr || (data == nullptr && size != 0)) {
    return false;
  }

  std::size_t offset = 0;
  while (offset < size) {
    const WriteAttempt attempt = write(context, data + offset, size - offset);
    if (attempt.count < 0) {
      if (attempt.error == EINTR) {
        continue;
      }
      return false;
    }
    if (attempt.count == 0 ||
        static_cast<std::uint64_t>(attempt.count) > size - offset) {
      return false;
    }
    offset += static_cast<std::size_t>(attempt.count);
  }
  return true;
}

bool unicode15_1ReportSafe(std::uint32_t scalar) {
  std::size_t first = 0;
  std::size_t count = std::size(kFailureReportSafeUnicode15_1);
  while (count != 0) {
    const std::size_t step = count / 2;
    const std::size_t index = first + step;
    const UnicodeInterval interval = kFailureReportSafeUnicode15_1[index];
    if (scalar < interval.first) {
      count = step;
    } else if (scalar > interval.last) {
      first = index + 1;
      count -= step + 1;
    } else {
      return true;
    }
  }
  return false;
}

std::size_t unicode15_1ReportSafeIntervalCount() {
  return std::size(kFailureReportSafeUnicode15_1);
}

UnicodeInterval unicode15_1ReportSafeInterval(std::size_t index) {
  if (index >= std::size(kFailureReportSafeUnicode15_1)) {
    return {};
  }
  return kFailureReportSafeUnicode15_1[index];
}

} // namespace gti::runtime::detail

namespace {

using gti::runtime::detail::WriteAttempt;
using gti::runtime::detail::WriteFunction;

constexpr std::string_view kFailureCategories[] = {
    "",
    "integer_overflow",
    "division_by_zero",
    "modulo_by_zero",
    "negative_shift_count",
    "shift_count_out_of_range",
    "numeric_conversion_out_of_range",
    "index_out_of_bounds",
    "empty_owner_access",
    "invalid_expected_access",
    "invalid_storage_state",
    "allocation_failure",
    "infallible_host_operation_failed",
    "hosted_runtime_contract_failure",
};

constexpr std::string_view kFailureDetails[] = {
    "",
    "addition",
    "subtraction",
    "multiplication",
    "division",
    "negation",
    "integer_division",
    "integer_modulo",
    "left_shift",
    "right_shift",
    "numeric_cast",
    "hosted_argument_count",
    "fixed_array",
    "string_view",
    "vector",
    "string",
    "private_storage",
    "dereference",
    "member_access",
    "value_on_error",
    "error_on_value",
    "duplicate_construction",
    "uninitialized_access",
    "relocation_capacity",
    "invalid_relocation_source",
    "occupied_relocation_destination",
    "unique_owner",
    "element_construction",
    "hosted_arguments",
    "stdout_write",
    "automatic_join",
    "negative_argument_count",
    "recursive_thread_local_initialization",
};

static_assert(std::size(kFailureCategories) == 14);
static_assert(std::size(kFailureDetails) == 33);

struct ReportOutput {
  WriteFunction write = nullptr;
  void *context = nullptr;
  bool valid = true;

  bool bytes(const void *data, std::size_t size) {
    if (!valid) {
      return false;
    }
    valid = gti::runtime::detail::writeAll(
        write, context, static_cast<const std::uint8_t *>(data), size);
    return valid;
  }

  bool text(std::string_view value) {
    return bytes(value.data(), value.size());
  }

  template <std::size_t Size> bool literal(const char (&value)[Size]) {
    return bytes(value, Size - 1);
  }
};

struct ResolvedFailure {
  const gti_failure_record_v1 *record = nullptr;
  const gti_failure_site_descriptor_v1 *site = nullptr;
  bool runtimeSite = false;
};

[[nodiscard]] bool identityIsZero(const std::uint8_t identity[32]) {
  std::uint8_t combined = 0;
  for (std::size_t index = 0; index < 32; ++index) {
    combined = static_cast<std::uint8_t>(combined | identity[index]);
  }
  return combined == 0;
}

[[nodiscard]] bool sameIdentity(const std::uint8_t left[32],
                                const std::uint8_t right[32]) {
  return std::memcmp(left, right, 32) == 0;
}

[[nodiscard]] bool globallyValidOutcome(gti_failure_code_v1 code,
                                        gti_failure_detail_v1 detail) {
  switch (code) {
  case GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1:
    return detail >= GTI_FAILURE_DETAIL_ADDITION_V1 &&
           detail <= GTI_FAILURE_DETAIL_NEGATION_V1;
  case GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1:
    return detail == GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1;
  case GTI_FAILURE_CODE_MODULO_BY_ZERO_V1:
    return detail == GTI_FAILURE_DETAIL_INTEGER_MODULO_V1;
  case GTI_FAILURE_CODE_NEGATIVE_SHIFT_COUNT_V1:
  case GTI_FAILURE_CODE_SHIFT_COUNT_OUT_OF_RANGE_V1:
    return detail == GTI_FAILURE_DETAIL_LEFT_SHIFT_V1 ||
           detail == GTI_FAILURE_DETAIL_RIGHT_SHIFT_V1;
  case GTI_FAILURE_CODE_NUMERIC_CONVERSION_OUT_OF_RANGE_V1:
    return detail == GTI_FAILURE_DETAIL_NUMERIC_CAST_V1 ||
           detail == GTI_FAILURE_DETAIL_HOSTED_ARGUMENT_COUNT_V1;
  case GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1:
    return detail >= GTI_FAILURE_DETAIL_FIXED_ARRAY_V1 &&
           detail <= GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1;
  case GTI_FAILURE_CODE_EMPTY_OWNER_ACCESS_V1:
    return detail == GTI_FAILURE_DETAIL_DEREFERENCE_V1 ||
           detail == GTI_FAILURE_DETAIL_MEMBER_ACCESS_V1;
  case GTI_FAILURE_CODE_INVALID_EXPECTED_ACCESS_V1:
    return detail == GTI_FAILURE_DETAIL_VALUE_ON_ERROR_V1 ||
           detail == GTI_FAILURE_DETAIL_ERROR_ON_VALUE_V1;
  case GTI_FAILURE_CODE_INVALID_STORAGE_STATE_V1:
    return detail >= GTI_FAILURE_DETAIL_DUPLICATE_CONSTRUCTION_V1 &&
           detail <= GTI_FAILURE_DETAIL_OCCUPIED_RELOCATION_DESTINATION_V1;
  case GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1:
    return detail == GTI_FAILURE_DETAIL_UNIQUE_OWNER_V1 ||
           detail == GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1 ||
           detail == GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1 ||
           detail == GTI_FAILURE_DETAIL_HOSTED_ARGUMENTS_V1;
  case GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1:
    return detail == GTI_FAILURE_DETAIL_STDOUT_WRITE_V1 ||
           detail == GTI_FAILURE_DETAIL_AUTOMATIC_JOIN_V1;
  case GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1:
    return detail == GTI_FAILURE_DETAIL_NEGATIVE_ARGUMENT_COUNT_V1 ||
           detail ==
               GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1;
  default:
    return false;
  }
}

[[nodiscard]] bool canonicalOutcomeLess(gti_failure_code_v1 leftCode,
                                        gti_failure_detail_v1 leftDetail,
                                        gti_failure_code_v1 rightCode,
                                        gti_failure_detail_v1 rightDetail) {
  if (leftCode != rightCode) {
    return leftCode < rightCode;
  }
  return kFailureDetails[leftDetail] < kFailureDetails[rightDetail];
}

[[nodiscard]] bool validRecordHeader(const gti_failure_record_v1 &record) {
  return record.abi_version == GTI_FAILURE_ABI_VERSION_V1 &&
         record.reserved == 0 &&
         globallyValidOutcome(record.code, record.detail);
}

[[nodiscard]] bool siteAdmits(const gti_failure_site_descriptor_v1 &site,
                              const gti_failure_record_v1 &record) {
  if (site.reserved != 0 || site.line == 0 || site.end < site.start ||
      site.logical_source.data == nullptr || site.logical_source.length == 0 ||
      site.outcomes == nullptr || site.outcome_count == 0) {
    return false;
  }

  bool admitted = false;
  gti_failure_code_v1 previousCode = 0;
  gti_failure_detail_v1 previousDetail = 0;
  for (std::uint32_t index = 0; index < site.outcome_count; ++index) {
    const gti_failure_outcome_descriptor_v1 outcome = site.outcomes[index];
    if (outcome.reserved != 0 ||
        !globallyValidOutcome(outcome.code, outcome.detail) ||
        (index != 0 && !canonicalOutcomeLess(previousCode, previousDetail,
                                             outcome.code, outcome.detail))) {
      return false;
    }
    previousCode = outcome.code;
    previousDetail = outcome.detail;
    admitted = admitted ||
               (outcome.code == record.code && outcome.detail == record.detail);
  }
  return admitted;
}

[[nodiscard]] bool
resolveFailure(const gti_failure_record_v1 *record,
               const gti_failure_artifact_descriptor_v1 *artifact,
               ResolvedFailure &resolved) {
  if (record == nullptr || !validRecordHeader(*record)) {
    return false;
  }

  const bool zeroIdentity = identityIsZero(record->artifact_identity);
  if (zeroIdentity || record->site_index == 0) {
    if (!zeroIdentity || record->site_index != 0 || artifact != nullptr) {
      return false;
    }
    resolved = {.record = record, .site = nullptr, .runtimeSite = true};
    return true;
  }

  if (artifact == nullptr ||
      artifact->abi_version != GTI_FAILURE_ABI_VERSION_V1 ||
      artifact->reserved != 0 || artifact->sites_reserved != 0 ||
      identityIsZero(artifact->artifact_identity) ||
      !sameIdentity(record->artifact_identity, artifact->artifact_identity) ||
      artifact->sites == nullptr || artifact->site_count == 0 ||
      record->site_index > artifact->site_count ||
      artifact->canonical_descriptor == nullptr ||
      artifact->canonical_descriptor_size == 0 ||
      artifact->canonical_descriptor_size > SIZE_MAX) {
    return false;
  }

  const gti_failure_site_descriptor_v1 &site =
      artifact->sites[record->site_index - 1];
  if (!siteAdmits(site, *record) || site.logical_source.length > SIZE_MAX) {
    return false;
  }

  resolved = {.record = record, .site = &site, .runtimeSite = false};
  return true;
}

bool writeUnsigned(ReportOutput &output, std::uint64_t value) {
  char digits[20];
  std::size_t count = 0;
  do {
    digits[count++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  for (std::size_t first = 0, last = count - 1; first < last; ++first, --last) {
    const char temporary = digits[first];
    digits[first] = digits[last];
    digits[last] = temporary;
  }
  return output.bytes(digits, count);
}

bool writeFailureCode(ReportOutput &output, gti_failure_code_v1 code) {
  char value[] = "GTI-R0000";
  value[8] = static_cast<char>('0' + code % 10);
  code = static_cast<gti_failure_code_v1>(code / 10);
  value[7] = static_cast<char>('0' + code % 10);
  code = static_cast<gti_failure_code_v1>(code / 10);
  value[6] = static_cast<char>('0' + code % 10);
  code = static_cast<gti_failure_code_v1>(code / 10);
  value[5] = static_cast<char>('0' + code % 10);
  return output.bytes(value, sizeof(value) - 1);
}

bool writeIdentity(ReportOutput &output, const std::uint8_t identity[32]) {
  constexpr char hex[] = "0123456789abcdef";
  char encoded[64];
  for (std::size_t index = 0; index < 32; ++index) {
    encoded[index * 2] = hex[identity[index] >> 4];
    encoded[index * 2 + 1] = hex[identity[index] & 0x0F];
  }
  return output.bytes(encoded, sizeof(encoded));
}

void writeEscapedByte(ReportOutput &output, std::uint8_t byte) {
  constexpr char hex[] = "0123456789ABCDEF";
  const char escaped[] = {'\\', 'x', hex[byte >> 4], hex[byte & 0x0F]};
  (void)output.bytes(escaped, sizeof(escaped));
}

[[nodiscard]] std::size_t decodeUtf8(const std::uint8_t *data, std::size_t size,
                                     std::uint32_t &scalar) {
  if (size == 0) {
    return 0;
  }
  const std::uint8_t first = data[0];
  if (first <= 0x7F) {
    scalar = first;
    return 1;
  }

  std::size_t length = 0;
  std::uint32_t value = 0;
  if (first >= 0xC2 && first <= 0xDF) {
    length = 2;
    value = first & 0x1F;
  } else if (first >= 0xE0 && first <= 0xEF) {
    length = 3;
    value = first & 0x0F;
  } else if (first >= 0xF0 && first <= 0xF4) {
    length = 4;
    value = first & 0x07;
  } else {
    return 0;
  }
  if (size < length) {
    return 0;
  }
  for (std::size_t index = 1; index < length; ++index) {
    if ((data[index] & 0xC0) != 0x80) {
      return 0;
    }
    value = (value << 6) | (data[index] & 0x3F);
  }
  if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) ||
      value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
    return 0;
  }
  scalar = value;
  return length;
}

bool writeEscapedSource(ReportOutput &output, gti_c_string_view source) {
  const auto *data = reinterpret_cast<const std::uint8_t *>(source.data);
  const std::size_t size = static_cast<std::size_t>(source.length);
  std::size_t offset = 0;
  while (offset < size && output.valid) {
    const std::uint8_t byte = data[offset];
    switch (byte) {
    case '\\':
      (void)output.literal("\\\\");
      ++offset;
      continue;
    case '"':
      (void)output.literal("\\\"");
      ++offset;
      continue;
    case '\r':
      (void)output.literal("\\r");
      ++offset;
      continue;
    case '\n':
      (void)output.literal("\\n");
      ++offset;
      continue;
    case '\t':
      (void)output.literal("\\t");
      ++offset;
      continue;
    default:
      break;
    }

    std::uint32_t scalar = 0;
    const std::size_t length = decodeUtf8(data + offset, size - offset, scalar);
    if (length == 0) {
      writeEscapedByte(output, byte);
      ++offset;
      continue;
    }
    if (gti::runtime::detail::unicode15_1ReportSafe(scalar)) {
      (void)output.bytes(data + offset, length);
    } else {
      for (std::size_t index = 0; index < length && output.valid; ++index) {
        writeEscapedByte(output, data[offset + index]);
      }
    }
    offset += length;
  }
  return output.valid;
}

bool writeLocation(ReportOutput &output, const ResolvedFailure &failure) {
  if (!writeIdentity(output, failure.record->artifact_identity) ||
      !output.literal(" at \"")) {
    return false;
  }
  if (failure.runtimeSite) {
    return output.literal("<runtime>\":0@0..0");
  }
  return writeEscapedSource(output, failure.site->logical_source) &&
         output.literal("\":") && writeUnsigned(output, failure.site->line) &&
         output.literal("@") && writeUnsigned(output, failure.site->start) &&
         output.literal("..") && writeUnsigned(output, failure.site->end);
}

bool writeOrdinaryReport(ReportOutput &output, const ResolvedFailure &failure) {
  return output.literal("GTI runtime failure [") &&
         writeFailureCode(output, failure.record->code) &&
         output.literal("] ") &&
         output.text(kFailureCategories[failure.record->code]) &&
         output.literal(" in ") && writeLocation(output, failure) &&
         output.literal(": ") &&
         output.text(kFailureDetails[failure.record->detail]) &&
         output.literal("\n");
}

bool writeCleanupReport(ReportOutput &output, const ResolvedFailure &primary,
                        const ResolvedFailure &secondary) {
  return output.literal("GTI runtime failure [GTI-R0014] ") &&
         output.literal("failure_during_cleanup in ") &&
         writeLocation(output, secondary) &&
         output.literal(": failure during cleanup; primary [") &&
         writeFailureCode(output, primary.record->code) &&
         output.literal("] in ") && writeLocation(output, primary) &&
         output.literal("; secondary [") &&
         writeFailureCode(output, secondary.record->code) &&
         output.literal("]\n");
}

#if defined(_WIN32)
struct NativeWriteContext {
  HANDLE handle;
};

WriteAttempt nativeWrite(void *opaque, const std::uint8_t *data,
                         std::size_t size) {
  auto &context = *static_cast<NativeWriteContext *>(opaque);
  const DWORD chunk =
      static_cast<DWORD>(size > std::numeric_limits<DWORD>::max()
                             ? std::numeric_limits<DWORD>::max()
                             : size);
  DWORD written = 0;
  if (context.handle == nullptr || context.handle == INVALID_HANDLE_VALUE ||
      !WriteFile(context.handle, data, chunk, &written, nullptr)) {
    return {.count = -1, .error = EIO};
  }
  return {.count = written, .error = 0};
}

NativeWriteContext nativeStderrContext() {
  return {.handle = GetStdHandle(STD_ERROR_HANDLE)};
}
#else
struct ScopedSigpipeBlock {
  sigset_t mask{};
  sigset_t previousMask{};
  bool active = false;
  bool previouslyPending = true;
  bool brokenPipe = false;

  ScopedSigpipeBlock() {
    if (sigemptyset(&mask) != 0 || sigaddset(&mask, SIGPIPE) != 0 ||
        pthread_sigmask(SIG_BLOCK, &mask, &previousMask) != 0) {
      return;
    }

    sigset_t pending{};
    if (sigpending(&pending) != 0) {
      (void)pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
      return;
    }
    previouslyPending = sigismember(&pending, SIGPIPE) != 0;
    active = true;
  }

  ScopedSigpipeBlock(const ScopedSigpipeBlock &) = delete;
  ScopedSigpipeBlock &operator=(const ScopedSigpipeBlock &) = delete;

  ~ScopedSigpipeBlock() {
    if (!active) {
      return;
    }

    if (brokenPipe && !previouslyPending) {
      sigset_t pending{};
      if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        int captured = 0;
        int waitError = 0;
        do {
          waitError = sigwait(&mask, &captured);
        } while (waitError == EINTR);
      }
    }
    (void)pthread_sigmask(SIG_SETMASK, &previousMask, nullptr);
  }

  void noteError(int error) { brokenPipe = brokenPipe || error == EPIPE; }
};

struct NativeWriteContext {
  ScopedSigpipeBlock sigpipe;
  int descriptor;

  explicit NativeWriteContext(int descriptorValue)
      : descriptor(descriptorValue) {}
};

WriteAttempt nativeWrite(void *opaque, const std::uint8_t *data,
                         std::size_t size) {
  auto &context = *static_cast<NativeWriteContext *>(opaque);
  const std::size_t chunk =
      size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())
          ? static_cast<std::size_t>(std::numeric_limits<ssize_t>::max())
          : size;
  const ssize_t written = ::write(context.descriptor, data, chunk);
  const int error = written < 0 ? errno : 0;
  context.sigpipe.noteError(error);
  return {.count = static_cast<std::int64_t>(written), .error = error};
}

NativeWriteContext nativeStderrContext() {
  return NativeWriteContext{STDERR_FILENO};
}
#endif

gti_failure_report_result_v1
writeOrdinaryNative(const gti_failure_record_v1 *record,
                    const gti_failure_artifact_descriptor_v1 *artifact) {
  ResolvedFailure resolved;
  if (!resolveFailure(record, artifact, resolved)) {
    return GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
  }
  NativeWriteContext context = nativeStderrContext();
  ReportOutput output{.write = nativeWrite, .context = &context};
  return writeOrdinaryReport(output, resolved) ? GTI_FAILURE_REPORT_OK_V1
                                               : GTI_FAILURE_REPORT_IO_ERROR_V1;
}

gti_failure_report_result_v1 writeCleanupNative(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) {
  if (failure == nullptr ||
      failure->abi_version != GTI_FAILURE_ABI_VERSION_V1 ||
      failure->reserved != 0) {
    return GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
  }
  ResolvedFailure primary;
  ResolvedFailure secondary;
  if (!resolveFailure(&failure->primary, primaryArtifact, primary) ||
      !resolveFailure(&failure->secondary, secondaryArtifact, secondary)) {
    return GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
  }
  NativeWriteContext context = nativeStderrContext();
  ReportOutput output{.write = nativeWrite, .context = &context};
  return writeCleanupReport(output, primary, secondary)
             ? GTI_FAILURE_REPORT_OK_V1
             : GTI_FAILURE_REPORT_IO_ERROR_V1;
}

std::atomic<std::uint32_t> gTerminalState{0};

struct ActiveTerminal {
  gti_failure_record_v1 original;
  const gti_failure_artifact_descriptor_v1 *artifact;
};

thread_local const ActiveTerminal *gActiveTerminal = nullptr;

[[noreturn]] void completeTerminal() {
  gTerminalState.store(2, std::memory_order_release);
  std::_Exit(GTI_FAILURE_EXIT_STATUS);
}

[[noreturn]] void terminateObserverViolation() {
  if (gActiveTerminal != nullptr) {
    (void)writeOrdinaryNative(&gActiveTerminal->original,
                              gActiveTerminal->artifact);
  }
  completeTerminal();
}

void claimTerminalWinner() {
  if (gActiveTerminal != nullptr) {
    terminateObserverViolation();
  }

  std::uint32_t expected = 0;
  if (gTerminalState.compare_exchange_strong(
          expected, 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
    return;
  }
  while (gTerminalState.load(std::memory_order_acquire) != 2) {
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }
  std::_Exit(GTI_FAILURE_EXIT_STATUS);
}

} // namespace

extern "C" gti_failure_report_result_v1 gti_rt_failure_write_report_v1(
    const gti_failure_record_v1 *record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  return writeOrdinaryNative(record, artifact);
}

extern "C" gti_failure_report_result_v1 gti_rt_failure_write_cleanup_report_v1(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  return writeCleanupNative(failure, primary_artifact, secondary_artifact);
}

extern "C" [[noreturn]] void
gti_rt_failure_terminate_v1(const gti_failure_record_v1 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v1 observer,
                            void *observer_context) {
  claimTerminalWinner();

  const gti_failure_record_v1 invalidRecord{};
  const ActiveTerminal active{
      .original = record == nullptr ? invalidRecord : *record,
      .artifact = artifact,
  };
  gActiveTerminal = &active;

  ResolvedFailure resolved;
  const bool valid = resolveFailure(&active.original, artifact, resolved);
  if (valid && observer != nullptr) {
    gti_failure_record_v1 observed = active.original;
    try {
      observer(&observed, observer_context);
    } catch (...) {
      terminateObserverViolation();
    }
    if (std::memcmp(&observed, &active.original, sizeof(observed)) != 0) {
      terminateObserverViolation();
    }
  }

  if (valid) {
    (void)writeOrdinaryNative(&active.original, artifact);
  }
  completeTerminal();
}

extern "C" [[noreturn]] void gti_rt_failure_terminate_cleanup_v1(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  claimTerminalWinner();
  (void)writeCleanupNative(failure, primary_artifact, secondary_artifact);
  completeTerminal();
}
