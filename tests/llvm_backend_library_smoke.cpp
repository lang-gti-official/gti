#include "gti/llvm_backend.h"

#include <type_traits>

static_assert(std::is_final_v<lang::LlvmBackend>);

int main() {
  const lang::LlvmBackend backend;
  return backend.name() == "llvm" ? 0 : 1;
}
