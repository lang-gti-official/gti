#include "gti/runtime.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::int32_t kEndOfFile = -1;
constexpr std::int32_t kReadFailure = -2;
std::mutex stdoutMutex;
std::mutex stderrMutex;

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

std::int64_t writeStandardStream(std::FILE *stream, std::mutex &mutex,
                                 gti_c_string_view value) {
  if (value.length == 0) {
    return 0;
  }
  if (value.data == nullptr || value.length > SIZE_MAX ||
      value.length > static_cast<std::uint64_t>(INT64_MAX)) {
    return -1;
  }

  const std::lock_guard lock(mutex);
  const std::size_t length = static_cast<std::size_t>(value.length);
  std::size_t written = 0;
  while (written < length) {
    const std::size_t count =
        std::fwrite(value.data + written, 1, length - written, stream);
    if (count == 0) {
      return -1;
    }
    written += count;
  }
  return static_cast<std::int64_t>(written);
}

std::int64_t readBytes(int descriptor, std::uint8_t *output,
                       std::uint64_t capacity) {
  if (capacity == 0) {
    return 0;
  }
  if (output == nullptr) {
    return -1;
  }
#if defined(_WIN32)
  if (capacity > UINT_MAX) {
    return -1;
  }
  int count = 0;
  do {
    count = _read(descriptor, output, static_cast<unsigned int>(capacity));
  } while (count < 0 && errno == EINTR);
#else
  if (capacity >
      static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())) {
    return -1;
  }
  ssize_t count = 0;
  do {
    count = ::read(descriptor, output, static_cast<std::size_t>(capacity));
  } while (count < 0 && errno == EINTR);
#endif
  return count < 0 ? -1 : static_cast<std::int64_t>(count);
}

std::int64_t writeBytes(int descriptor, const char *input,
                        std::uint64_t length) {
  if (length == 0) {
    return 0;
  }
  if (input == nullptr || length > static_cast<std::uint64_t>(INT64_MAX)) {
    return -1;
  }
  std::uint64_t written = 0;
  while (written < length) {
    const std::uint64_t remaining = length - written;
#if defined(_WIN32)
    const unsigned int request =
        static_cast<unsigned int>(std::min<std::uint64_t>(remaining, UINT_MAX));
    int count = 0;
    do {
      count = _write(descriptor, input + written, request);
    } while (count < 0 && errno == EINTR);
#else
    const std::size_t request =
        static_cast<std::size_t>(std::min<std::uint64_t>(
            remaining,
            static_cast<std::uint64_t>(std::numeric_limits<ssize_t>::max())));
    ssize_t count = 0;
    do {
      count = ::write(descriptor, input + written, request);
    } while (count < 0 && errno == EINTR);
#endif
    if (count <= 0) {
      return -1;
    }
    written += static_cast<std::uint64_t>(count);
  }
  return static_cast<std::int64_t>(written);
}

int openFlags(gti_file_open_mode_v1 mode) {
  switch (mode) {
  case GTI_FILE_OPEN_READ_V1:
    return O_RDONLY;
  case GTI_FILE_OPEN_WRITE_V1:
    return O_WRONLY | O_CREAT | O_TRUNC;
  case GTI_FILE_OPEN_APPEND_V1:
    return O_WRONLY | O_CREAT | O_APPEND;
  case GTI_FILE_OPEN_READ_WRITE_V1:
    return O_RDWR;
  case GTI_FILE_OPEN_READ_WRITE_TRUNCATE_V1:
    return O_RDWR | O_CREAT | O_TRUNC;
  case GTI_FILE_OPEN_READ_APPEND_V1:
    return O_RDWR | O_CREAT | O_APPEND;
  default:
    return -1;
  }
}

int seekOrigin(gti_file_seek_origin_v1 origin) {
  switch (origin) {
  case GTI_FILE_SEEK_BEGIN_V1:
    return SEEK_SET;
  case GTI_FILE_SEEK_CURRENT_V1:
    return SEEK_CUR;
  case GTI_FILE_SEEK_END_V1:
    return SEEK_END;
  default:
    return -1;
  }
}

} // namespace

extern "C" std::int32_t gti_rt_write_stdout(gti_c_string_view value) {
  const std::int64_t count = writeStandardStream(stdout, stdoutMutex, value);
  return count >= 0 && static_cast<std::uint64_t>(count) == value.length ? 0
                                                                         : 1;
}

