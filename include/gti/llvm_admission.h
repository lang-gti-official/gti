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
  std::string_view positiveTest = "llvm_admission";
  std::string_view negativeTest = "llvm_coverage_exhaustiveness";

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
};

// Every executable body is conservatively required until a sealed-input root
// and dependency analysis proves an omission safe. This is not reachability.
enum class LlvmAdmissionClosure {
  AllExecutableBodies,
};

class LlvmAdmissionPlan final {
public:
  [[nodiscard]] const LoweredProgram &program() const {
    return input.program();
  }
  [[nodiscard]] const LlvmBackendOptions &options() const { return policy; }
  [[nodiscard]] LlvmAdmissionClosure closure() const {
    return LlvmAdmissionClosure::AllExecutableBodies;
  }
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

  friend LlvmAdmissionPlan planLlvmAdmission(VerifiedLoweredProgram program,
                                             LlvmBackendOptions options);
};

[[nodiscard]] LlvmAdmissionPlan
planLlvmAdmission(VerifiedLoweredProgram program,
                  LlvmBackendOptions options = {});

} // namespace lang
