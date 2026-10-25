#pragma once

#include "gti/llvm_backend.h"
#include "gti/lowered_program.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

enum class LlvmCoverageState {
  Pending,
  Implemented,
  VerifiedNonExecuting,
};

// Nonexecuting metadata is distinct from a source operation that must have
// disappeared upstream. The latter cannot silently become a runtime no-op.
enum class LlvmNonExecutingRule {
  None,
  VerifiedMetadata,
  MustBeAbsent,
};

struct LlvmCoverageEntry {
  std::string_view family;
  std::string_view name;
  unsigned milestone = 0;
  LlvmCoverageState state = LlvmCoverageState::Pending;
  LlvmNonExecutingRule nonExecuting = LlvmNonExecutingRule::None;
  std::string_view predicate;
  // An implemented executable row names the execution test and the exact
  // case or profile which owns its acceptance evidence. Pending rows leave
  // both fields empty: a declared obligation is not observed execution.
  std::string_view positiveTest;
  std::string_view positiveAssertion;
  // An explicit gap preserves an existing implementation state while making
  // the missing executable evidence visible to promotion-readiness tooling.
  std::string_view positiveEvidenceGap;
  std::string_view negativeTest = "llvm_coverage_exhaustiveness";
  std::string_view negativeAssertion = "vocabulary-and-shape-drift";

  [[nodiscard]] std::string key() const {
    return std::string(family) + "." + std::string(name);
  }
};

[[nodiscard]] std::span<const LlvmCoverageEntry> llvmCoverageLedger();

struct LlvmCoverageUse {
  const LlvmCoverageEntry *entry = nullptr;
  std::optional<MirBodyAddress> body;
  SourceSpan source;
  MirBlockId block = 0;
  MirInstructionId instruction = 0;
  std::size_t occurrences = 0;
  bool required = true;
  bool executable = false;
  // A pending vocabulary family may have a smaller implemented structural
  // shape. The owning inventory, never a caller option, establishes this fact.
  const LlvmCoverageEntry *implementedShape = nullptr;
};

// Sealed hosted roots admit an exact direct-dependency closure. Unknown
// dispatch, exports and declaration-backed adapters retain the full inventory.
enum class LlvmAdmissionClosure {
  AllExecutableBodies,
  SealedEntryDependencies,
};

class LlvmAdmissionPlan final {
public:
  [[nodiscard]] const LoweredProgram &program() const {
    return input.program();
  }
  [[nodiscard]] const LlvmBackendOptions &options() const { return policy; }
  [[nodiscard]] LlvmAdmissionClosure closure() const { return closure_; }
  [[nodiscard]] std::span<const LlvmCoverageUse> requirements() const {
    return uses;
  }
  [[nodiscard]] std::span<const MirBodyAddress> requiredBodies() const {
    return bodies;
  }
  [[nodiscard]] const LlvmCoverageUse *firstUnsupported() const;
  [[nodiscard]] bool admitted() const { return firstUnsupported() == nullptr; }
  [[nodiscard]] std::string limitation() const;

private:
  LlvmAdmissionPlan(VerifiedLoweredProgram program, LlvmBackendOptions options);

  // Retain exact immutable source/target/MIR identity, rather than a hash of
  // the deliberately nonexhaustive inspection print or borrowed frontend data.
  VerifiedLoweredProgram input;
  LlvmBackendOptions policy;
  std::vector<LlvmCoverageUse> uses;
  std::vector<MirBodyAddress> bodies;
  LlvmAdmissionClosure closure_ = LlvmAdmissionClosure::AllExecutableBodies;

  friend LlvmAdmissionPlan planLlvmAdmission(VerifiedLoweredProgram program,
                                             LlvmBackendOptions options);
};

[[nodiscard]] LlvmAdmissionPlan
planLlvmAdmission(VerifiedLoweredProgram program,
                  LlvmBackendOptions options = {});

} // namespace lang
