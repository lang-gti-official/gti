#pragma once

#include "gti/optimization/rewrite.h"

#include <cstddef>
#include <string>
#include <vector>

namespace lang {

struct OptimizationPassReport {
  std::string name;
  bool changed = false;
  std::size_t appliedEdits = 0;
  std::size_t shadowComparisons = 0;
  std::size_t shadowMismatches = 0;
  bool valueUsesRebuilt = false;
  MirAnalysisInvalidation invalidation;
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
