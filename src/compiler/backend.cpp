#include "gti/backend.h"

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

} // namespace lang
