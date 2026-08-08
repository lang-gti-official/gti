#pragma once

#include "gti/mir.h"

#include <string>

namespace lang {

class MirPrinter {
public:
  [[nodiscard]] std::string print(const MirProgram &program) const;
  [[nodiscard]] std::string print(const MirBody &body) const;
};

} // namespace lang
