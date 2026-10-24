#include "gti/runtime_failure.h"
#include "gti/runtime_thread.hpp"

#include "failure_internal.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <type_traits>

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

// Reserved for the emergency report, never an ordinary record category.
constexpr gti_failure_code_v1 kCleanupFailureReportCode = 14;

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
    "", // 14 is not an ordinary failure record.
    "key_not_found",
    "invalid_container_argument",
    "panic",
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
    "mutex_lock",
    "key_lookup",
    "container_argument",
    "shared_owner",
    "explicit_panic",
    "thread_local_initialization_during_shutdown",
    "thread_local_outside_attached_lifetime",
};

static_assert(std::size(kFailureCategories) == 18);
static_assert(std::size(kFailureDetails) == 40);

struct ReportOutput {
  WriteFunction write = nullptr;
  void *context = nullptr;
  bool valid = true;
  bool color = false;
  bool errorStyleActive = false;

  ~ReportOutput() {
    if (errorStyleActive) {
      // A partial report must not leave the terminal styled. This final
      // best-effort write neither recurses nor changes the report result.
      constexpr char reset[] = "\033[0m";
      (void)gti::runtime::detail::writeAll(
          write, context, reinterpret_cast<const std::uint8_t *>(reset),
          sizeof(reset) - 1);
    }
  }

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

  bool beginErrorStyle() {
    if (!color) {
      return true;
    }
    errorStyleActive = true;
    return literal("\033[1;31m");
  }

  bool endErrorStyle() {
    if (!color) {
      return true;
    }
    if (!literal("\033[0m")) {
      return false;
    }
    errorStyleActive = false;
    return true;
  }
};

struct ResolvedFailure {
  gti_failure_code_v1 code = 0;
  gti_failure_detail_v1 detail = 0;
  const gti_failure_site_descriptor_v1 *site = nullptr;
  bool runtimeSite = false;
  gti_c_string_view message{};
  bool messageTruncated = false;
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
           detail == GTI_FAILURE_DETAIL_SHARED_OWNER_V1 ||
           detail == GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1 ||
           detail == GTI_FAILURE_DETAIL_ELEMENT_CONSTRUCTION_V1 ||
           detail == GTI_FAILURE_DETAIL_HOSTED_ARGUMENTS_V1;
  case GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1:
    return detail == GTI_FAILURE_DETAIL_STDOUT_WRITE_V1 ||
           detail == GTI_FAILURE_DETAIL_AUTOMATIC_JOIN_V1 ||
           detail == GTI_FAILURE_DETAIL_MUTEX_LOCK_V1;
  case GTI_FAILURE_CODE_HOSTED_RUNTIME_CONTRACT_FAILURE_V1:
    return detail == GTI_FAILURE_DETAIL_NEGATIVE_ARGUMENT_COUNT_V1 ||
           detail ==
               GTI_FAILURE_DETAIL_RECURSIVE_THREAD_LOCAL_INITIALIZATION_V1 ||
           detail ==
               GTI_FAILURE_DETAIL_THREAD_LOCAL_INITIALIZATION_DURING_SHUTDOWN_V2 ||
           detail ==
               GTI_FAILURE_DETAIL_THREAD_LOCAL_OUTSIDE_ATTACHED_LIFETIME_V2;
  case GTI_FAILURE_CODE_KEY_NOT_FOUND_V1:
    return detail == GTI_FAILURE_DETAIL_KEY_LOOKUP_V1;
  case GTI_FAILURE_CODE_INVALID_CONTAINER_ARGUMENT_V1:
    return detail == GTI_FAILURE_DETAIL_CONTAINER_ARGUMENT_V1;
  case GTI_FAILURE_CODE_PANIC_V2:
    return detail == GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2;
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
         record.reserved == 0 && record.code != GTI_FAILURE_CODE_PANIC_V2 &&
         globallyValidOutcome(record.code, record.detail);
}

