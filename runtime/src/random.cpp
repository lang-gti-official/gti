#include "gti/random.h"

#if defined(_WIN32)
#include <windows.h>
// bcrypt.h must follow windows.h.
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h>
#else
#include <cerrno>
#include <sys/random.h>
#endif

extern "C" int32_t gti_rt_random_bytes(uint8_t *out, uint64_t count) {
  if (count == 0) {
    return 0;
  }
  if (out == nullptr) {
    return -1;
  }

#if defined(_WIN32)
  // BCryptGenRandom takes a 32-bit length, so a larger request is chunked.
  uint64_t filled = 0;
  while (filled < count) {
    const uint64_t remaining = count - filled;
    const ULONG chunk =
        remaining > 0xFFFFFFFFull ? 0xFFFFFFFFul : (ULONG)remaining;
    const NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR)(out + filled), chunk,
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(status)) {
      return -1;
    }
    filled += chunk;
  }
  return 0;
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
  // arc4random_buf cannot fail and always fills the whole buffer.
  arc4random_buf(out, (size_t)count);
  return 0;
#else
  // getrandom may return short and may be interrupted, so it is drained in a
  // loop. Reads of up to 256 bytes from an initialized pool are uninterrupted,
  // but nothing guarantees that for larger buffers.
  uint64_t filled = 0;
  while (filled < count) {
    const ssize_t written =
        getrandom(out + filled, (size_t)(count - filled), 0);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (written == 0) {
      return -1;
    }
    filled += (uint64_t)written;
  }
  return 0;
#endif
}
