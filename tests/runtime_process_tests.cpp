#include "gti/runtime.h"

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "runtime process test failure: " << message << '\n';
  ++failures;
}

gti_c_string_view view(std::string_view value) {
  return {.data = value.data(),
          .length = static_cast<std::uint64_t>(value.size())};
}

gti_process_error_v1 run(std::string_view encoded,
                         std::int32_t &terminationKind,
                         std::int32_t &terminationValue,
                         std::int32_t &nativeCode) {
  return gti_rt_process_run_v1(view(encoded), &terminationKind,
                               &terminationValue, &nativeCode);
}

} // namespace

int main() {
  std::int32_t kind = -1;
  std::int32_t value = -1;
  std::int32_t native = -1;

#if defined(_WIN32)
  constexpr char commandText[] = "cmd\0/c\0exit 0\0";
  const std::string command{commandText, sizeof(commandText) - 1};
  expect(run(command, kind, value, native) == GTI_PROCESS_UNSUPPORTED_V1,
         "the deferred Windows boundary must fail explicitly");
#else
  constexpr char successfulText[] = "sh\0-c\0exit 23\0";
  const std::string successful{successfulText, sizeof(successfulText) - 1};
  expect(run(successful, kind, value, native) == GTI_PROCESS_OK_V1,
         "an exact argv command should execute");
  expect(kind == GTI_PROCESS_TERMINATION_EXITED_V1 && value == 23 &&
             native == 0,
         "ordinary exit must retain its exact code");
  expect(run(successful, kind, value, native) == GTI_PROCESS_OK_V1 &&
             kind == GTI_PROCESS_TERMINATION_EXITED_V1 && value == 23,
         "the same exact argv payload must be reusable");

  constexpr char exit127Text[] = "sh\0-c\0exit 127\0";
  const std::string exit127{exit127Text, sizeof(exit127Text) - 1};
  expect(run(exit127, kind, value, native) == GTI_PROCESS_OK_V1 &&
             kind == GTI_PROCESS_TERMINATION_EXITED_V1 && value == 127,
         "a child exit 127 must remain distinct from execution failure");

  constexpr char exactArgumentsText[] =
      "sh\0-c\0test \"$1\" = \"two words\" && test -z \"$2\"\0"
      "gti-process-test\0two words\0\0";
  const std::string exactArguments{exactArgumentsText,
                                   sizeof(exactArgumentsText) - 1};
  expect(run(exactArguments, kind, value, native) == GTI_PROCESS_OK_V1 &&
             kind == GTI_PROCESS_TERMINATION_EXITED_V1 && value == 0,
         "spaces and an empty argument must survive without shell splitting");

  constexpr char signaledText[] = "sh\0-c\0kill -TERM $$\0";
  const std::string signaled{signaledText, sizeof(signaledText) - 1};
  expect(run(signaled, kind, value, native) == GTI_PROCESS_OK_V1,
         "signal termination should still be a completed process");
  expect(kind == GTI_PROCESS_TERMINATION_SIGNALED_V1 && value == SIGTERM,
         "signal termination must remain distinct from an exit code");

  constexpr char missingText[] =
      "gti-process-command-that-does-not-exist-7d4c9f3e\0";
  const std::string missing{missingText, sizeof(missingText) - 1};
  expect(run(missing, kind, value, native) == GTI_PROCESS_EXECUTE_FAILED_V1,
         "exec failure must not masquerade as child exit 127");
  expect(native == ENOENT,
         "exec failure should retain host detail at the private ABI");
#endif

  expect(run("unterminated", kind, value, native) ==
             GTI_PROCESS_INVALID_ARGUMENT_V1,
         "an unterminated argv payload must be rejected");
  const std::string emptyExecutable{"\0", 1};
  expect(run(emptyExecutable, kind, value, native) ==
             GTI_PROCESS_INVALID_ARGUMENT_V1,
         "an empty executable must be rejected");
  const std::string oneCommand{"true\0", 5};
  expect(gti_rt_process_run_v1(view(oneCommand), nullptr, &value, &native) ==
             GTI_PROCESS_INVALID_ARGUMENT_V1,
         "null result pointers must be rejected");

#if !defined(_WIN32)
  constexpr char legacyText[] = "sh\0-c\0exit 7\0";
  expect(gti_rt_run_process_v1(
             view(std::string_view{legacyText, sizeof(legacyText) - 1})) == 7,
         "the package-build compatibility entry must preserve exit status");
#endif

  return failures == 0 ? 0 : 1;
}
