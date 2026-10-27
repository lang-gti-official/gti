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

BackendKind defaultBackend(const TargetInfo &target) {
  return target.os == "macos" && target.arch == "arm64" ? BackendKind::Llvm
                                                        : BackendKind::Cpp;
}

} // namespace lang
