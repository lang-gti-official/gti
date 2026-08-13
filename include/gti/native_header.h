#pragma once

#include "gti/backend.h"

#include <string_view>

namespace lang {

// Emits the compiler-owned native bridge header for the checked program. The
// artifact is valid as both C17 and C++20/C++23. C++ consumers see the exact
// source namespaces while C consumers receive deterministic flattened record
// names. Layout-stable records are defined, opaque handles remain incomplete,
// and both branches declare the same external C symbols.
class NativeHeaderBackend final : public Backend {
public:
  [[nodiscard]] std::string_view name() const override {
    return "native-header";
  }

  [[nodiscard]] BackendArtifact generate(const BackendInput &input) override;
};

} // namespace lang
