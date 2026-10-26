#include "gti/runtime_failure.h"
#include "gti/runtime_thread.hpp"

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

gti_failure_record_v2 panicRecord(const Fixture &fixture, const char *message,
                                  std::size_t size) {
  gti_failure_record_v2 record{.abi_version = GTI_FAILURE_ABI_VERSION_V2,
                               .code = GTI_FAILURE_CODE_PANIC_V2,
                               .detail = GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2,
                               .site_index = 1};
  std::memcpy(record.artifact_identity, fixture.record.artifact_identity, 32);
  (void)gti_rt_failure_capture_panic_v2(&record,
                                        {.data = message, .length = size});
  return record;
}

void markerObserverV2(const gti_failure_record_v2 *, void *) {
  writeStdout("O", 1);
}

void mutatingObserverV2(const gti_failure_record_v2 *record, void *) {
  writeStdout("O", 1);
  const_cast<gti_failure_record_v2 *>(record)->message[0] = 'X';
}

void throwingObserverV2(const gti_failure_record_v2 *, void *) {
  writeStdout("O", 1);
  throw 17;
}

void reenteringObserverV2(const gti_failure_record_v2 *, void *) {
  // Deliberately re-enter through the older ABI: arbitration must protect the
  // active v2 record regardless of the nested entry point's version.
  reenteringObserver(nullptr, nullptr);
}

struct PanicWorker {
  gti_failure_record_v2 record;
  const gti_failure_artifact_descriptor_v1 *artifact;
};

void panicWorker(void *opaque) {
  const auto &context = *static_cast<PanicWorker *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    gti_rt_failure_terminate_v2(&context.record, context.artifact, nullptr,
                                nullptr);
  });
}

void destroyPanicWorker(void *opaque) {
  delete static_cast<PanicWorker *>(opaque);
}

