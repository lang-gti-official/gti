#pragma once

#include "gti/mir.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace lang {

// A verifier rejection that is a property of the source program, not an
// internal inconsistency: MIR is the single authority for body-local borrow
// validity, so these become user diagnostics.
enum class MirBorrowRejectionKind {
  None,
  // An access overlaps a live loan it is incompatible with.
  Conflict,
  // A returned or stored borrow outlives the storage it depends on.
  Escape,
  // A parent loan is used while a child reborrow of it is still live.
  ParentUse,
  // A returned borrow comes from a formal parameter its signature does not
  // name as a supplier of the result.
  Supplier,
};

struct MirBorrowRejection {
  MirBorrowRejectionKind kind = MirBorrowRejectionKind::None;
  // Conflict: whether the rejected access writes.
  bool write = false;
  // Conflict: the access of the live loan it overlaps.
  AccessMode loanAccess = AccessMode::ReadOnly;
  MirLoanId loan = 0;
  // Conflict: the rejected instruction's kind (a Borrow creates a loan).
  MirInstructionKind instruction = MirInstructionKind::Compute;

  friend bool operator==(const MirBorrowRejection &,
                         const MirBorrowRejection &) = default;
};

struct MirVerificationError {
  MirBodyKind bodyKind = MirBodyKind::Function;
  std::size_t owner = 0;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::string message;
  MirBorrowRejection borrow;
};

struct MirVerificationResult {
  std::vector<MirVerificationError> errors;

  [[nodiscard]] bool valid() const { return errors.empty(); }
};

[[nodiscard]] MirVerificationResult verifyMirBody(const MirBody &body,
                                                  std::size_t owner = 0);

enum class MirDebugReadability { Unknown, Unavailable, DefinitelyReadable };
enum class MirDebugAvailabilityReason {
  Unproven,
  Inactive,
  Readable,
  OutOfScope
};

struct MirDebugAvailabilityRequest {
  SourceLocalId local = 0;
  MirDebugPoint point;
};

struct MirDebugAvailabilityFact {
  MirDebugAvailabilityRequest request;
  MirPlaceId place = 0;
  MirDebugReadability readability = MirDebugReadability::Unknown;
  MirDebugAvailabilityReason reason = MirDebugAvailabilityReason::Unproven;
};

struct MirDebugAvailabilityBatch {
  MirVerificationResult verification;
  std::vector<MirDebugAvailabilityFact> requested;
  std::vector<MirSourceLocalEnd> requiredStops;
};

// Fresh canonical execution facts after joining each source local to its exact
// body-local binding/type/domain. Lexical ends are additional required cuts,
// never initialization evidence. This batch is not a construction certificate.
[[nodiscard]] MirDebugAvailabilityBatch queryMirDebugAvailability(
    const MirProgram &program, MirBodyAddress address,
    std::span<const MirDebugAvailabilityRequest> requests);

// The exact type of a canonical concrete parameter-pack element. Complete
// projections additionally prove their physical type, read-only carrier
// access, traits and initial availability; longer paths retain the element's
// type for the independent remaining projection checks.
[[nodiscard]] const SemanticType *
mirConcretePackElementType(const MirBody &body, const MirPlace &place);

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
