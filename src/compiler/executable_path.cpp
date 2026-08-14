#include "gti/executable_path.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace lang {
namespace detail {

std::filesystem::path nativeExecutablePath() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(512);
  while (buffer.size() <= 32768) {
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) {
      return {};
    }
    if (length < buffer.size()) {
      return std::filesystem::path(
          std::wstring(buffer.data(), static_cast<std::size_t>(length)));
    }
    buffer.resize(buffer.size() * 2);
  }
#elif defined(__APPLE__)
  std::vector<char> buffer(1024);
  std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    buffer.resize(size);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
      return {};
    }
  }
  return std::filesystem::path(buffer.data());
#elif defined(__linux__)
  std::vector<char> buffer(1024);
  while (buffer.size() <= 1024 * 1024) {
    const ssize_t length =
        readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0) {
      return {};
    }
    if (static_cast<std::size_t>(length) < buffer.size()) {
      return std::filesystem::path(
          std::string(buffer.data(), static_cast<std::size_t>(length)));
    }
    buffer.resize(buffer.size() * 2);
  }
#endif
  return {};
}

std::filesystem::path executableFromArgument(const char *driver) {
  if (driver == nullptr || *driver == '\0') {
    return {};
  }

  const std::filesystem::path argument(driver);
  std::error_code error;
  if (argument.has_parent_path()) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(argument, error);
    return error ? argument : absolute;
  }

  const char *environment = std::getenv("PATH");
  if (environment == nullptr) {
    return argument;
  }

#if defined(_WIN32)
  constexpr char separator = ';';
#else
  constexpr char separator = ':';
#endif
  const std::string paths(environment);
  std::size_t start = 0;
  while (start <= paths.size()) {
    const std::size_t end = paths.find(separator, start);
    const std::string entry = paths.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const std::filesystem::path directory =
        entry.empty() ? std::filesystem::current_path(error)
                      : std::filesystem::path(entry);
    error.clear();
    const std::filesystem::path candidate = directory / argument;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return argument;
}

} // namespace detail

std::filesystem::path executablePath(const char *driver) {
  std::filesystem::path executable = detail::nativeExecutablePath();
  if (executable.empty()) {
    executable = detail::executableFromArgument(driver);
  }

  std::error_code error;
  const std::filesystem::path canonical =
      std::filesystem::weakly_canonical(executable, error);
  if (!error) {
    return canonical;
  }

  error.clear();
  const std::filesystem::path absolute =
      std::filesystem::absolute(executable, error);
  return error ? executable : absolute;
}

} // namespace lang
