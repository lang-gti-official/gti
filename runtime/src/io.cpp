#include "gti/runtime.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::int32_t kEndOfFile = -1;
constexpr std::int32_t kReadFailure = -2;

int narrowDescriptor(std::int64_t descriptor) {
  if (descriptor < 0 || descriptor > INT_MAX) {
    return -1;
  }
  return static_cast<int>(descriptor);
}

std::int32_t readByte(int descriptor) {
  unsigned char byte = 0;
#if defined(_WIN32)
  const int count = _read(descriptor, &byte, 1);
  if (count == 1) {
    return static_cast<std::int32_t>(byte);
  }
  return count == 0 ? kEndOfFile : kReadFailure;
#else
  ssize_t count = 0;
  do {
    count = ::read(descriptor, &byte, 1);
  } while (count < 0 && errno == EINTR);
  if (count == 1) {
    return static_cast<std::int32_t>(byte);
  }
  return count == 0 ? kEndOfFile : kReadFailure;
#endif
}

} // namespace

extern "C" int gti_rt_write_stdout(const char *data, size_t length) {
  if (data == nullptr && length != 0) {
    return 1;
  }
  return std::fwrite(data, 1, length, stdout) == length ? 0 : 1;
}

extern "C" std::int32_t gti_rt_read_stdin_byte(void) {
#if defined(_WIN32)
  return readByte(_fileno(stdin));
#else
  return readByte(STDIN_FILENO);
#endif
}

extern "C" std::int64_t gti_rt_open_file_read(const char *path, size_t length) {
  if (path == nullptr || length == 0) {
    return -1;
  }

  const std::string ownedPath(path, length);
  if (ownedPath.find('\0') != std::string::npos) {
    return -1;
  }

#if defined(_WIN32)
  const int descriptor = _open(ownedPath.c_str(), _O_RDONLY | _O_BINARY);
#else
#if defined(O_CLOEXEC)
  const int descriptor = ::open(ownedPath.c_str(), O_RDONLY | O_CLOEXEC);
#else
  const int descriptor = ::open(ownedPath.c_str(), O_RDONLY);
#endif
#endif
  return descriptor < 0 ? -1 : static_cast<std::int64_t>(descriptor);
}

extern "C" std::int32_t gti_rt_read_file_byte(std::int64_t descriptor) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  return nativeDescriptor < 0 ? kReadFailure : readByte(nativeDescriptor);
}

extern "C" std::int32_t gti_rt_close_file(std::int64_t descriptor) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  if (nativeDescriptor < 0) {
    return 1;
  }
#if defined(_WIN32)
  return _close(nativeDescriptor) == 0 ? 0 : 1;
#else
  return ::close(nativeDescriptor) == 0 ? 0 : 1;
#endif
}