[[nodiscard]] bool validRecordHeader(const gti_failure_record_v2 &record) {
  if (record.abi_version != GTI_FAILURE_ABI_VERSION_V2 ||
      record.reserved != 0 ||
      !globallyValidOutcome(record.code, record.detail) ||
      record.message_size > GTI_FAILURE_MESSAGE_CAPACITY_V2 ||
      (record.message_flags & ~GTI_FAILURE_MESSAGE_TRUNCATED_V2) != 0 ||
      (record.code != GTI_FAILURE_CODE_PANIC_V2 &&
       (record.message_size != 0 || record.message_flags != 0))) {
    return false;
  }
  // Canonical zero padding makes complete by-value copies independently
  // comparable and never exposes bytes outside the captured message.
  for (std::size_t index = record.message_size; index < sizeof(record.message);
       ++index) {
    if (record.message[index] != 0) {
      return false;
    }
  }
  return true;
}

template <typename Record>
[[nodiscard]] bool siteAdmits(const gti_failure_site_descriptor_v1 &site,
                              const Record &record) {
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

template <typename Record>
[[nodiscard]] bool
resolveFailure(const Record *record,
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
    resolved.runtimeSite = true;
    resolved.site = nullptr;
  } else {
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
    resolved.site = &site;
    resolved.runtimeSite = false;
  }
  resolved.code = record->code;
  resolved.detail = record->detail;
  if constexpr (std::is_same_v<Record, gti_failure_record_v2>) {
    resolved.message = {.data = reinterpret_cast<const char *>(record->message),
                        .length = record->message_size};
    resolved.messageTruncated =
        (record->message_flags & GTI_FAILURE_MESSAGE_TRUNCATED_V2) != 0;
  }
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

// `<source>:<line>`, matching the compiler diagnostic prefix. The runtime has
// no column: a site descriptor carries only a line and a byte range, and
// deriving a column would require reading the source, which the reporting path
// cannot do. It allocates nothing, opens nothing, and writes one line.
bool writeSourceLocation(ReportOutput &output, const ResolvedFailure &failure) {
  if (failure.runtimeSite) {
    return output.literal("<runtime>:0");
  }
  return writeEscapedSource(output, failure.site->logical_source) &&
         output.literal(":") && writeUnsigned(output, failure.site->line);
}

bool writeSourcePrefix(ReportOutput &output, const ResolvedFailure &failure) {
  return writeSourceLocation(output, failure) && output.literal(": ");
}

bool writeErrorHeading(ReportOutput &output, gti_failure_code_v1 code) {
  return output.beginErrorStyle() && output.literal("runtime error[") &&
         writeFailureCode(output, code) && output.literal("]") &&
         output.endErrorStyle() && output.literal(": ");
}

bool writeOrdinaryReport(ReportOutput &output, const ResolvedFailure &failure) {
  if (!(writeSourcePrefix(output, failure) &&
        writeErrorHeading(output, failure.code) &&
        output.text(kFailureCategories[failure.code]))) {
    return false;
  }
  if (failure.code == GTI_FAILURE_CODE_PANIC_V2) {
    if (!(output.literal(": \"") &&
          writeEscapedSource(output, failure.message) && output.literal("\"") &&
          (!failure.messageTruncated || output.literal(" [truncated]")))) {
      return false;
    }
  } else if (!(output.literal(" in ") &&
               output.text(kFailureDetails[failure.detail]))) {
    return false;
  }
  return output.literal("\n");
}

bool writeCleanupMessage(ReportOutput &output, const ResolvedFailure &failure) {
  return failure.code != GTI_FAILURE_CODE_PANIC_V2 ||
         (output.literal(" message=\"") &&
          writeEscapedSource(output, failure.message) && output.literal("\"") &&
          (!failure.messageTruncated || output.literal(" [truncated]")));
}

bool writeCleanupReport(ReportOutput &output, const ResolvedFailure &primary,
                        const ResolvedFailure &secondary) {
  return writeSourcePrefix(output, secondary) &&
         writeErrorHeading(output, kCleanupFailureReportCode) &&
         output.literal("failure_during_cleanup; primary [") &&
         writeFailureCode(output, primary.code) && output.literal("] at ") &&
         writeSourceLocation(output, primary) &&
         writeCleanupMessage(output, primary) &&
         output.literal("; secondary [") &&
         writeFailureCode(output, secondary.code) && output.literal("]") &&
         writeCleanupMessage(output, secondary) && output.literal("\n");
}

#if defined(_WIN32)
struct NativeWriteContext {
  HANDLE handle;
  DWORD originalMode = 0;
  bool restoreMode = false;

  ~NativeWriteContext() {
    if (restoreMode) {
      (void)SetConsoleMode(handle, originalMode);
    }
  }
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

bool nativeColorEnabled(NativeWriteContext &context) {
  const char *mode = std::getenv("GTI_COLOR");
  if (mode != nullptr && std::string_view(mode) == "never") {
    return false;
  }
  const bool always = mode != nullptr && std::string_view(mode) == "always";
  if (!always) {
    const char *noColor = std::getenv("NO_COLOR");
    const char *term = std::getenv("TERM");
    if ((noColor != nullptr && *noColor != '\0') ||
        (term != nullptr && std::string_view(term) == "dumb")) {
      return false;
    }
  }
#if defined(_WIN32)
  if (!GetConsoleMode(context.handle, &context.originalMode) ||
      !SetConsoleMode(context.handle, context.originalMode |
                                          ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
    return always;
  }
  context.restoreMode = true;
  return true;
#else
  return always || isatty(context.descriptor) != 0;
#endif
}

template <typename Record>
gti_failure_report_result_v1
writeOrdinaryNative(const Record *record,
                    const gti_failure_artifact_descriptor_v1 *artifact) {
  ResolvedFailure resolved;
  if (!resolveFailure(record, artifact, resolved)) {
    return GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
  }
  NativeWriteContext context = nativeStderrContext();
  ReportOutput output{.write = nativeWrite,
                      .context = &context,
                      .color = nativeColorEnabled(context)};
  return writeOrdinaryReport(output, resolved) ? GTI_FAILURE_REPORT_OK_V1
                                               : GTI_FAILURE_REPORT_IO_ERROR_V1;
}

template <typename Emergency>
gti_failure_report_result_v1 writeCleanupNative(
    const Emergency *failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) {
  constexpr auto version = std::is_same_v<Emergency, gti_failure_emergency_v1>
                               ? GTI_FAILURE_ABI_VERSION_V1
                               : GTI_FAILURE_ABI_VERSION_V2;
  if (failure == nullptr || failure->abi_version != version ||
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
  ReportOutput output{.write = nativeWrite,
                      .context = &context,
                      .color = nativeColorEnabled(context)};
  return writeCleanupReport(output, primary, secondary)
             ? GTI_FAILURE_REPORT_OK_V1
             : GTI_FAILURE_REPORT_IO_ERROR_V1;
}

std::atomic<std::uint32_t> gTerminalState{0};

struct ActiveTerminal {
  const gti_failure_record_v1 *originalV1 = nullptr;
  const gti_failure_record_v2 *originalV2 = nullptr;
  const gti_failure_artifact_descriptor_v1 *artifact = nullptr;
};

thread_local const ActiveTerminal *gActiveTerminal = nullptr;

[[noreturn]] void completeTerminal() {
  // Defined failure exits through _Exit so host atexit/destructor machinery
  // cannot run after GTI cleanup. Preserve stdout writes that completed before
  // the failure boundary, including worker-local destructor output handed back
  // through a managed join.
  (void)std::fflush(stdout);
  gTerminalState.store(2, std::memory_order_release);
  std::_Exit(GTI_FAILURE_EXIT_STATUS);
}

[[noreturn]] void terminateObserverViolation() {
  if (gActiveTerminal != nullptr) {
    if (gActiveTerminal->originalV2 != nullptr) {
      (void)writeOrdinaryNative(gActiveTerminal->originalV2,
                                gActiveTerminal->artifact);
    } else {
      (void)writeOrdinaryNative(gActiveTerminal->originalV1,
                                gActiveTerminal->artifact);
    }
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

namespace gti::runtime::detail {

[[noreturn]] void
terminateLegacyFailure(const gti_failure_record_v2 &record,
                       const gti_failure_artifact_descriptor_v1 *artifact) {
  claimTerminalWinner();
  (void)writeOrdinaryNative(&record, artifact);
  completeTerminal();
}

[[noreturn]] void terminateLegacyFailure(
    const gti_failure_emergency_v2 &failure,
    const gti_failure_artifact_descriptor_v1 *primaryArtifact,
    const gti_failure_artifact_descriptor_v1 *secondaryArtifact) {
  claimTerminalWinner();
  (void)writeCleanupNative(&failure, primaryArtifact, secondaryArtifact);
  completeTerminal();
}

} // namespace gti::runtime::detail

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
  if (record != nullptr && gti_runtime_detail::managed_worker_active()) {
    // Worker failures are observed only by join. The generated MIR path has
    // already completed task cleanup before reaching this containment call.
    gti_runtime_detail::handoff_managed_worker_failure(*record, artifact);
  }
  gti_runtime_detail::drain_thread_local_cleanup_legacy(record, artifact);
  claimTerminalWinner();

  const gti_failure_record_v1 invalidRecord{};
  const gti_failure_record_v1 original =
      record == nullptr ? invalidRecord : *record;
  const ActiveTerminal active{
      .originalV1 = &original,
      .artifact = artifact,
  };
  gActiveTerminal = &active;

  ResolvedFailure resolved;
  const bool valid = resolveFailure(&original, artifact, resolved);
  if (valid && observer != nullptr) {
    gti_failure_record_v1 observed = original;
    try {
      observer(&observed, observer_context);
    } catch (...) {
      terminateObserverViolation();
    }
    if (std::memcmp(&observed, &original, sizeof(observed)) != 0) {
      terminateObserverViolation();
    }
  }

  if (valid) {
    (void)writeOrdinaryNative(&original, artifact);
  }
  completeTerminal();
}

extern "C" [[noreturn]] void gti_rt_failure_terminate_cleanup_v1(
    const gti_failure_emergency_v1 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  if (failure != nullptr && gti_runtime_detail::managed_worker_active()) {
    gti_runtime_detail::handoff_managed_worker_cleanup_failure(
        *failure, primary_artifact, secondary_artifact);
  }
  claimTerminalWinner();
  (void)writeCleanupNative(failure, primary_artifact, secondary_artifact);
  completeTerminal();
}

extern "C" gti_failure_report_result_v1
gti_rt_failure_capture_panic_v2(gti_failure_record_v2 *record,
                                gti_c_string_view message) {
  if (record == nullptr || record->abi_version != GTI_FAILURE_ABI_VERSION_V2 ||
      record->reserved != 0 || record->code != GTI_FAILURE_CODE_PANIC_V2 ||
      record->detail != GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2 ||
      (message.data == nullptr && message.length != 0)) {
    return GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
  }
  // A local bounded snapshot also makes an overlapping source view safe. Read
  // at most the capacity plus three lookahead bytes to preserve valid UTF-8 at
  // the boundary; malformed input is retained as bytes, not reinterpreted.
  std::uint8_t snapshot[GTI_FAILURE_MESSAGE_CAPACITY_V2]{};
  const auto *data = reinterpret_cast<const std::uint8_t *>(message.data);
  std::size_t size = 0;
  while (size < sizeof(snapshot) && size < message.length) {
    const std::uint64_t remaining = message.length - size;
    const std::size_t available =
        remaining < 4 ? static_cast<std::size_t>(remaining) : 4;
    std::uint32_t scalar = 0;
    std::size_t count = decodeUtf8(data + size, available, scalar);
    if (count == 0) {
      count = 1;
    }
    if (count > sizeof(snapshot) - size) {
      break;
    }
    std::memcpy(snapshot + size, data + size, count);
    size += count;
  }
  std::memcpy(record->message, snapshot, sizeof(snapshot));
  record->message_size = static_cast<std::uint32_t>(size);
  record->message_flags =
      size < message.length ? GTI_FAILURE_MESSAGE_TRUNCATED_V2 : 0;
  return GTI_FAILURE_REPORT_OK_V1;
}

extern "C" gti_failure_report_result_v1 gti_rt_failure_write_report_v2(
    const gti_failure_record_v2 *record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  return writeOrdinaryNative(record, artifact);
}

extern "C" gti_failure_report_result_v1
gti_rt_failure_validate_v2(const gti_failure_record_v2 *record,
                           const gti_failure_artifact_descriptor_v1 *artifact) {
  ResolvedFailure resolved;
  return resolveFailure(record, artifact, resolved)
             ? GTI_FAILURE_REPORT_OK_V1
             : GTI_FAILURE_REPORT_INVALID_ARGUMENT_V1;
}

extern "C" gti_failure_report_result_v1 gti_rt_failure_write_cleanup_report_v2(
    const gti_failure_emergency_v2 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  return writeCleanupNative(failure, primary_artifact, secondary_artifact);
}

extern "C" [[noreturn]] void
gti_rt_failure_terminate_v2(const gti_failure_record_v2 *record,
                            const gti_failure_artifact_descriptor_v1 *artifact,
                            gti_failure_observer_v2 observer,
                            void *observer_context) {
  if (record != nullptr && gti_runtime_detail::managed_worker_active()) {
    gti_runtime_detail::handoff_managed_worker_failure(*record, artifact);
  }
  // The verified caller has retired its local frame. Thread-owned values
  // remain live until this attached thread's terminal boundary.
  gti_runtime_detail::drain_thread_local_cleanup(record, artifact);
  claimTerminalWinner();
  const gti_failure_record_v2 original =
      record == nullptr ? gti_failure_record_v2{} : *record;
  const ActiveTerminal active{.originalV2 = &original, .artifact = artifact};
  gActiveTerminal = &active;
  ResolvedFailure resolved;
  const bool valid = resolveFailure(&original, artifact, resolved);
  if (valid && observer != nullptr) {
    gti_failure_record_v2 observed = original;
    try {
      observer(&observed, observer_context);
    } catch (...) {
      terminateObserverViolation();
    }
    if (std::memcmp(&observed, &original, sizeof(observed)) != 0) {
      terminateObserverViolation();
    }
  }
  if (valid) {
    (void)writeOrdinaryNative(&original, artifact);
  }
  completeTerminal();
}

extern "C" [[noreturn]] void gti_rt_failure_terminate_cleanup_v2(
    const gti_failure_emergency_v2 *failure,
    const gti_failure_artifact_descriptor_v1 *primary_artifact,
    const gti_failure_artifact_descriptor_v1 *secondary_artifact) {
  if (failure != nullptr && gti_runtime_detail::managed_worker_active()) {
    gti_runtime_detail::handoff_managed_worker_cleanup_failure(
        *failure, primary_artifact, secondary_artifact);
  }
  claimTerminalWinner();
  (void)writeCleanupNative(failure, primary_artifact, secondary_artifact);
  completeTerminal();
}

extern "C" [[noreturn]] void gti_rt_failure_abort_allocation_v2(
    const gti_failure_record_v2 *record,
    const gti_failure_artifact_descriptor_v1 *artifact) {
  // Do not enter terminal arbitration or managed-task containment. Exhaustion
  // may occur inside an observer or during cleanup; waiting for that same
  // thread to finish reporting would deadlock, and handing off would resume a
  // caller despite the process-terminal allocation contract.
  ResolvedFailure resolved;
  if (record != nullptr &&
      record->code == GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1 &&
      resolveFailure(record, artifact, resolved)) {
    (void)writeOrdinaryNative(record, artifact);
  } else {
    const gti_failure_record_v2 fallback{
        .abi_version = GTI_FAILURE_ABI_VERSION_V2,
        .code = GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
        .detail = GTI_FAILURE_DETAIL_PRIVATE_STORAGE_V1,
        .site_index = 0,
        .reserved = 0,
        .artifact_identity = {},
        .message_size = 0,
        .message_flags = 0,
        .message = {}};
    (void)writeOrdinaryNative(&fallback, nullptr);
  }
  std::abort();
}
