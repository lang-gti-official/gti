#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

// GTI's fixed-width uint64_t is not a spelling of native size_t. Until the
// native scalar ABI exposes size_t, these adapters make that conversion exact.
extern "C" std::uint64_t packet_native_size_max() {
  static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
  return (std::numeric_limits<std::size_t>::max)();
}
extern "C" void *packet_malloc(std::uint64_t bytes) {
  return std::malloc(static_cast<std::size_t>(bytes));
}

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
