// Host helper for contract terminations: a C++ terminate handler that exits
// with its own status, so std::terminate is distinguishable from abort.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

using Transform = std::int32_t (*)(std::int32_t);

extern "C" void contract_terminate_install() {
  std::set_terminate([] {
    std::fputs("terminate-handler\n", stderr);
    std::fflush(stderr);
    std::_Exit(86);
  });
}

extern "C" std::int32_t contract_invoke(Transform callback,
                                        std::int32_t value) {
  return callback(value);
}

extern "C" std::int32_t contract_invoke_guarded(Transform callback,
                                                std::int32_t value) {
  try {
    return callback(value);
  } catch (...) {
    std::fputs("escaped-through-C\n", stderr);
    return 87;
  }
}

extern "C" std::int32_t contract_throw(std::int32_t value) { throw value; }
