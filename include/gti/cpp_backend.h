#pragma once

#include "gti/backend.h"
#include "gti/cpp_standard.h"

#include <string_view>

namespace lang {

class CppBackend final : public Backend {
public:
  explicit CppBackend(CppStandard standard = CppStandard::Cpp23,
                      bool debugInfo = false);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] BackendArtifact
  generate(const LoweredProgram &program) override;

private:
  CppStandard standard;
  bool debugInfo;
};

} // namespace lang
