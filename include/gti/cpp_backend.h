#pragma once

#include "gti/backend.h"
#include "gti/cpp_standard.h"

#include <string_view>

namespace lang {

class CppBackend final : public Backend {
public:
  explicit CppBackend(CppStandard standard = CppStandard::Cpp23);

  [[nodiscard]] std::string_view name() const override;
  [[nodiscard]] BackendArtifact generate(const BackendInput &input) override;

private:
  CppStandard standard;
};

} // namespace lang
