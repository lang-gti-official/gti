#include "gti/backend.h"
#include "gti/target.h"

namespace lang {

std::string_view backendKindName(BackendKind backend) {
  switch (backend) {
  case BackendKind::Cpp:
    return "cpp";
  case BackendKind::Llvm:
    return "llvm";
  }
  return "unknown";
}

std::optional<BackendKind> parseBackendKind(std::string_view name) {
  if (name == "cpp") {
    return BackendKind::Cpp;
  }
  if (name == "llvm") {
    return BackendKind::Llvm;
  }
  return std::nullopt;
}

BackendKind defaultBackend(const TargetInfo &) {
  // LLVM is the only default. The frozen C++ backend is selected only
  // explicitly, and only in a toolchain built with it.
  return BackendKind::Llvm;
}

} // namespace lang
