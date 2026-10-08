#pragma once

#include "gti/mir.h"

#include <ostream>
#include <string>

namespace lang {

class MirPrinter {
public:
  [[nodiscard]] std::string print(const MirProgram &program) const;
  [[nodiscard]] std::string print(const MirBody &body) const;
  // Writes the same text as print(program) without materializing it.
  void print(const MirProgram &program, std::ostream &output) const;
};

} // namespace lang
