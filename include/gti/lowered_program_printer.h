#pragma once

#include <string>

namespace lang {

class LoweredProgram;

class LoweredProgramPrinter final {
public:
  [[nodiscard]] std::string print(const LoweredProgram &program) const;
};

} // namespace lang
