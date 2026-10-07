#include "gti/memory.h"

#include <cstddef>
#include <cstdint>
#include <iostream>

int main() {
  int failures = 0;
  const auto expect = [&](bool okay, const char *message) {
    if (!okay) {
      ++failures;
      std::cerr << message << '\n';
    }
  };
  std::int32_t status = -1;
  for (const std::uint64_t alignment : {0, 3, 6}) {
    auto *address = gti_rt_memory_allocate_v1(0, alignment, &status);
    expect(!address && status == 1,
           "invalid alignment is recoverable, even empty");
  }
  for (const std::uint64_t alignment : {1, 2, 8, 64, 4096}) {
    auto *empty = gti_rt_memory_allocate_v1(0, alignment, &status);
    expect(!empty && status == 0,
           "empty allocation performs no host allocation");
    gti_rt_memory_release_v1(empty, alignment);
    for (const std::uint64_t bytes : {1, 13, 128}) {
      auto *address = gti_rt_memory_allocate_v1(bytes, alignment, &status);
      expect(address && status == 0, "valid host allocation succeeds");
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
      gti_rt_memory_release_v1(address, alignment);
    }
  }
  return failures ? 1 : 0;
}
