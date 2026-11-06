#pragma once

#include "gti/mir.h"
#include "gti/lifetime_types.h"
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

// Reconstruct declaration variance and verify every concrete storage recipe.
[[nodiscard]] MirVerificationResult
verifyMirNominalLifetimeRecipes(const MirProgram &);

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

// Universal declaration relationship, proved under every feasible formula case.
[[nodiscard]] bool mirDeclaredLifetimeRelationship(
    const MirProgram &, const LifetimeSignatureContract &, Lifetime source, Lifetime target);

// Checks a reference conversion using the exact body contract/constraint
// graph, retaining referent identity, access and every nested region relation.
[[nodiscard]] bool mirLifetimeReferenceConversion(
    const MirProgram &, const MirBody &, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected);

// A by-value type conversion under every feasible body contract case. This
// proves the exact structural shape and nested regions, not copy permission,
// source identity, initialization, movement or loan activity.
[[nodiscard]] bool mirLifetimeValueConversion(
    const MirProgram &, const MirBody &, std::size_t owner,
    const SemanticType &actual, const SemanticType &expected);

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

// Validate the callable's owner in its own universal scope, then match the
// exact separately-scoped layout. Region-free legacy records may omit it.
[[nodiscard]] std::optional<SemanticType> checkedMirCallableOwnerType(
    const MirProgram &, std::optional<HirClassInstanceId>, const SemanticType &,
    const LifetimeSignatureContract &);
// Apply a method's own binders to its exact owner type. The layout's binder
// universe is separate and cannot be compared directly with a caller type.
[[nodiscard]] std::optional<SemanticType> instantiateMirCallableOwnerType(
    const MirProgram &, const MirFunctionInstance &, const MirLifetimeApplication &);

// A view preserves the actual semantic region type while selecting one
// universal layout. Matching identifies substitutions; formation obligations
// still require the body/application proof before representation use.
struct MirClassLifetimeView {
  const MirClassInstance *instance = nullptr;
  std::vector<LifetimeBinding> bindings;
  [[nodiscard]] std::optional<SemanticType> instantiate(const SemanticType &type) const {
    return substituteTypeLifetimes(type, bindings);
  }
};
struct MirEnumLifetimeView {
  const MirIntegralEnum *integral = nullptr;
  const MirPayloadEnum *payload = nullptr;
  std::vector<LifetimeBinding> bindings;
  [[nodiscard]] std::optional<SemanticType> instantiate(const SemanticType &type) const {
    return substituteTypeLifetimes(type, bindings);
  }
  [[nodiscard]] std::optional<MirPayloadEnumVariant> instantiateVariant(std::size_t index) const {
    if (!payload || index >= payload->variants.size() ||
        payload->variants[index].variantIndex != index)
      return std::nullopt;
    auto variant = payload->variants[index];
    for (auto &type : variant.payloadTypes) {
      const auto actual = instantiate(type);
      if (!actual)
        return std::nullopt;
      type = *actual;
    }
    return variant;
  }
};
[[nodiscard]] std::optional<MirClassLifetimeView>
mirClassLifetimeView(const MirProgram &, const SemanticType &);
[[nodiscard]] std::optional<MirEnumLifetimeView>
mirEnumLifetimeView(const MirProgram &, const SemanticType &);

// An exact closure instance owns invocation and cleanup. Environment matching
// substitutes only its fixed slots; per-invocation quantified regions remain
// bound in the carrier type and cannot become environment choices.
struct MirLambdaLifetimeView {
  const MirLambdaInstance *instance = nullptr;
  std::vector<LifetimeBinding> bindings;
  [[nodiscard]] std::optional<SemanticType> instantiate(const SemanticType &type) const {
    return substituteTypeLifetimes(type, bindings);
  }
};
[[nodiscard]] std::optional<MirLambdaLifetimeView>
mirLambdaLifetimeView(const MirLambdaInstance &, const SemanticType &);
// Compatible physical capture inventory only; this never selects a callable
// body or cleanup target when several concrete bodies share one environment.
[[nodiscard]] std::optional<std::vector<SemanticType>>
mirLambdaCaptureTypes(const MirProgram &, const SemanticType &);

// The carrier capability and its exact type select the source-free authority.
[[nodiscard]] const CallableLifetimeSignature *
checkedMirCallableLifetimeSignature(const MirProgram &, const MirInstruction &);
// Conservative direct-reference suppliers derived from the trusted carrier
// signature, with invocation operand zero reserved for the pointer itself.
[[nodiscard]] std::optional<FunctionBorrowDependencySummary>
mirCallableLifetimeBorrowSummary(const MirProgram &, const MirInstruction &);
[[nodiscard]] bool mirCallableLifetimeTargetMatches(
    const MirProgram &, const CallableLifetimeSignature &,
    const SemanticType &returnType, std::span<const SemanticType> parameters,
    const LifetimeSignatureContract &);

} // namespace lang
