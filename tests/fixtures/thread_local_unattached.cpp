#include <cstdint>
#include <thread>

extern "C" void gti_tls_call_unattached(std::int32_t (*callback)()) {
  // Deliberately bypass GTI's checked manual/managed entry protocol. The test
  // proves first-use rejection before constructor effects or registration.
  std::thread foreign([=] { (void)callback(); });
  foreign.join();
}
