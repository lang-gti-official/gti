#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lang {

struct MirVerificationError {
  MirBodyKind bodyKind = MirBodyKind::Function;
  std::size_t owner = 0;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::string message;
};

struct MirVerificationResult {
  std::vector<MirVerificationError> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

[[nodiscard]] MirVerificationResult verifyMirBody(const MirBody &body,
                                                  std::size_t owner = 0);

// Identity-only projection containment using the same carrier/value forwarding
// as loan verification. This is not a liveness or access permission proof.
[[nodiscard]] bool mirCanonicalPlaceContains(const MirBody &body,
                                             MirPlaceId parent,
                                             MirPlaceId child);

enum class MirVerificationComponent : std::uint8_t {
  BodyLocal = 1U << 0U,
  ProgramContracts = 1U << 1U,
  EffectSummaries = 1U << 2U,
};

using MirVerificationComponents = std::uint8_t;

[[nodiscard]] constexpr MirVerificationComponents
mirVerificationComponent(MirVerificationComponent component) {
  return static_cast<MirVerificationComponents>(component);
}

inline constexpr MirVerificationComponents requiredMirVerificationComponents =
    mirVerificationComponent(MirVerificationComponent::BodyLocal) |
    mirVerificationComponent(MirVerificationComponent::ProgramContracts) |
    mirVerificationComponent(MirVerificationComponent::EffectSummaries);

// The public facade admits only the complete component set. The explicit mask
// exists for controlled verification tests and fails closed when a component
// is omitted or an unknown bit is supplied.
[[nodiscard]] MirVerificationResult verifyMirProgram(
    const MirProgram &program,
    MirVerificationComponents components = requiredMirVerificationComponents);
[[nodiscard]] MirVerificationResult
verifyMirEffectSummaries(const MirProgram &program);

// Verifies that `optimized` is exactly `source` plus rewrites admitted by the
// controlled MIR editor. Every transformation record is treated as an
// untrusted claim and replayed through the canonical rewrite authority.
[[nodiscard]] MirVerificationResult
verifyMirOptimizationCoherence(const MirProgram &source,
                               const MirProgram &optimized,
                               const std::vector<MirTransformRecord> &records);

} // namespace lang
