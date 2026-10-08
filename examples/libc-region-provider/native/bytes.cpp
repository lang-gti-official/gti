#include <cstdint>

// A native byte consumer is intentionally unrelated to GTI typed storage.
// It borrows a writable extent only for this call and retains no pointer.
extern "C" std::uint64_t packet_fill_checksum(void *address,
                                              std::uint64_t bytes) {
  auto *data = static_cast<unsigned char *>(address);
  std::uint64_t checksum = 0;
  for (std::uint64_t index = 0; index < bytes; ++index) {
    data[index] = static_cast<unsigned char>(index % 251);
    checksum += data[index];
  }
  return checksum;
}
