#pragma once

#include "gti/backend.h"
#include "gti/cpp_emitter.h"

#include <string_view>

namespace lang {

class CppBackend final : public Backend {
public:
  explicit CppBackend(CppStandard standard = CppStandard::Cpp23)
      : standard(standard) {}

  [[nodiscard]] std::string_view name() const override { return "cpp"; }

  [[nodiscard]] BackendArtifact generate(const BackendInput &input) override {
    return {.kind = BackendArtifactKind::Source,
            .contents = CppEmitter(standard, input.target, &input.optimizations,
                                   &input.semantics, &input.hir)
                            .emit(input.program),
            .extension = ".cpp"};
  }

private:
  CppStandard standard;
};

} // namespace lang
