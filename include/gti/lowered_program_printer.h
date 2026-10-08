#pragma once

#include <ostream>
#include <string>

namespace lang {

class LoweredProgram;

class LoweredProgramPrinter final {
public:
  [[nodiscard]] std::string print(const LoweredProgram &program) const;
  // Writes the same text as print(program) without materializing it.
  void print(const LoweredProgram &program, std::ostream &output) const;
};

} // namespace lang