void nestedLegacyPanicWorker(void *opaque) {
  const auto &context = *static_cast<PanicWorker *>(opaque);
  gti_runtime_detail::run_managed_task_adapter([&] {
    auto inner = gti_rt_thread_spawn_v1(panicWorker, new PanicWorker(context),
                                        destroyPanicWorker);
    if (inner.status != GTI_THREAD_SPAWN_OK_V1) {
      std::terminate();
    }
    (void)gti_rt_thread_join_v1(&inner.state);
    writeStdout("unexpected continuation", 23);
  });
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

  if (std::strncmp(mode, "panic_", 6) == 0) {
    Fixture panic(GTI_FAILURE_CODE_PANIC_V2,
                  GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2, 3, source,
                  sizeof(source) - 1);
    auto record = panicRecord(panic, "cannot format Uuid", 18);
    if (std::strcmp(mode, "panic_report") == 0) {
      return static_cast<int>(
          gti_rt_failure_write_report_v2(&record, &panic.artifact));
    }
    if (std::strcmp(mode, "panic_snapshot") == 0) {
      char message[] = "cannot format Uuid";
      record = panicRecord(panic, message, sizeof(message) - 1);
      std::memset(message, 'X', sizeof(message));
      return static_cast<int>(
          gti_rt_failure_write_report_v2(&record, &panic.artifact));
    }
    if (std::strcmp(mode, "panic_truncated") == 0 ||
        std::strcmp(mode, "panic_utf8_boundary") == 0) {
      char message[600];
      std::memset(message, 'a', sizeof(message));
      if (std::strcmp(mode, "panic_utf8_boundary") == 0) {
        message[511] = char(0xCE);
        message[512] = char(0xBB);
      }
      record = panicRecord(panic, message, sizeof(message));
      return static_cast<int>(
          gti_rt_failure_write_report_v2(&record, &panic.artifact));
    }
    if (std::strcmp(mode, "panic_escaped") == 0) {
      constexpr char message[] = {
          'A',        '\0',       '\n',       '\r',       '\t',
          '"',        '\\',       char(0xC0), char(0xAF), char(0xE2),
          char(0x80), char(0xAE), char(0xCE), char(0xBB)};
      record = panicRecord(panic, message, sizeof(message));
      return static_cast<int>(
          gti_rt_failure_write_report_v2(&record, &panic.artifact));
    }
    if (std::strcmp(mode, "panic_cleanup") == 0 ||
        std::strcmp(mode, "panic_terminal_cleanup") == 0 ||
        std::strcmp(mode, "panic_context_cleanup") == 0) {
      Fixture secondary(GTI_FAILURE_CODE_PANIC_V2,
                        GTI_FAILURE_DETAIL_EXPLICIT_PANIC_V2, 5, "cleanup.gti",
                        11, 11);
      const gti_failure_emergency_v2 emergency{
          .abi_version = GTI_FAILURE_ABI_VERSION_V2,
          .primary = record,
          .secondary = panicRecord(secondary, "cleanup failed", 14)};
      if (std::strcmp(mode, "panic_context_cleanup") == 0) {
        const auto *root =
            gti_rt_failure_cleanup_enter_v1(&record, &panic.artifact);
        const auto *nested = gti_rt_failure_cleanup_enter_v1(
            &emergency.secondary, &secondary.artifact);
        if (root != &record || nested != nullptr)
          return 99;
        gti_rt_failure_cleanup_leave_v1(nested);
        gti_rt_failure_cleanup_check_v1(&emergency.secondary,
                                         &secondary.artifact);
        return 98;
      }
      if (std::strcmp(mode, "panic_terminal_cleanup") == 0) {
        gti_rt_failure_terminate_cleanup_v2(&emergency, &panic.artifact,
                                            &secondary.artifact);
      }
      return static_cast<int>(gti_rt_failure_write_cleanup_report_v2(
          &emergency, &panic.artifact, &secondary.artifact));
    }
    if (std::strcmp(mode, "panic_legacy_join") == 0 ||
        std::strcmp(mode, "panic_nested_legacy_join") == 0) {
      auto spawn = gti_rt_thread_spawn_v1(
          std::strcmp(mode, "panic_legacy_join") == 0 ? panicWorker
                                                      : nestedLegacyPanicWorker,
          new PanicWorker{record, &panic.artifact}, destroyPanicWorker);
      if (spawn.status != GTI_THREAD_SPAWN_OK_V1) {
        return 99;
      }
      if (std::strcmp(mode, "panic_legacy_join") == 0) {
        (void)gti_rt_thread_join_v1(&spawn.state);
      } else {
        // A legacy boundary must terminate, not hand off past its caller's
        // cleanup protocol and return a worker error to this outer join.
        (void)gti_rt_thread_join_v2(&spawn.state);
      }
      return 98;
    }
    gti_failure_observer_v2 observer = markerObserverV2;
    if (std::strcmp(mode, "panic_terminal_mutation") == 0) {
      observer = mutatingObserverV2;
    } else if (std::strcmp(mode, "panic_terminal_exception") == 0) {
      observer = throwingObserverV2;
    } else if (std::strcmp(mode, "panic_terminal_reentry") == 0) {
      observer = reenteringObserverV2;
    } else if (std::strcmp(mode, "panic_terminal_closed") == 0) {
      closeStderr();
    } else if (std::strcmp(mode, "panic_terminal_broken_pipe") == 0) {
      if (!breakStderrPipe()) {
        return 99;
      }
    } else if (std::strcmp(mode, "panic_terminal_observer") != 0) {
      return 97;
    }
    gti_rt_failure_terminate_v2(&record, &panic.artifact, observer, nullptr);
  }

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
  if (std::strcmp(mode, "key_not_found") == 0 ||
      std::strcmp(mode, "container_argument") == 0 ||
      std::strcmp(mode, "mutex_lock") == 0 ||
      std::strcmp(mode, "reserved_14") == 0) {
    const bool key = std::strcmp(mode, "key_not_found") == 0;
    const bool mutex = std::strcmp(mode, "mutex_lock") == 0;
    const bool reserved = std::strcmp(mode, "reserved_14") == 0;
    Fixture check(reserved ? 14
                  : key    ? GTI_FAILURE_CODE_KEY_NOT_FOUND_V1
                  : mutex ? GTI_FAILURE_CODE_INFALLIBLE_HOST_OPERATION_FAILED_V1
                          : GTI_FAILURE_CODE_INVALID_CONTAINER_ARGUMENT_V1,
                  key     ? GTI_FAILURE_DETAIL_KEY_LOOKUP_V1
                  : mutex ? GTI_FAILURE_DETAIL_MUTEX_LOCK_V1
                          : GTI_FAILURE_DETAIL_CONTAINER_ARGUMENT_V1,
                  1, "unit.gti", 8);
    return static_cast<int>(
        gti_rt_failure_write_report_v1(&check.record, &check.artifact));
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