extern "C" std::int32_t gti_rt_write_stdout_byte(std::uint8_t value) {
  const gti_c_string_view view{.data = reinterpret_cast<const char *>(&value),
                               .length = 1};
  return writeStandardStream(stdout, stdoutMutex, view) == 1 ? 0 : 1;
}

extern "C" std::int64_t gti_rt_write_stdout_bytes_v1(gti_c_string_view value) {
  return writeStandardStream(stdout, stdoutMutex, value);
}

extern "C" std::int64_t gti_rt_write_stderr_bytes_v1(gti_c_string_view value) {
  return writeStandardStream(stderr, stderrMutex, value);
}

extern "C" std::int32_t gti_rt_flush_stdout_v1(void) {
  const std::lock_guard lock(stdoutMutex);
  return std::fflush(stdout) == 0 ? 0 : 1;
}

extern "C" std::int32_t gti_rt_flush_stderr_v1(void) {
  const std::lock_guard lock(stderrMutex);
  return std::fflush(stderr) == 0 ? 0 : 1;
}

extern "C" std::int32_t gti_rt_read_stdin_byte(void) {
#if defined(_WIN32)
  return readByte(_fileno(stdin));
#else
  return readByte(STDIN_FILENO);
#endif
}

extern "C" std::int64_t gti_rt_read_stdin_bytes_v1(std::uint8_t *output,
                                                   std::uint64_t capacity) {
#if defined(_WIN32)
  return readBytes(_fileno(stdin), output, capacity);
#else
  return readBytes(STDIN_FILENO, output, capacity);
#endif
}

extern "C" std::int64_t gti_rt_open_file_read(gti_c_string_view path) {
  return gti_rt_open_file_v1(path, GTI_FILE_OPEN_READ_V1);
}

extern "C" std::int64_t gti_rt_open_file_v1(gti_c_string_view path,
                                            gti_file_open_mode_v1 mode) {
  if (path.data == nullptr || path.length == 0 || path.length > SIZE_MAX) {
    return -1;
  }

  int flags = openFlags(mode);
  if (flags < 0) {
    return -1;
  }

  try {
    const std::string ownedPath(path.data,
                                static_cast<std::size_t>(path.length));
    if (ownedPath.find('\0') != std::string::npos) {
      return -1;
    }

#if defined(_WIN32)
    flags |= _O_BINARY;
    const int descriptor =
        _open(ownedPath.c_str(), flags, _S_IREAD | _S_IWRITE);
#else
#if defined(O_CLOEXEC)
    flags |= O_CLOEXEC;
#endif
    const int descriptor = ::open(ownedPath.c_str(), flags, 0666);
#endif
    return descriptor < 0 ? -1 : static_cast<std::int64_t>(descriptor);
  } catch (...) {
    // C ABI entry points must not allow a C++ allocation failure to escape.
    return -1;
  }
}

extern "C" std::int32_t gti_rt_read_file_byte(std::int64_t descriptor) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  return nativeDescriptor < 0 ? kReadFailure : readByte(nativeDescriptor);
}

extern "C" std::int64_t gti_rt_read_file_v1(std::int64_t descriptor,
                                            std::uint8_t *output,
                                            std::uint64_t capacity) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  return nativeDescriptor < 0 ? -1
                              : readBytes(nativeDescriptor, output, capacity);
}

extern "C" std::int64_t gti_rt_write_file_v1(std::int64_t descriptor,
                                             gti_c_string_view value) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  return nativeDescriptor < 0
             ? -1
             : writeBytes(nativeDescriptor, value.data, value.length);
}

extern "C" std::int64_t gti_rt_seek_file_v1(std::int64_t descriptor,
                                            std::int64_t offset,
                                            gti_file_seek_origin_v1 origin) {
  const int nativeDescriptor = narrowDescriptor(descriptor);
  const int nativeOrigin = seekOrigin(origin);
  if (nativeDescriptor < 0 || nativeOrigin < 0) {
    return -1;
  }
#if defined(_WIN32)
  const __int64 position = _lseeki64(nativeDescriptor, offset, nativeOrigin);
#else
  off_t position = 0;
  do {
    position =
        ::lseek(nativeDescriptor, static_cast<off_t>(offset), nativeOrigin);
  } while (position < 0 && errno == EINTR);
#endif
  if (position < 0 || static_cast<std::uint64_t>(position) >
                          static_cast<std::uint64_t>(INT64_MAX)) {
    return -1;
  }
  return static_cast<std::int64_t>(position);
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
