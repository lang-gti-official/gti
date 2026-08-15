#include "gti/runtime_failure.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr std::uint8_t kCanonicalDescriptor[] = {1};

struct Fixture {
  gti_failure_outcome_descriptor_v1 outcome{};
  gti_failure_site_descriptor_v1 site{};
  gti_failure_artifact_descriptor_v1 artifact{};
  gti_failure_record_v1 record{};

  Fixture(gti_failure_code_v1 code, gti_failure_detail_v1 detail,
          std::uint8_t identitySeed, const char *source, std::size_t sourceSize,
          std::uint64_t line = 7, std::uint64_t start = 12,
          std::uint64_t end = 19) {
    outcome = {.code = code, .detail = detail, .reserved = 0};
    site = {
        .logical_source = {.data = source,
                           .length = static_cast<std::uint64_t>(sourceSize)},
        .line = line,
        .start = start,
        .end = end,
        .outcomes = &outcome,
        .outcome_count = 1,
        .reserved = 0,
    };
    artifact = {
        .abi_version = GTI_FAILURE_ABI_VERSION_V1,
        .reserved = 0,
        .artifact_identity = {},
        .sites = &site,
        .site_count = 1,
        .sites_reserved = 0,
        .canonical_descriptor = kCanonicalDescriptor,
        .canonical_descriptor_size = sizeof(kCanonicalDescriptor),
    };
    record = {
        .abi_version = GTI_FAILURE_ABI_VERSION_V1,
        .code = code,
        .detail = detail,
        .site_index = 1,
        .reserved = 0,
        .artifact_identity = {},
    };
    for (std::size_t index = 0; index < 32; ++index) {
      const auto byte = static_cast<std::uint8_t>(identitySeed + index);
      artifact.artifact_identity[index] = byte;
      record.artifact_identity[index] = byte;
    }
  }
};

void writeStdout(const char *text, std::size_t size) {
#if defined(_WIN32)
  DWORD written = 0;
  (void)WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text,
                  static_cast<DWORD>(size), &written, nullptr);
#else
  while (size != 0) {
    const ssize_t count = ::write(STDOUT_FILENO, text, size);
    if (count <= 0) {
      return;
    }
    text += count;
    size -= static_cast<std::size_t>(count);
  }
#endif
}

void markerObserver(const gti_failure_record_v1 *, void *) {
  writeStdout("O", 1);
}

void throwingObserver(const gti_failure_record_v1 *, void *) {
  writeStdout("O", 1);
  throw 17;
}

void mutatingObserver(const gti_failure_record_v1 *record, void *) {
  writeStdout("O", 1);
  const_cast<gti_failure_record_v1 *>(record)->detail =
      GTI_FAILURE_DETAIL_SUBTRACTION_V1;
}

void reenteringObserver(const gti_failure_record_v1 *, void *) {
  writeStdout("O", 1);
  const gti_failure_record_v1 nested{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .code = GTI_FAILURE_CODE_MODULO_BY_ZERO_V1,
      .detail = GTI_FAILURE_DETAIL_INTEGER_MODULO_V1,
      .site_index = 0,
      .reserved = 0,
      .artifact_identity = {},
  };
  gti_rt_failure_terminate_v1(&nested, nullptr, markerObserver, nullptr);
}

void closeStderr() {
#if defined(_WIN32)
  (void)SetStdHandle(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE);
#else
  (void)::close(STDERR_FILENO);
#endif
}

bool breakStderrPipe() {
#if defined(_WIN32)
  HANDLE readHandle = nullptr;
  HANDLE writeHandle = nullptr;
  if (!CreatePipe(&readHandle, &writeHandle, nullptr, 0)) {
    return false;
  }
  (void)CloseHandle(readHandle);
  if (!SetStdHandle(STD_ERROR_HANDLE, writeHandle)) {
    (void)CloseHandle(writeHandle);
    return false;
  }
  return true;
#else
  int descriptors[2] = {-1, -1};
  if (::pipe(descriptors) != 0) {
    return false;
  }
  (void)::close(descriptors[0]);
  const bool replaced = ::dup2(descriptors[1], STDERR_FILENO) >= 0;
  (void)::close(descriptors[1]);
  return replaced;
#endif
}

