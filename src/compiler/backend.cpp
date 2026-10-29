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
  // LLVM is the default where its promotion evidence holds: macOS arm64 and
  // Linux x86_64. Other targets keep C++ until their own gates pass.
  const bool promoted = (target.os == "macos" && target.arch == "arm64") ||
                        (target.os == "linux" && target.arch == "x86_64");
  return promoted ? BackendKind::Llvm : BackendKind::Cpp;
}

} // namespace lang
