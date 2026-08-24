#pragma once

#include "gti/optimization/rewrite.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

enum class OptimizationRemarkKind : std::uint8_t { Applied, Missed };

struct OptimizationRemark {
  OptimizationRemarkKind kind = OptimizationRemarkKind::Applied;
  std::string pass;
  std::string message;
  MirBodyAddress body;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  MirPlaceId place = 0;
  HirValueId sourceValue = 0;
  HirStatementId sourceStatement = 0;
  MirTransformProofKind proof = MirTransformProofKind::Count;
};

struct OptimizationPassReport {
  std::string name;
  bool changed = false;
  std::size_t iterations = 0;
  std::size_t analyzedBodies = 0;
  std::size_t candidates = 0;
  std::size_t missed = 0;
  std::size_t appliedEdits = 0;
  std::size_t shadowComparisons = 0;
  std::size_t shadowMismatches = 0;
  bool valueUsesRebuilt = false;
  MirAnalysisInvalidation invalidation;
  std::vector<std::string> invalidatedAnalyses;
  std::vector<OptimizationRemark> remarks;
};

struct OptimizationSafetyCounts {
  std::size_t sourceOperations = 0;
  std::size_t emittedOperations = 0;
  std::size_t staticallyDischarged = 0;
  std::array<std::size_t, static_cast<std::size_t>(MirSafetyCheckFamily::Count)>
      sourceByFamily{};
  std::array<std::size_t, static_cast<std::size_t>(MirSafetyCheckFamily::Count)>
      emittedByFamily{};
  std::array<std::size_t, static_cast<std::size_t>(MirSafetyCheckFamily::Count)>
      dischargedByFamily{};
};

struct OptimizationReport {
  bool verificationEnabled = false;
  MirVerificationResult inputVerification;
  // Complete output validity, including exact coherence with the retained
  // pre-optimization MIR snapshot.
  MirVerificationResult outputVerification;
  std::vector<OptimizationPassReport> passes;
  std::vector<MirTransformRecord> transformations;
  OptimizationSafetyCounts safety;

  [[nodiscard]] bool valid() const {
    return inputVerification.valid() && outputVerification.valid();
  }
};

inline constexpr std::uint32_t optimizationReportSchemaVersion = 3;

// Presentation is kept outside the optimizer and uses only verified report
// data. The JSON schema is versioned independently from MIR serialization.
[[nodiscard]] std::string
formatOptimizationRemarks(const OptimizationReport &report,
                          std::string_view optimizationLevel);
[[nodiscard]] std::string
serializeOptimizationReportJson(const OptimizationReport &report,
                                std::string_view optimizationLevel);

} // namespace lang
