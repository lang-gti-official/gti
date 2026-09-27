#include "gti/runtime.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

extern "C" std::int32_t gti_rt_run_process_v1(gti_c_string_view arguments) {
#if defined(_WIN32)
  (void)arguments;
  return 127;
#else
  if (arguments.data == nullptr || arguments.length == 0 ||
      arguments.length > SIZE_MAX ||
      arguments.data[arguments.length - 1] != '\0') {
    return 127;
  }

  std::vector<std::string> storage;
  std::size_t start = 0;
  const std::size_t length = static_cast<std::size_t>(arguments.length);
  while (start < length) {
    std::size_t end = start;
    while (end < length && arguments.data[end] != '\0') {
      ++end;
    }
    if (end == length) {
      return 127;
    }
    storage.emplace_back(arguments.data + start, end - start);
    start = end + 1;
  }
  if (storage.empty() || storage.front().empty()) {
    return 127;
  }

  std::vector<char *> argv;
  argv.reserve(storage.size() + 1);
  for (std::string &argument : storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  std::fflush(stdout);
  std::fflush(stderr);
  const pid_t child = fork();
  if (child == -1) {
    return 127;
  }
  if (child == 0) {
    execvp(argv.front(), argv.data());
    const int executeError = errno;
    std::fprintf(stderr, "gti: failed to execute build command '%s': %s\n",
                 storage.front().c_str(), std::strerror(executeError));
    std::fflush(stderr);
    _exit(127);
  }

  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno == EINTR) {
      continue;
    }
    return 127;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 127;
#endif
}
