#pragma once

#include "gti/mir.h"

#include <string>
#include <vector>

namespace lang {

struct OptimizationPassReport {
  std::string name;
  bool changed = false;
};

struct OptimizationReport {
  bool verificationEnabled = false;
  MirVerificationResult inputVerification;
  MirVerificationResult outputVerification;
  std::vector<OptimizationPassReport> passes;

  [[nodiscard]] bool valid() const {
    return inputVerification.valid() && outputVerification.valid();
  }
};

} // namespace lang
