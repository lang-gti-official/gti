#pragma once

#include "gti/backend.h"

#include <string_view>

namespace lang {

struct LoweredEmbeddingExport;
// Canonical structural signature using the same C projection as the header.
[[nodiscard]] std::string nativeEmbeddingSignature(
    const LoweredProgram &program, const LoweredEmbeddingExport &selected);

// Emits the compiler-owned native bridge header for the checked program. The
// artifact is valid as both C17 and C++20/C++23. C++ consumers see the exact
// source namespaces while C consumers receive deterministic flattened record
// names. Layout-stable records are defined, opaque handles remain incomplete,
// and both branches declare the same external C symbols.
// Embedding libraries use the canonical C type names in both languages and
// declare only selected generated exports, not the module's native imports.
class NativeHeaderBackend final : public Backend {
public:
  [[nodiscard]] std::string_view name() const override {
    return "native-header";
  }

  [[nodiscard]] BackendArtifact generate(LoweredProgram program) override;
};

} // namespace lang
