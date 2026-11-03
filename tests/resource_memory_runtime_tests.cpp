#include "gti/memory.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

int main() {
  int failures = 0;
  const auto expect = [&](bool okay, const char *message) {
    if (!okay) {
      ++failures;
      std::cerr << message << '\n';
    }
  };
  expect(gti_rt_native_size_max_v1() ==
             (std::numeric_limits<std::size_t>::max)(),
         "native size conversion uses the target size_t limit");
  const char text[] = {'a', static_cast<char>(255), '\0'};
  const auto *bytes = gti_rt_c_string_bytes_v1(text);
  expect(gti_rt_c_string_bytes_v1(nullptr) == nullptr &&
             bytes == reinterpret_cast<const std::uint8_t *>(text) &&
             bytes[0] == 97 && bytes[1] == 255 && bytes[2] == 0,
         "C-string bridge preserves null, address and all byte values");
  expect(!gti_rt_address_is_aligned_v1(nullptr, 1),
         "null does not satisfy nonempty-region alignment");
  alignas(64) unsigned char aligned[128]{};
  expect(gti_rt_address_is_aligned_v1(aligned, 64) &&
             !gti_rt_address_is_aligned_v1(aligned + 1, 64) &&
             gti_rt_address_is_aligned_v1(aligned + 1, 1) &&
             !gti_rt_address_is_aligned_v1(aligned, 0) &&
             !gti_rt_address_is_aligned_v1(aligned, 3),
         "address observer checks actual alignment without dereferencing");
#if !defined(_WIN32)
  expect(gti_rt_posix_pointer_size_v1() == sizeof(void *),
         "POSIX alignment normalization uses the actual pointer size");
  expect(gti_rt_posix_einval_v1() == EINVAL &&
             gti_rt_posix_enomem_v1() == ENOMEM,
         "POSIX errors use native constants, not guessed values");
#endif
  // Invalid/zero requests belong to source policy and are tested with an
  // injected adapter. These native calls satisfy the adapter preconditions.
  for (const std::uint64_t alignment : {8, 64, 4096}) {
    for (const std::uint64_t bytes : {1, 13, 128}) {
#if defined(_WIN32)
      auto *address = gti_rt_aligned_malloc_v1(bytes, alignment);
      expect(address != nullptr, "valid CRT allocation succeeds");
#else
      std::int32_t status = -1;
      auto *address = gti_rt_posix_memalign_v1(bytes, alignment, &status);
      expect(address && status == 0, "valid host allocation succeeds");
#endif
      if (!address)
        continue;
      expect(reinterpret_cast<std::uintptr_t>(address) % alignment == 0,
             "actual address meets the requested alignment");
      auto *data = static_cast<unsigned char *>(address);
      for (std::size_t index = 0; index < bytes; ++index)
        data[index] = static_cast<unsigned char>(index);
      for (std::size_t index = 0; index < bytes; ++index)
        expect(data[index] == static_cast<unsigned char>(index),
               "the complete extent is writable");
#if defined(_WIN32)
      gti_rt_aligned_free_v1(address);
#else
      gti_rt_posix_free_v1(address);
#endif
    }
  }
  return failures ? 1 : 0;
}
