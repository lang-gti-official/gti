#pragma once

#include "gti/mir.h"
#include "gti/mir_verifier.h"

namespace lang {
[[nodiscard]] MirLifetimeApplication
lowerMirLifetimeApplication(const HirLifetimeApplication &);
// Mapping is provenance preservation, not an outlives/containment proof.
[[nodiscard]] bool resolveMirLifetimeOrigins(MirLifetimeApplication &,
                                             const MirBody &);
[[nodiscard]] bool
preservesHirLifetimeApplication(const HirLifetimeApplication &,
                                const MirLifetimeApplication &);
[[nodiscard]] MirLifetimeBody lowerMirLifetimeBody(const HirLifetimeBody &);
[[nodiscard]] bool resolveMirLifetimeOrigins(MirLifetimeBody &,
                                             const MirBody &);
[[nodiscard]] bool preservesHirLifetimeBody(const HirLifetimeBody &,
                                            const MirLifetimeBody &);

struct MirInstantiatedLifetimeSignature {
  SemanticType returnType = SemanticType::Unknown;
  std::vector<SemanticType> parameterTypes;
};

// Checks declaration scoping, exact universal coverage and preservation of
// every instantiated requirement. This is structural substitution only: the
// caller's body graph and actual origin relations must be proved separately.
[[nodiscard]] std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeSignature(const LifetimeSignatureContract &,
                                const SemanticType &returnType,
                                std::span<const SemanticType> parameterTypes,
                                const MirLifetimeApplication &);

// Independently checks the body-wide graph, exact type obligations and
// application-to-body preservation. Call/store/return reconstruction is a
// separate prerequisite, so this result alone never authorizes erasure.
[[nodiscard]] MirVerificationResult
verifyMirLifetimeBodyFacts(const MirProgram &, const MirBody &,
                           const LifetimeSignatureContract &,
                           std::size_t owner = 0);

// Reconstructs argument/receiver, storage and return requirements from exact
// MIR operands and target signatures, in addition to checking the body graph.
[[nodiscard]] MirVerificationResult
verifyMirLifetimeApplications(const MirProgram &, const MirBody &,
                              std::size_t owner = 0);

// Exact shape consumers may use these only alongside program lifetime proof.
[[nodiscard]] bool
mirLifetimeParameterTypesMatch(std::span<const SemanticType> selected,
                               std::span<const SemanticType> declared,
                               const LifetimeSignatureContract &,
                               const MirLifetimeApplication &);
[[nodiscard]] bool mirLifetimeCallSignatureMatches(const MirInstruction &,
                                                   const MirFunctionInstance &);
[[nodiscard]] bool mirLifetimeCallSignatureMatches(const MirInstruction &,
                                                   const MirLambdaInstance &);
[[nodiscard]] std::optional<MirInstantiatedLifetimeSignature>
instantiateMirLifetimeTarget(const MirProgram &, const MirInstruction &);

} // namespace lang