int run(const char *mode) {
  constexpr char source[] = "unit.gti";
  Fixture ordinary(GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
                   GTI_FAILURE_DETAIL_ADDITION_V1, 1, source,
                   sizeof(source) - 1);

  if (std::strcmp(mode, "report") == 0) {
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&ordinary.record, &ordinary.artifact));
  }
  if (std::strcmp(mode, "runtime") == 0) {
    const gti_failure_record_v1 runtime{
        .abi_version = GTI_FAILURE_ABI_VERSION_V1,
        .code = GTI_FAILURE_CODE_ALLOCATION_FAILURE_V1,
        .detail = GTI_FAILURE_DETAIL_HOSTED_ARGUMENTS_V1,
        .site_index = 0,
        .reserved = 0,
        .artifact_identity = {},
    };
    return static_cast<int>(gti_rt_failure_write_report_v1(&runtime, nullptr));
  }
  if (std::strcmp(mode, "unicode") == 0) {
    constexpr char unicodeSource[] = {
        'A',        ' ',        '"',        '\\',       '\t',
        '\r',       '\n',       char(0xCE), char(0xBB), // Greek lambda, Letter
        char(0xCC), char(0x81),                         // combining acute, Mark
        char(0xC2), char(0xA0),                         // no-break space, Zs
        char(0xF0), char(0x9F), char(0x98), char(0x80), // symbol
        char(0xC2), char(0x85),                         // NEL, Control
        char(0xE2), char(0x80), char(0xA8),             // line separator
        char(0xE2), char(0x80), char(0xAE),             // bidi override, Format
        char(0xC0), char(0xAF), // overlong invalid UTF-8
        char(0xE2),             // truncated invalid UTF-8
        '\0',
    };
    Fixture unicode(GTI_FAILURE_CODE_INDEX_OUT_OF_BOUNDS_V1,
                    GTI_FAILURE_DETAIL_STRING_V1, 0x40, unicodeSource,
                    sizeof(unicodeSource), 9, 21, 23);
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&unicode.record, &unicode.artifact));
  }

  constexpr char secondarySource[] = "cleanup.gti";
  Fixture secondary(GTI_FAILURE_CODE_DIVISION_BY_ZERO_V1,
                    GTI_FAILURE_DETAIL_INTEGER_DIVISION_V1, 0x80,
                    secondarySource, sizeof(secondarySource) - 1, 11, 31, 32);
  const gti_failure_emergency_v1 emergency{
      .abi_version = GTI_FAILURE_ABI_VERSION_V1,
      .reserved = 0,
      .primary = ordinary.record,
      .secondary = secondary.record,
  };

  if (std::strcmp(mode, "cleanup") == 0) {
    return static_cast<int>(gti_rt_failure_write_cleanup_report_v1(
        &emergency, &ordinary.artifact, &secondary.artifact));
  }
  if (std::strcmp(mode, "invalid_outcome") == 0) {
    ordinary.outcome.detail = GTI_FAILURE_DETAIL_SUBTRACTION_V1;
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&ordinary.record, &ordinary.artifact));
  }
  if (std::strcmp(mode, "canonical_outcomes") == 0) {
    const gti_failure_outcome_descriptor_v1 outcomes[] = {
        {
            .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            .detail = GTI_FAILURE_DETAIL_DIVISION_V1,
            .reserved = 0,
        },
        {
            .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            .detail = GTI_FAILURE_DETAIL_MULTIPLICATION_V1,
            .reserved = 0,
        },
    };
    ordinary.site.outcomes = outcomes;
    ordinary.site.outcome_count = 2;
    ordinary.record.detail = GTI_FAILURE_DETAIL_MULTIPLICATION_V1;
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&ordinary.record, &ordinary.artifact));
  }
  if (std::strcmp(mode, "noncanonical_outcomes") == 0) {
    const gti_failure_outcome_descriptor_v1 outcomes[] = {
        {
            .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            .detail = GTI_FAILURE_DETAIL_MULTIPLICATION_V1,
            .reserved = 0,
        },
        {
            .code = GTI_FAILURE_CODE_INTEGER_OVERFLOW_V1,
            .detail = GTI_FAILURE_DETAIL_DIVISION_V1,
            .reserved = 0,
        },
    };
    ordinary.site.outcomes = outcomes;
    ordinary.site.outcome_count = 2;
    ordinary.record.detail = GTI_FAILURE_DETAIL_MULTIPLICATION_V1;
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&ordinary.record, &ordinary.artifact));
  }
  if (std::strcmp(mode, "write_closed") == 0) {
    closeStderr();
    return gti_rt_failure_write_report_v1(&ordinary.record,
                                          &ordinary.artifact) ==
                   GTI_FAILURE_REPORT_IO_ERROR_V1
               ? 0
               : 1;
  }
  if (std::strcmp(mode, "write_broken_pipe") == 0) {
    if (!breakStderrPipe()) {
      return 97;
    }
    return gti_rt_failure_write_report_v1(&ordinary.record,
                                          &ordinary.artifact) ==
                   GTI_FAILURE_REPORT_IO_ERROR_V1
               ? 0
               : 1;
  }
  if (std::strcmp(mode, "terminal_observer") == 0) {
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                markerObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_no_observer") == 0) {
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact, nullptr,
                                nullptr);
  }
  if (std::strcmp(mode, "terminal_exception") == 0) {
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                throwingObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_mutation") == 0) {
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                mutatingObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_reentry") == 0) {
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                reenteringObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_cleanup") == 0) {
    gti_rt_failure_terminate_cleanup_v1(&emergency, &ordinary.artifact,
                                        &secondary.artifact);
  }
  if (std::strcmp(mode, "terminal_closed") == 0) {
    closeStderr();
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                markerObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_broken_pipe") == 0) {
    if (!breakStderrPipe()) {
      return 97;
    }
    gti_rt_failure_terminate_v1(&ordinary.record, &ordinary.artifact,
                                markerObserver, nullptr);
  }
  if (std::strcmp(mode, "terminal_race") == 0) {
    constexpr char otherSource[] = "other.gti";
    static Fixture other(GTI_FAILURE_CODE_MODULO_BY_ZERO_V1,
                         GTI_FAILURE_DETAIL_INTEGER_MODULO_V1, 0xA0,
                         otherSource, sizeof(otherSource) - 1, 13, 41, 42);
    static std::atomic<int> ready{0};
    static std::atomic<bool> go{false};
    auto terminal = [&](Fixture *fixture) {
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire)) {
      }
      gti_rt_failure_terminate_v1(&fixture->record, &fixture->artifact,
                                  markerObserver, nullptr);
    };
    std::thread first(terminal, &ordinary);
    std::thread second(terminal, &other);
    while (ready.load(std::memory_order_acquire) != 2) {
    }
    go.store(true, std::memory_order_release);
    first.join();
    second.join();
  }
  return 99;
}

} // namespace

int main(int argc, char **argv) { return argc == 2 ? run(argv[1]) : 98; }
