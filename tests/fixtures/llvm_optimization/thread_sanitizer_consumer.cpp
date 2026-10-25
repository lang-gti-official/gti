#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

extern "C" int gti_test_read(int *, int);
extern "C" void gti_test_write(int *, int, int);
#define DECLARE_ATOMIC(BITS) \
  extern "C" void gti_test_atomic_store##BITS(void *, std::uint64_t); \
  extern "C" std::uint64_t gti_test_atomic_load##BITS(void *);
DECLARE_ATOMIC(8)
DECLARE_ATOMIC(16)
DECLARE_ATOMIC(32)
DECLARE_ATOMIC(64)
#undef DECLARE_ATOMIC

template <class Load> bool awaitPublished(Load load) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (load() != 1) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    // Test synchronization only: this is not an implementation of GTI wait.
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  return true;
}

template <class T>
bool publication(bool generatedProducer, void (*store)(void *, std::uint64_t),
                 std::uint64_t (*load)(void *)) {
  static_assert(__atomic_always_lock_free(sizeof(T), nullptr));
  alignas(sizeof(T)) T flag = 0;
  int payload = 0;
  // There is no shared mutex or join before the payload read. Only the flag's
  // release/acquire pair can publish this otherwise unprotected payload.
  std::thread producer([&] {
    if (generatedProducer) {
      gti_test_write(&payload, 0, 73);
      store(&flag, 1);
    } else {
      payload = 73;
      __atomic_store_n(&flag, T(1), __ATOMIC_RELEASE);
    }
  });
  const bool ready = awaitPublished([&]() -> std::uint64_t {
    return generatedProducer ? __atomic_load_n(&flag, __ATOMIC_ACQUIRE) : load(&flag);
  });
  const bool valid = ready && gti_test_read(&payload, 0) == 73;
  producer.join();
  return valid;
}

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  if (!std::strcmp(argv[1], "safe")) {
    int value = 0;
    gti_test_write(&value, 0, 73);
    return gti_test_read(&value, 0) == 73 ? 0 : 3;
  }
  if (!std::strcmp(argv[1], "plain-race") ||
      !std::strcmp(argv[1], "atomic-plain-race")) {
    int value = 0;
    const bool atomicHost = !std::strcmp(argv[1], "atomic-plain-race");
    std::thread generated([&] {
      for (int index = 0; index < 20000; ++index)
        gti_test_write(&value, 0, index);
    });
    for (int index = 0; index < 20000; ++index) {
      if (atomicHost)
        __atomic_store_n(&value, index, __ATOMIC_RELAXED);
      else
        value = index;
    }
    generated.join();
    return 0;
  }
  const bool generated = !std::strncmp(argv[1], "generated-publication-", 22);
  const bool host = !std::strncmp(argv[1], "host-publication-", 17);
  if (!generated && !host)
    return 2;
  const char *width = argv[1] + (generated ? 22 : 17);
#define CHECK_WIDTH(BITS) \
  if (!std::strcmp(width, #BITS)) \
    return publication<std::uint##BITS##_t>(generated, gti_test_atomic_store##BITS, \
                                          gti_test_atomic_load##BITS) ? 0 : 4;
  CHECK_WIDTH(8)
  CHECK_WIDTH(16)
  CHECK_WIDTH(32)
  CHECK_WIDTH(64)
#undef CHECK_WIDTH
  return 2;
}
