#include "gti/lowered_program.h"
#include "gti/mir_verifier.h"

#include "gti/failure_metadata.h"
#include "gti/lowered_program_printer.h"
#include "lowered_program_closure.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

[[nodiscard]] bool lifecycleContainsClosure(const SemanticType &type) {
  return type.kind == SemanticType::Lambda ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     lifecycleContainsClosure);
}

using lowered_detail::addIssue;
using lowered_detail::bodyRole;
using lowered_detail::containsGenerated;
using lowered_detail::DerivedGeneratedGraph;
using lowered_detail::deriveGeneratedGraph;
using lowered_detail::findBody;
using lowered_detail::findDeclaration;
using lowered_detail::findGenerated;
using lowered_detail::fingerprint;
using lowered_detail::generatedLess;
using lowered_detail::ordinal;

[[nodiscard]] bool
validGeneratedIdentity(const LoweredGeneratedItemIdentity &identity) {
  if (ordinal(identity.kind) >= ordinal(LoweredGeneratedItemKind::Count)) {
    return false;
  }
  if (identity.kind == LoweredGeneratedItemKind::ProgramInitialization) {
    return identity.owner == 0 && identity.ordinal == 0;
  }
  if (identity.kind == LoweredGeneratedItemKind::HostedEntry ||
      identity.kind == LoweredGeneratedItemKind::StructuralOperatorAdapter ||
      identity.kind == LoweredGeneratedItemKind::CallableAdapter ||
      identity.kind == LoweredGeneratedItemKind::LifecycleCleanup ||
      identity.kind == LoweredGeneratedItemKind::NativeInteropAdapter ||
      identity.kind == LoweredGeneratedItemKind::ThreadTaskAdapter) {
    return identity.owner != 0 && identity.ordinal == 0;
  }
  return identity.owner != 0;
}

[[nodiscard]] bool resolvedType(const SemanticType &type) {
  return type != SemanticType::Unknown &&
         std::all_of(type.arguments.begin(), type.arguments.end(),
                     resolvedType);
}

[[nodiscard]] bool resolvedCompileTimeValue(const CompileTimeValue &value) {
  return value.kind == CompileTimeValue::UInt64 ||
         (value.kind == CompileTimeValue::Parameter && value.parameterId != 0);
}

[[nodiscard]] bool resolvedTypes(const std::vector<SemanticType> &types) {
  return std::all_of(types.begin(), types.end(), resolvedType);
}

[[nodiscard]] bool resolvedValues(const std::vector<CompileTimeValue> &values) {
  return std::all_of(values.begin(), values.end(), resolvedCompileTimeValue);
}

[[nodiscard]] bool symbolRequiresType(SymbolKind kind) {
  switch (kind) {
  case SymbolKind::Enumerator:
  case SymbolKind::Field:
  case SymbolKind::GlobalVariable:
  case SymbolKind::LocalVariable:
  case SymbolKind::Parameter:
  case SymbolKind::LambdaCapture:
    return true;
  case SymbolKind::Namespace:
  case SymbolKind::NamespaceAlias:
  case SymbolKind::Concept:
  case SymbolKind::TypeAlias:
  case SymbolKind::Class:
  case SymbolKind::Struct:
  case SymbolKind::Enum:
  case SymbolKind::Constructor:
  case SymbolKind::Destructor:
  case SymbolKind::Function:
  case SymbolKind::Method:
  case SymbolKind::Operator:
  case SymbolKind::TypeParameter:
  case SymbolKind::ValueParameter:
  case SymbolKind::RequirementParameter:
    return false;
  }
  return true;
}

[[nodiscard]] bool
validGenericParameters(const std::vector<LoweredGenericParameter> &parameters,
                       bool allowProjected = false) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const LoweredGenericParameter &parameter = parameters[index];
    if (parameter.id == 0 || parameter.name.empty() ||
        (!allowProjected && !parameter.typeArgumentPath.empty()) ||
        (parameter.value && !parameter.typeArgumentPath.empty()) ||
        std::find_if(parameters.begin(),
                     parameters.begin() + static_cast<std::ptrdiff_t>(index),
                     [&](const LoweredGenericParameter &prior) {
                       return prior.id == parameter.id;
                     }) !=
            parameters.begin() + static_cast<std::ptrdiff_t>(index)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
validClassPatternBindings(const LoweredClassDeclaration &declaration) {
  if (declaration.genericArgumentPatterns.empty()) {
    return std::all_of(declaration.genericParameters.begin(),
                       declaration.genericParameters.end(),
                       [](const auto &parameter) {
                         return parameter.typeArgumentPath.empty();
                       });
  }
  if (!resolvedTypes(declaration.genericArgumentPatterns) ||
      declaration.genericParameters.empty()) {
    return false;
  }
  for (const auto &parameter : declaration.genericParameters) {
    const auto projected = projectGenericTypeArgument(
        declaration.genericArgumentPatterns, parameter.typeArgumentPath,
        parameter.id, parameter.pack);
    if (!projected || parameter.value ||
        projected->genericParameterId != parameter.id ||
        projected->kind != (parameter.pack ? SemanticType::TypePack
                                           : SemanticType::TypeParameter)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool validRequirementExpression(
    const std::vector<LoweredConceptRequirement> &requirements,
    const std::vector<LoweredConstraintExpressionNode> &nodes, std::size_t root,
    bool hasClause) {
  if (!hasClause) {
    return requirements.empty() && nodes.empty() &&
           root == ConstraintExpressionNode::invalidIndex;
  }
  if (requirements.empty() || nodes.empty() || root >= nodes.size()) {
    return false;
  }
  std::vector<bool> referenced(requirements.size(), false);
  std::vector<bool> reachable(nodes.size(), false);
  std::function<bool(std::size_t)> visit = [&](std::size_t index) {
    if (index >= nodes.size() || reachable[index]) {
      return false;
    }
    reachable[index] = true;
    const LoweredConstraintExpressionNode &node = nodes[index];
    switch (node.kind) {
    case ConstraintExpressionKind::Application:
      if (node.application >= requirements.size() ||
          node.left != ConstraintExpressionNode::invalidIndex ||
          node.right != ConstraintExpressionNode::invalidIndex) {
        return false;
      }
      referenced[node.application] = true;
      return true;
    case ConstraintExpressionKind::Conjunction:
    case ConstraintExpressionKind::Disjunction:
      return node.application == ConstraintExpressionNode::invalidIndex &&
             node.left < index && node.right < index && visit(node.left) &&
             visit(node.right);
    }
    return false;
  };
  return visit(root) &&
         std::all_of(reachable.begin(), reachable.end(),
                     [](bool value) { return value; }) &&
         std::all_of(referenced.begin(), referenced.end(),
                     [](bool value) { return value; });
}

[[nodiscard]] std::optional<std::string>
parameterMetadataError(const std::vector<LoweredParameter> &parameters) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const LoweredParameter &parameter = parameters[index];
    const std::string prefix = "parameter " + std::to_string(index + 1) + ' ';
    if (!resolvedType(parameter.type) || parameter.type == SemanticType::Void) {
      return prefix + "has an unresolved or void type";
    }
    if (parameter.symbol != 0 &&
        std::find_if(parameters.begin(),
                     parameters.begin() + static_cast<std::ptrdiff_t>(index),
                     [&](const LoweredParameter &prior) {
                       return prior.symbol == parameter.symbol;
                     }) !=
            parameters.begin() + static_cast<std::ptrdiff_t>(index)) {
      return prefix + "duplicates a prior symbol";
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool
validParameters(const std::vector<LoweredParameter> &parameters) {
  return !parameterMetadataError(parameters).has_value();
}

[[nodiscard]] bool validLayout(const MirCAbiRecordLayout &layout) {
  return std::all_of(layout.fields.begin(), layout.fields.end(),
                     [](const MirCAbiRecordFieldLayout &field) {
                       return field.field != 0 && resolvedType(field.type) &&
                              field.type != SemanticType::Void;
                     });
}

[[nodiscard]] bool validLayout(const MirUnionLayout &layout) {
  return std::all_of(layout.fields.begin(), layout.fields.end(),
                     [](const MirUnionFieldLayout &field) {
                       return field.field != 0 && resolvedType(field.type) &&
                              field.type != SemanticType::Void;
                     });
}

[[nodiscard]] std::optional<std::string>
callableParameterError(const std::vector<LoweredCallableParameter> &contracts,
                       std::size_t parameterCount) {
  for (std::size_t index = 0; index < contracts.size(); ++index) {
    const LoweredCallableParameter &contract = contracts[index];
    const std::string prefix =
        "callable contract " + std::to_string(index + 1) + ' ';
    if (contract.parameterIndex >= parameterCount) {
      return prefix + "refers outside the function parameter list";
    }
    if (!resolvedType(contract.callableType)) {
      return prefix + "has an unresolved callable type";
    }
    if (std::find_if(contracts.begin(),
                     contracts.begin() + static_cast<std::ptrdiff_t>(index),
                     [&](const LoweredCallableParameter &prior) {
                       return prior.parameterIndex == contract.parameterIndex;
                     }) !=
        contracts.begin() + static_cast<std::ptrdiff_t>(index)) {
      return prefix + "duplicates a prior parameter contract";
    }
    if (contract.ownedTransport &&
        (!resolvedType(contract.ownedTransport->destinationType) ||
         (contract.ownedTransport->kind ==
              CallableOwnedTransportKind::ExactField &&
          contract.ownedTransport->field == 0))) {
      return prefix + "has an invalid owned-transport destination";
    }
    for (std::size_t signatureIndex = 0;
         signatureIndex < contract.signatures.size(); ++signatureIndex) {
      const LoweredCallableSignature &signature =
          contract.signatures[signatureIndex];
      if ((signature.returnType != SemanticType::Unknown &&
           !resolvedType(signature.returnType)) ||
          !std::all_of(
              signature.parameterTypes.begin(), signature.parameterTypes.end(),
              [](const SemanticType &type) {
                return type == SemanticType::Unknown || resolvedType(type);
              })) {
        return prefix + "signature " + std::to_string(signatureIndex + 1) +
               " has an unresolved type";
      }
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string>
declarationPayloadError(const LoweredDeclaration &declaration) {
  const auto invalid = [](std::string detail) {
    return std::optional<std::string>{std::move(detail)};
  };
  const auto validNamePath = [](const std::vector<std::string> &path) {
    return !path.empty() && std::all_of(path.begin(), path.end(),
                                        [](const std::string &segment) {
                                          return !segment.empty();
                                        });
  };

  switch (declaration.kind) {
  case LoweredDeclarationKind::Namespace:
    if (!std::holds_alternative<std::monostate>(declaration.payload) ||
        declaration.name.empty()) {
      return invalid("namespace declaration has an invalid payload");
    }
    break;
  case LoweredDeclarationKind::NamespaceAlias: {
    const auto *payload =
        std::get_if<LoweredNamespaceAliasDeclaration>(&declaration.payload);
    if (payload == nullptr || declaration.name.empty() ||
        !validNamePath(payload->target)) {
      return invalid("namespace alias lacks a resolved target path");
    }
    break;
  }
  case LoweredDeclarationKind::TypeAlias: {
    const auto *payload =
        std::get_if<LoweredTypeAliasDeclaration>(&declaration.payload);
    if (payload == nullptr || declaration.name.empty() ||
        payload->qualifiedName.empty() || !resolvedType(payload->type)) {
      return invalid("type alias lacks a resolved type identity");
    }
    break;
  }
  case LoweredDeclarationKind::Class: {
    const auto *payload =
        std::get_if<LoweredClassDeclaration>(&declaration.payload);
    const bool exactSpecialization =
        payload != nullptr && payload->exactSpecializationPrimary != 0;
    if (payload == nullptr || payload->id == 0 ||
        declaration.semanticIdentity != payload->id ||
        declaration.name.empty() || payload->qualifiedName.empty() ||
        declaration.generic != !payload->genericParameters.empty() ||
        !validGenericParameters(payload->genericParameters, true) ||
        !validClassPatternBindings(*payload) ||
        (exactSpecialization &&
         (!payload->genericParameters.empty() ||
          (payload->exactTypeArguments.empty() &&
           payload->exactValueArguments.empty()) ||
          payload->forwardDeclaration || payload->cAbiRecord ||
          payload->cOpaqueHandle)) ||
        (!exactSpecialization && (!payload->exactTypeArguments.empty() ||
                                  !payload->exactValueArguments.empty())) ||
        !std::all_of(payload->exactTypeArguments.begin(),
                     payload->exactTypeArguments.end(), resolvedType) ||
        std::any_of(payload->exactValueArguments.begin(),
                    payload->exactValueArguments.end(),
                    [](const CompileTimeValue &value) {
                      return value.kind != CompileTimeValue::UInt64;
                    }) ||
        !std::all_of(payload->bases.begin(), payload->bases.end(),
                     [](const LoweredClassBase &base) {
                       return resolvedType(base.type) &&
                              base.type != SemanticType::Void;
                     }) ||
        (payload->cAbiLayout && !validLayout(*payload->cAbiLayout)) ||
        (payload->unionLayout && !validLayout(*payload->unionLayout)) ||
        (payload->cAbiLayout && !payload->cAbiRecord) ||
        (payload->unionLayout && payload->kind != ClassKind::Union)) {
      return invalid("class declaration payload is unresolved or incoherent");
    }
    break;
  }
  case LoweredDeclarationKind::Enum: {
    const auto *payload =
        std::get_if<LoweredEnumDeclaration>(&declaration.payload);
    if (payload == nullptr || payload->id == 0 ||
        declaration.semanticIdentity != payload->id ||
        declaration.name.empty() || payload->qualifiedName.empty() ||
        !resolvedType(payload->underlyingType) ||
        payload->underlyingType == SemanticType::Void ||
        std::any_of(
            payload->enumerators.begin(), payload->enumerators.end(),
            [&](const LoweredEnumerator &enumerator) {
              return enumerator.name.empty() ||
                     (!payload->payload && !enumerator.payloadTypes.empty()) ||
                     (payload->payload &&
                      enumerator.payloadTypes.size() !=
                          enumerator.payloadRequiresActiveCleanup.size()) ||
                     !std::all_of(enumerator.payloadTypes.begin(),
                                  enumerator.payloadTypes.end(), resolvedType);
            }) ||
        (payload->requiresActiveCleanup &&
         payload->traits.drop != DropKind::Lexical)) {
      return invalid("enum declaration payload is unresolved or incoherent");
    }
    break;
  }
  case LoweredDeclarationKind::Function: {
    const auto *payload =
        std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
    if (payload == nullptr) {
      return invalid("function declaration lacks its resolved payload");
    }
    if (payload->id == 0 || declaration.semanticIdentity != payload->id) {
      return invalid("function declaration has an invalid semantic identity");
    }
    if (declaration.name.empty() || payload->qualifiedName.empty()) {
      return invalid("function declaration has an empty resolved name");
    }
    if (!resolvedType(payload->returnType)) {
      return invalid("function declaration has an unresolved return type");
    }
    if (const std::optional<std::string> error =
            parameterMetadataError(payload->parameters)) {
      return invalid("function declaration " + *error);
    }
    if (declaration.generic != !payload->genericParameters.empty() ||
        !validGenericParameters(payload->genericParameters)) {
      return invalid("function declaration has invalid generic metadata");
    }
    if (payload->requiredParameterCount > payload->parameters.size() ||
        (payload->cArrayCountParameter &&
         *payload->cArrayCountParameter >= payload->parameters.size())) {
      return invalid("function declaration has invalid parameter bounds");
    }
    if (const std::optional<std::string> error = callableParameterError(
            payload->callableParameters, payload->parameters.size())) {
      return invalid("function declaration " + *error);
    }
    if (std::any_of(payload->requirements.begin(), payload->requirements.end(),
                    [](const LoweredConceptRequirement &requirement) {
                      return (requirement.memberName.empty()
                                  ? requirement.conceptId == 0 ||
                                        requirement.mutableReceiver ||
                                        requirement.requiresUnsafe
                                  : requirement.conceptId != 0 ||
                                        requirement.arguments.size() < 2) ||
                             !std::all_of(requirement.arguments.begin(),
                                          requirement.arguments.end(),
                                          resolvedType);
                    }) ||
        !validRequirementExpression(
            payload->requirements, payload->requirementExpression,
            payload->requirementRoot, payload->hasRequiresClause)) {
      return invalid("function declaration has invalid concept requirements");
    }
    if (std::any_of(payload->virtualRoots.begin(), payload->virtualRoots.end(),
                    [](FunctionId root) { return root == 0; })) {
      return invalid("function declaration has an invalid virtual root");
    }
    if ((payload->returnBorrowOrigin == BorrowOriginKind::Argument &&
         payload->returnBorrowParameter >= payload->parameters.size()) ||
        (payload->returnBorrowOrigin == BorrowOriginKind::Global &&
         (!payload->returnBorrowPlace ||
          !payload->returnBorrowPlace->valid()))) {
      return invalid(
          "function declaration has an invalid return-borrow origin");
    }
    if (payload->ownerClass != declaration.ownerClass) {
      return invalid("function declaration has an inconsistent class owner");
    }
    break;
  }
  case LoweredDeclarationKind::Constructor: {
    const auto *payload =
        std::get_if<LoweredConstructorDeclaration>(&declaration.payload);
    if (payload == nullptr) {
      return invalid("constructor declaration lacks its resolved payload");
    }
    if (payload->id == 0 || payload->owner == 0 ||
        declaration.semanticIdentity != payload->id) {
      return invalid(
          "constructor declaration has an invalid semantic identity");
    }
    if (declaration.ownerClass != payload->owner) {
      return invalid("constructor declaration has an inconsistent class owner");
    }
    if (declaration.name.empty()) {
      return invalid("constructor declaration has an empty name");
    }
    if (declaration.generic != !payload->genericParameters.empty() ||
        !validGenericParameters(payload->genericParameters)) {
      return invalid("constructor declaration has invalid generic metadata");
    }
    if (std::any_of(payload->requirements.begin(), payload->requirements.end(),
                    [](const LoweredConceptRequirement &requirement) {
                      return (requirement.memberName.empty()
                                  ? requirement.conceptId == 0 ||
                                        requirement.mutableReceiver ||
                                        requirement.requiresUnsafe
                                  : requirement.conceptId != 0 ||
                                        requirement.arguments.size() < 2) ||
                             !std::all_of(requirement.arguments.begin(),
                                          requirement.arguments.end(),
                                          resolvedType);
                    }) ||
        !validRequirementExpression(
            payload->requirements, payload->requirementExpression,
            payload->requirementRoot, payload->hasRequiresClause)) {
      return invalid(
          "constructor declaration has invalid concept requirements");
    }
    if (const std::optional<std::string> error =
            parameterMetadataError(payload->parameters)) {
      return invalid("constructor declaration " + *error);
    }
    if (payload->requiredParameterCount > payload->parameters.size() ||
        (payload->borrowParameter &&
         *payload->borrowParameter >= payload->parameters.size())) {
      return invalid("constructor declaration has invalid parameter bounds");
    }
    break;
  }
  case LoweredDeclarationKind::Destructor: {
    const auto *payload =
        std::get_if<LoweredDestructorDeclaration>(&declaration.payload);
    if (payload == nullptr || payload->owner == 0 ||
        declaration.semanticIdentity != payload->owner ||
        declaration.ownerClass != payload->owner || declaration.name.empty()) {
      return invalid(
          "destructor declaration payload is unresolved or incoherent");
    }
    break;
  }
  case LoweredDeclarationKind::Storage: {
    const auto *payload =
        std::get_if<LoweredStorageDeclaration>(&declaration.payload);
    if (payload == nullptr || payload->symbol == 0 ||
        declaration.semanticIdentity != payload->symbol ||
        declaration.ownerClass != payload->ownerClass ||
        declaration.name.empty() || !resolvedType(payload->type) ||
        payload->type == SemanticType::Void) {
      return invalid("storage declaration payload is unresolved or incoherent");
    }
    break;
  }
  case LoweredDeclarationKind::Access:
    if (!std::holds_alternative<LoweredAccessDeclaration>(
            declaration.payload) ||
        declaration.ownerClass == 0) {
      return invalid("access declaration is not attached to a class");
    }
    break;
  case LoweredDeclarationKind::LanguageLinkage:
    if (!std::holds_alternative<LoweredLanguageLinkageDeclaration>(
            declaration.payload)) {
      return invalid("language-linkage declaration has an invalid payload");
    }
    break;
  case LoweredDeclarationKind::Concept:
  case LoweredDeclarationKind::Empty:
  case LoweredDeclarationKind::Other:
    if (!std::holds_alternative<std::monostate>(declaration.payload)) {
      return invalid("compile-time declaration unexpectedly has a payload");
    }
    break;
  case LoweredDeclarationKind::Count:
    return invalid("declaration uses the sentinel kind");
  }
  return std::nullopt;
}

} // namespace
std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program) {
  std::vector<LoweredProgramIssue> issues;
  if (!program.target_.supported()) {
    addIssue(issues, LoweredProgramIssueKind::UnsupportedTarget,
             "lowered program has an unsupported target data layout");
  }
  if (program.target_ != program.mir_.target()) {
    addIssue(issues, LoweredProgramIssueKind::FrontendMismatch,
             "lowered target and MIR target ABI facts differ");
  }
  if (program.target_.executionProfile != program.mir_.executionProfile()) {
    addIssue(issues, LoweredProgramIssueKind::FrontendMismatch,
             "lowered target and MIR execution profiles differ");
  }
  if (program.programKind_ != program.mir_.programKind() ||
      program.embeddingPlan_.has_value() !=
          program.mir_.embeddingPlan().has_value()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidEmbeddingPlan,
             "lowered program/export policy differs from verified MIR");
  } else if (program.embeddingPlan_ && program.mir_.embeddingPlan()) {
    const LoweredEmbeddingPlan &lowered = *program.embeddingPlan_;
    const MirEmbeddingPlan &mir = *program.mir_.embeddingPlan();
    if (program.programKind_ != ProgramKind::Library ||
        lowered.abiVersion != embeddingAbiVersionV1 ||
        lowered.abiVersion != mir.abiVersion || lowered.exports.empty() ||
        lowered.exports.size() != mir.exports.size()) {
      addIssue(issues, LoweredProgramIssueKind::InvalidEmbeddingPlan,
               "lowered embedding plan has invalid ABI provenance");
    }
    for (std::size_t index = 0;
         index < lowered.exports.size() && index < mir.exports.size();
         ++index) {
      const LoweredEmbeddingExport &selected = lowered.exports[index];
      const MirEmbeddingExport &mirSelected = mir.exports[index];
      const LoweredFunctionInstance *instance =
          program.findFunctionInstance(selected.function);
      const LoweredFunctionDeclaration *declaration =
          program.findFunctionDeclaration(selected.declaration);
      const bool exactParameters =
          declaration != nullptr &&
          declaration->parameters.size() == selected.parameterTypes.size() &&
          std::equal(
              declaration->parameters.begin(), declaration->parameters.end(),
              selected.parameterTypes.begin(),
              [](const LoweredParameter &parameter, const SemanticType &type) {
                return parameter.type == type;
              });
      if (selected.declaration != mirSelected.declaration ||
          selected.function != mirSelected.function ||
          selected.qualifiedName != mirSelected.qualifiedName ||
          selected.symbol != mirSelected.symbol ||
          selected.returnType != mirSelected.returnType ||
          selected.parameterTypes != mirSelected.parameterTypes ||
          selected.requestSource != mirSelected.requestSource ||
          instance == nullptr || declaration == nullptr ||
          instance->declaration != selected.declaration || instance->owner ||
          !instance->typeArguments.empty() ||
          !instance->valueArguments.empty() ||
          declaration->qualifiedName != selected.qualifiedName ||
          declaration->returnType != selected.returnType ||
          declaration->ownerClass != 0 ||
          declaration->definitionKind != MirDefinitionKind::Source ||
          declaration->linkage != LanguageLinkage::Gti || !exactParameters) {
        addIssue(issues, LoweredProgramIssueKind::InvalidEmbeddingPlan,
                 "lowered embedding export differs from its MIR/declaration "
                 "root");
      }
    }
  }
  const MirVerificationResult mirVerification = verifyMirProgram(program.mir_);
  if (!program.mir_.valid() || !mirVerification.valid()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidOptimizedMir,
             mirVerification.errors.empty()
                 ? "lowered MIR is not marked valid"
                 : mirVerification.errors.front().message);
  }
  const FailureMetadataVerificationResult failureVerification =
      verifyFailureMetadata(program.mir_.failureMetadata());
  if (!failureVerification.valid()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidFailureMetadata,
             failureVerification.errors.empty()
                 ? "lowered MIR failure metadata is invalid"
                 : failureVerification.errors.front());
  }

  const std::vector<MirBodyAddress> addresses =
      enumerateMirBodyAddresses(program.mir_);
  if (program.bodies_.size() != addresses.size()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidBodyInventory,
             "lowered body census does not match the MIR body census");
  }
  for (std::size_t index = 0;
       index < std::min(program.bodies_.size(), addresses.size()); ++index) {
    const LoweredBody &body = program.bodies_[index];
    const MirBody *mirBody = findMirBody(program.mir_, addresses[index]);
    if (body.identity.address != addresses[index] || mirBody == nullptr ||
        body.identity.placeDomain != mirBody->placeDomain ||
        body.role != bodyRole(program.mir_, body.identity) ||
        ordinal(body.identity.definition) >=
            ordinal(LoweredBodyDefinitionKind::Count) ||
        ordinal(body.role) >= ordinal(LoweredBodyRole::Count)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidBodyInventory,
               "lowered body identity or role differs from verified MIR",
               body.identity.address);
    }
    if (!std::is_sorted(body.requiredGeneratedItems.begin(),
                        body.requiredGeneratedItems.end(), generatedLess) ||
        std::adjacent_find(body.requiredGeneratedItems.begin(),
                           body.requiredGeneratedItems.end()) !=
            body.requiredGeneratedItems.end() ||
        (body.role != LoweredBodyRole::SourceExecutable &&
         !body.requiredGeneratedItems.empty())) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "body generated-item roots are unordered, duplicated, or "
               "attached to a non-executable body",
               body.identity.address);
    }
    std::set<std::pair<MirBlockId, MirInstructionId>> debugStages;
    for (const LoweredSourceLocation &location : body.sourceLocations) {
      const MirBlock *block =
          mirBody == nullptr ? nullptr : mirBody->findBlock(location.block);
      const bool instructionExists =
          block != nullptr &&
          (location.instruction == 0 ||
           std::any_of(block->instructions.begin(), block->instructions.end(),
                       [&](const MirInstruction &instruction) {
                         return instruction.id == location.instruction &&
                                !instruction.tombstone;
                       }));
      if (!instructionExists || location.source.source.empty() ||
          location.source.start >= location.source.end ||
          location.source.line < 1 ||
          !debugStages.emplace(location.block, location.instruction).second) {
        addIssue(issues, LoweredProgramIssueKind::InvalidBodyInventory,
                 "source location has an invalid or duplicated MIR stage/span",
                 body.identity.address);
      }
    }
  }

  std::unordered_map<LoweredDeclarationId, std::size_t> nextOrdinal;
  std::unordered_map<ClassId, LoweredDeclarationId> classDeclarations;
  std::unordered_map<EnumId, LoweredDeclarationId> enumDeclarations;
  std::unordered_map<FunctionId, LoweredDeclarationId> functionDeclarations;
  std::unordered_map<ConstructorId, LoweredDeclarationId>
      constructorDeclarations;
  std::unordered_map<ClassId, LoweredDeclarationId> destructorDeclarations;
  std::unordered_map<SymbolId, LoweredDeclarationId> storageDeclarations;
  for (std::size_t index = 0; index < program.declarations_.size(); ++index) {
    const LoweredDeclaration &declaration = program.declarations_[index];
    const std::size_t expectedOrdinal = ++nextOrdinal[declaration.parent];
    if (declaration.id != index + 1 ||
        (declaration.parent != 0 && declaration.parent >= declaration.id) ||
        declaration.ordinal != expectedOrdinal ||
        ordinal(declaration.kind) >= ordinal(LoweredDeclarationKind::Count)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "lowered declaration identities are not dense, ordered, and "
               "parent-before-child",
               std::nullopt, declaration.id);
    }
    if (const std::optional<std::string> payloadError =
            declarationPayloadError(declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               *payloadError, std::nullopt, declaration.id);
    }

    const auto recordIdentity = [&](auto &identities, std::size_t identity,
                                    std::string_view kind) {
      if (identity != 0 &&
          !identities.emplace(identity, declaration.id).second) {
        addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
                 "duplicate " + std::string(kind) + " declaration identity " +
                     std::to_string(identity),
                 std::nullopt, declaration.id);
      }
    };
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      recordIdentity(classDeclarations, payload->id, "class");
    } else if (const auto *payload =
                   std::get_if<LoweredEnumDeclaration>(&declaration.payload)) {
      recordIdentity(enumDeclarations, payload->id, "enum");
      const MirPayloadEnum *mirEnumeration =
          program.mir_.findPayloadEnum(payload->id);
      if (payload->payload) {
        bool exact =
            mirEnumeration != nullptr &&
            payload->traits == mirEnumeration->traits &&
            payload->requiresActiveCleanup ==
                mirEnumeration->requiresActiveCleanup &&
            payload->enumerators.size() == mirEnumeration->variants.size();
        for (std::size_t variantIndex = 0;
             exact && variantIndex < payload->enumerators.size();
             ++variantIndex) {
          const LoweredEnumerator &lowered = payload->enumerators[variantIndex];
          const MirPayloadEnumVariant &mirVariant =
              mirEnumeration->variants[variantIndex];
          exact = lowered.variantIndex == mirVariant.variantIndex &&
                  lowered.payloadTypes == mirVariant.payloadTypes &&
                  lowered.payloadRequiresActiveCleanup ==
                      mirVariant.payloadRequiresActiveCleanup;
        }
        if (!exact) {
          addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
                   "payload enum declaration differs from the exact MIR "
                   "lifecycle table",
                   std::nullopt, declaration.id);
        }
      } else if (mirEnumeration != nullptr) {
        addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
                 "non-payload enum has a MIR payload lifecycle table",
                 std::nullopt, declaration.id);
      }
    } else if (const auto *payload = std::get_if<LoweredFunctionDeclaration>(
                   &declaration.payload)) {
      recordIdentity(functionDeclarations, payload->id, "function");
    } else if (const auto *payload = std::get_if<LoweredConstructorDeclaration>(
                   &declaration.payload)) {
      recordIdentity(constructorDeclarations, payload->id, "constructor");
    } else if (const auto *payload = std::get_if<LoweredDestructorDeclaration>(
                   &declaration.payload)) {
      recordIdentity(destructorDeclarations, payload->owner, "destructor");
    } else if (const auto *payload = std::get_if<LoweredStorageDeclaration>(
                   &declaration.payload)) {
      recordIdentity(storageDeclarations, payload->symbol, "storage");
    }
  }

  for (const LoweredDeclaration &declaration : program.declarations_) {
    const auto *specialization =
        std::get_if<LoweredClassDeclaration>(&declaration.payload);
    if (specialization == nullptr ||
        specialization->exactSpecializationPrimary == 0) {
      continue;
    }
    const LoweredClassDeclaration *primary = program.findClassDeclaration(
        specialization->exactSpecializationPrimary);
    const auto primaryIdentity =
        classDeclarations.find(specialization->exactSpecializationPrimary);
    const LoweredDeclaration *primaryRow =
        primaryIdentity == classDeclarations.end()
            ? nullptr
            : program.findDeclaration(primaryIdentity->second);
    const std::size_t primaryTypeParameters =
        primary == nullptr ? 0
        : !primary->genericArgumentPatterns.empty()
            ? primary->genericArgumentPatterns.size()
            : static_cast<std::size_t>(
                  std::count_if(primary->genericParameters.begin(),
                                primary->genericParameters.end(),
                                [](const LoweredGenericParameter &parameter) {
                                  return !parameter.value;
                                }));
    const std::size_t primaryValueParameters =
        primary == nullptr ? 0
                           : static_cast<std::size_t>(std::count_if(
                                 primary->genericParameters.begin(),
                                 primary->genericParameters.end(),
                                 [](const LoweredGenericParameter &parameter) {
                                   return parameter.value;
                                 }));
    if (primary == nullptr || primaryRow == nullptr ||
        primary->exactSpecializationPrimary != 0 ||
        primary->kind != specialization->kind ||
        primary->qualifiedName != specialization->qualifiedName ||
        primaryRow->name != declaration.name ||
        primaryRow->namespaceScope != declaration.namespaceScope ||
        primaryTypeParameters != specialization->exactTypeArguments.size() ||
        primaryValueParameters != specialization->exactValueArguments.size()) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "exact class specialization does not match one lowered "
               "generic primary",
               std::nullopt, declaration.id);
    }
  }

  for (const MirClassInstance &instance : program.mir_.classInstances()) {
    if (!classDeclarations.contains(instance.declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "MIR class instance refers to an absent lowered declaration");
    }
  }
  for (const MirFunctionInstance &instance : program.mir_.functionInstances()) {
    if (!functionDeclarations.contains(instance.declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "MIR function instance refers to an absent lowered declaration");
    }
  }
  for (const LoweredBody &body : program.bodies_) {
    if (body.identity.address.kind == MirBodyKind::Constructor &&
        body.identity.declaration != 0 &&
        !constructorDeclarations.contains(body.identity.declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidDeclarationInventory,
               "MIR constructor body refers to an absent lowered declaration",
               body.identity.address);
    }
  }

  std::unordered_map<SymbolId, std::size_t> symbolIdentities;
  for (std::size_t index = 0; index < program.symbols_.size(); ++index) {
    const LoweredSymbol &symbol = program.symbols_[index];
    const bool unresolvedType =
        symbolRequiresType(symbol.kind) && !resolvedType(symbol.type) &&
        !(symbol.kind == SymbolKind::LocalVariable &&
          symbol.instanceDependentType && symbol.type == SemanticType::Unknown);
    const bool invalidInstanceDependentType =
        symbol.instanceDependentType &&
        (symbol.kind != SymbolKind::LocalVariable ||
         symbol.type != SemanticType::Unknown);
    if (symbol.id == 0 || symbol.name.empty() || unresolvedType ||
        invalidInstanceDependentType ||
        !symbolIdentities.emplace(symbol.id, index).second) {
      addIssue(issues, LoweredProgramIssueKind::InvalidSymbolInventory,
               "lowered symbol has an absent/duplicate identity, empty name, "
               "invalid instance-dependent marker, or unresolved required "
               "type");
    }
  }
  const auto requireSymbol = [&](SymbolId symbol,
                                 LoweredDeclarationId declaration) {
    if (symbol != 0 && !symbolIdentities.contains(symbol)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidSymbolInventory,
               "lowered declaration refers to an absent symbol", std::nullopt,
               declaration);
    }
  };
  for (const LoweredDeclaration &declaration : program.declarations_) {
    if (const auto *storage =
            std::get_if<LoweredStorageDeclaration>(&declaration.payload)) {
      requireSymbol(storage->symbol, declaration.id);
    } else if (const auto *function = std::get_if<LoweredFunctionDeclaration>(
                   &declaration.payload)) {
      for (const LoweredParameter &parameter : function->parameters) {
        requireSymbol(parameter.symbol, declaration.id);
      }
    } else if (const auto *constructor =
                   std::get_if<LoweredConstructorDeclaration>(
                       &declaration.payload)) {
      for (const LoweredParameter &parameter : constructor->parameters) {
        requireSymbol(parameter.symbol, declaration.id);
      }
    }
  }
  for (const MirBodyAddress address : addresses) {
    const MirBody *body = findMirBody(program.mir_, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirPlace &place : body->places) {
      if (place.symbol != 0 && !symbolIdentities.contains(place.symbol)) {
        addIssue(issues, LoweredProgramIssueKind::InvalidSymbolInventory,
                 "MIR place refers to an absent lowered symbol", address);
      }
    }
  }

  if (program.classInstances_.size() != program.mir_.classInstances().size() ||
      program.functionInstances_.size() !=
          program.mir_.functionInstances().size() ||
      program.constructorInstances_.size() !=
          program.mir_.constructorInstances().size() ||
      program.destructorInstances_.size() !=
          program.mir_.destructorInstances().size() ||
      program.lambdaInstances_.size() !=
          program.mir_.lambdaInstances().size()) {
    addIssue(issues, LoweredProgramIssueKind::InvalidInstanceInventory,
             "lowered instance tables do not match the MIR instance census");
  }

  for (std::size_t index = 0;
       index < std::min(program.classInstances_.size(),
                        program.mir_.classInstances().size());
       ++index) {
    const LoweredClassInstance &instance = program.classInstances_[index];
    const MirClassInstance &mir = program.mir_.classInstances()[index];
    if (const auto *declaration =
            program.findClassDeclaration(instance.declaration)) {
      for (const LoweredGenericParameter &parameter :
           declaration->genericParameters) {
        if (!parameter.typeArgumentPath.empty() &&
            !projectGenericTypeArgument(instance.typeArguments,
                                        parameter.typeArgumentPath,
                                        parameter.id, parameter.pack)) {
          addIssue(
              issues, LoweredProgramIssueKind::InvalidInstanceInventory,
              "lowered class binding projection does not select an argument");
        }
      }
    }
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.declaration != mir.declaration ||
        instance.typeArguments != mir.type.arguments ||
        instance.valueArguments != mir.type.valueArguments ||
        !resolvedTypes(instance.typeArguments) ||
        !resolvedValues(instance.valueArguments) ||
        !classDeclarations.contains(instance.declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidInstanceInventory,
               "lowered class instance is unresolved or differs from MIR");
    }
  }
  for (std::size_t index = 0;
       index < std::min(program.functionInstances_.size(),
                        program.mir_.functionInstances().size());
       ++index) {
    const LoweredFunctionInstance &instance = program.functionInstances_[index];
    const MirFunctionInstance &mir = program.mir_.functionInstances()[index];
    const auto identity = functionDeclarations.find(mir.declaration);
    const auto *row = identity == functionDeclarations.end()
                          ? nullptr
                          : program.findDeclaration(identity->second);
    const auto *declaration =
        row == nullptr ? nullptr
                       : std::get_if<LoweredFunctionDeclaration>(&row->payload);
    const bool ownerValid =
        !instance.owner || (*instance.owner != 0 &&
                            *instance.owner <= program.classInstances_.size());
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.declaration != mir.declaration || declaration == nullptr ||
        declaration->requiresUnsafe != mir.requiresUnsafe ||
        declaration->neverReturns != mir.neverReturns ||
        declaration->tracksCaller != mir.tracksCaller ||
        declaration->cThreadOperation != mir.cThreadOperation ||
        (declaration->cThreadOperation != CThreadRuntimeOperation::None &&
         !declaration->compilerPrivate) ||
        instance.owner != mir.owner ||
        instance.typeArguments != mir.typeArguments ||
        instance.valueArguments != mir.valueArguments ||
        !resolvedTypes(instance.typeArguments) ||
        !resolvedValues(instance.valueArguments) || !ownerValid ||
        !functionDeclarations.contains(instance.declaration)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidInstanceInventory,
               "lowered function instance is unresolved or differs from MIR");
    }
  }
  for (std::size_t index = 0;
       index < std::min(program.constructorInstances_.size(),
                        program.mir_.constructorInstances().size());
       ++index) {
    const LoweredConstructorInstance &instance =
        program.constructorInstances_[index];
    const MirConstructorInstance &mir =
        program.mir_.constructorInstances()[index];
    const auto *declaration =
        program.findConstructorDeclaration(instance.declaration);
    const auto *owner = program.mir_.findClassInstance(instance.owner);
    if (instance.declaration == 0 &&
        (owner == nullptr ||
         owner->defaultConstructor != SpecialMemberStatus::Generated ||
         !owner->declaredFields.empty() || !mir.parameterTypes.empty() ||
         !mir.typeArguments.empty() || !mir.valueArguments.empty() ||
         mir.initializers.size() != 1 ||
         mir.initializers.front().kind !=
             ConstructorInitializerTargetKind::Base ||
         mir.definitionKind != MirDefinitionKind::Source)) {
      addIssue(
          issues, LoweredProgramIssueKind::InvalidInstanceInventory,
          "generated constructor instance has an invalid structural schedule");
    }
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.owner != mir.owner ||
        instance.typeArguments != mir.typeArguments ||
        instance.valueArguments != mir.valueArguments || instance.owner == 0 ||
        instance.parameterTypes != mir.parameterTypes ||
        mir.constexprConstructor !=
            (declaration != nullptr && declaration->constexprConstructor) ||
        instance.owner > program.classInstances_.size() ||
        !resolvedTypes(instance.typeArguments) ||
        !resolvedValues(instance.valueArguments) ||
        (instance.declaration != 0 &&
         !constructorDeclarations.contains(instance.declaration))) {
      addIssue(
          issues, LoweredProgramIssueKind::InvalidInstanceInventory,
          "lowered constructor instance is unresolved or differs from MIR");
    }
  }
  for (std::size_t index = 0;
       index < std::min(program.destructorInstances_.size(),
                        program.mir_.destructorInstances().size());
       ++index) {
    const LoweredDestructorInstance &instance =
        program.destructorInstances_[index];
    const MirDestructorInstance &mir =
        program.mir_.destructorInstances()[index];
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.owner != mir.owner || instance.owner == 0 ||
        instance.owner > program.classInstances_.size()) {
      addIssue(issues, LoweredProgramIssueKind::InvalidInstanceInventory,
               "lowered destructor instance is unresolved or differs from MIR");
    }
  }
  for (std::size_t index = 0;
       index < std::min(program.lambdaInstances_.size(),
                        program.mir_.lambdaInstances().size());
       ++index) {
    const LoweredLambdaInstance &instance = program.lambdaInstances_[index];
    const MirLambdaInstance &mir = program.mir_.lambdaInstances()[index];
    bool capturesMatch =
        instance.captures.size() == mir.captureSymbols.size() &&
        instance.captures.size() == mir.captureTypes.size() &&
        instance.captures.size() == mir.captureModes.size() &&
        instance.captures.size() == mir.captureRequiresActiveCleanup.size();
    for (std::size_t capture = 0;
         capturesMatch && capture < instance.captures.size(); ++capture) {
      const LoweredLambdaCapture &lowered = instance.captures[capture];
      capturesMatch = lowered.symbol != 0 && !lowered.name.empty() &&
                      lowered.symbol == mir.captureSymbols[capture] &&
                      lowered.type == mir.captureTypes[capture] &&
                      lowered.mode == mir.captureModes[capture] &&
                      lowered.requiresActiveCleanup ==
                          mir.captureRequiresActiveCleanup[capture] &&
                      resolvedType(lowered.type) &&
                      symbolIdentities.contains(lowered.symbol);
    }
    std::vector<SemanticType> parameterTypes;
    parameterTypes.reserve(instance.parameters.size());
    for (const LoweredParameter &parameter : instance.parameters) {
      parameterTypes.push_back(parameter.type);
      requireSymbol(parameter.symbol, 0);
    }
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.declaration != mir.declaration || instance.type != mir.type ||
        instance.returnType != mir.returnType ||
        parameterTypes != mir.parameterTypes ||
        !validParameters(instance.parameters) || !capturesMatch) {
      addIssue(issues, LoweredProgramIssueKind::InvalidInstanceInventory,
               "lowered lambda instance is unresolved or differs from MIR");
    }
  }

  const DerivedGeneratedGraph expected = deriveGeneratedGraph(
      program.mir_, program.declarations_, program.constructorInstances_);
  if (program.generatedItems_ != expected.items) {
    addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
             "generated-item inventory differs from exact MIR-derived "
             "contracts");
  }
  for (const auto &[address, roots] : expected.bodyRoots) {
    const LoweredBody *body = findBody(program.bodies_, address);
    if (body == nullptr || body->requiredGeneratedItems != roots) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "body generated-item roots differ from exact MIR operations",
               address);
    }
  }
  for (const auto &[declaration, roots] : expected.declarationRoots) {
    const LoweredDeclaration *row =
        findDeclaration(program.declarations_, declaration);
    if (row == nullptr || row->requiredGeneratedItems != roots) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "declaration generated-item roots differ from exact lowered "
               "declaration contracts",
               std::nullopt, declaration);
    }
  }

  bool uniqueItems = true;
  for (std::size_t index = 0; index < program.generatedItems_.size(); ++index) {
    const LoweredGeneratedItem &item = program.generatedItems_[index];
    if (!validGeneratedIdentity(item.identity)) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "generated item has an invalid stable identity", std::nullopt,
               std::nullopt, item.identity);
    }
    if (std::find_if(program.generatedItems_.begin(),
                     program.generatedItems_.begin() +
                         static_cast<std::ptrdiff_t>(index),
                     [&](const LoweredGeneratedItem &prior) {
                       return prior.identity == item.identity;
                     }) !=
        program.generatedItems_.begin() + static_cast<std::ptrdiff_t>(index)) {
      uniqueItems = false;
      addIssue(issues, LoweredProgramIssueKind::DuplicateGeneratedItem,
               "generated item identity is duplicated", std::nullopt,
               std::nullopt, item.identity);
    }
    const bool declarationAdapter =
        item.identity.kind ==
            LoweredGeneratedItemKind::StructuralOperatorAdapter ||
        item.identity.kind == LoweredGeneratedItemKind::CallableAdapter ||
        item.identity.kind == LoweredGeneratedItemKind::LifecycleCleanup ||
        item.identity.kind == LoweredGeneratedItemKind::ConcreteInstanceAdapter;
    std::optional<MirBodyAddress> sourceBody;
    bool sourceValid = ordinal(item.sourceKind) <
                       ordinal(LoweredGeneratedItemSourceKind::Count);
    if (item.sourceKind == LoweredGeneratedItemSourceKind::Body) {
      sourceBody = item.sourceBody;
      sourceValid = sourceValid && item.sourceDeclaration == 0 &&
                    !declarationAdapter &&
                    findMirBody(program.mir_, item.sourceBody) != nullptr;
    } else if (item.sourceKind == LoweredGeneratedItemSourceKind::Declaration) {
      const LoweredDeclaration *declaration =
          findDeclaration(program.declarations_, item.sourceDeclaration);
      sourceValid =
          sourceValid && declarationAdapter && item.sourceDeclaration != 0 &&
          item.sourceBody == MirBodyAddress{} && declaration != nullptr &&
          containsGenerated(declaration->requiredGeneratedItems, item.identity);
    }
    if (!sourceValid ||
        !std::is_sorted(item.dependencies.begin(), item.dependencies.end(),
                        generatedLess) ||
        std::adjacent_find(item.dependencies.begin(),
                           item.dependencies.end()) !=
            item.dependencies.end()) {
      addIssue(
          issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
          "generated item source or dependency order is invalid", sourceBody,
          item.sourceDeclaration == 0
              ? std::nullopt
              : std::optional<LoweredDeclarationId>{item.sourceDeclaration},
          item.identity);
    }
    if (item.identity.kind ==
        LoweredGeneratedItemKind::StructuralOperatorAdapter) {
      const auto *payload =
          std::get_if<LoweredStructuralOperatorAdapterItem>(&item.payload);
      const LoweredDeclaration *source =
          findDeclaration(program.declarations_, item.sourceDeclaration);
      const auto *function =
          source == nullptr
              ? nullptr
              : std::get_if<LoweredFunctionDeclaration>(&source->payload);
      if (payload == nullptr || function == nullptr ||
          payload->function != item.identity.owner ||
          function->id != payload->function || !function->overloadedOperator ||
          payload->operation != *function->overloadedOperator) {
        addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
                 "structural operator adapter payload differs from its exact "
                 "function declaration",
                 std::nullopt, item.sourceDeclaration, item.identity);
      }
    } else if (item.identity.kind ==
               LoweredGeneratedItemKind::CallableAdapter) {
      const auto *payload =
          std::get_if<LoweredCallableAdapterItem>(&item.payload);
      const LoweredDeclaration *source =
          findDeclaration(program.declarations_, item.sourceDeclaration);
      const auto *function =
          source == nullptr
              ? nullptr
              : std::get_if<LoweredFunctionDeclaration>(&source->payload);
      if (payload == nullptr || function == nullptr ||
          payload->function != item.identity.owner ||
          function->id != payload->function ||
          payload->capability !=
              callableInvocationCapability(function->receiverMutability)) {
        addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
                 "callable adapter payload differs from its exact function "
                 "declaration",
                 std::nullopt, item.sourceDeclaration, item.identity);
      }
    } else if (item.identity.kind ==
               LoweredGeneratedItemKind::LifecycleCleanup) {
      const auto *payload =
          std::get_if<LoweredLifecycleCleanupItem>(&item.payload);
      const LoweredDeclaration *source =
          findDeclaration(program.declarations_, item.sourceDeclaration);
      const auto *destructorDeclaration =
          source == nullptr
              ? nullptr
              : std::get_if<LoweredDestructorDeclaration>(&source->payload);
      const MirDestructorInstance *destructor =
          payload == nullptr ? nullptr
                             : program.mir_.findDestructorInstance(
                                   payload->destructorInstance);
      const MirClassInstance *owner =
          destructor == nullptr
              ? nullptr
              : program.mir_.findClassInstance(destructor->owner);
      const LoweredClassDeclaration *classDeclaration =
          payload == nullptr
              ? nullptr
              : program.findClassDeclaration(payload->ownerClass);
      const LoweredLifecycleCleanupForm expectedForm =
          owner != nullptr && lifecycleContainsClosure(owner->type)
              ? LoweredLifecycleCleanupForm::RetainedPrimary
          : classDeclaration != nullptr &&
                  !classDeclaration->genericParameters.empty()
              ? LoweredLifecycleCleanupForm::ConcreteSpecialization
              : LoweredLifecycleCleanupForm::OrdinaryClass;
      if (payload == nullptr || destructorDeclaration == nullptr ||
          destructor == nullptr || owner == nullptr ||
          classDeclaration == nullptr ||
          item.identity.owner != payload->destructorInstance ||
          destructorDeclaration->owner != payload->ownerClass ||
          destructor->owner != payload->classInstance ||
          owner->id != payload->classInstance ||
          owner->declaration != payload->ownerClass ||
          owner->destructor != payload->destructorInstance ||
          payload->form != expectedForm ||
          ordinal(payload->form) >=
              ordinal(LoweredLifecycleCleanupForm::Count) ||
          payload->mayRaiseDefinedFailure !=
              destructor->mayRaiseDefinedFailure) {
        addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
                 "lifecycle cleanup payload differs from its exact class and "
                 "destructor instances",
                 std::nullopt, item.sourceDeclaration, item.identity);
      }
    } else if (item.identity.kind ==
               LoweredGeneratedItemKind::ThreadTaskAdapter) {
      const auto *payload =
          std::get_if<LoweredThreadTaskAdapterItem>(&item.payload);
      const MirThreadTaskAdapter *adapter =
          payload == nullptr
              ? nullptr
              : program.mir_.findThreadTaskAdapter(payload->adapter.id);
      const MirBody *body =
          payload == nullptr
              ? nullptr
              : findMirBody(program.mir_, payload->adapter.sourceBody);
      const MirInstruction *spawn = nullptr;
      if (body != nullptr && payload != nullptr) {
        for (const MirBlock &block : body->blocks) {
          const auto found = std::find_if(
              block.instructions.begin(), block.instructions.end(),
              [&](const MirInstruction &instruction) {
                return instruction.id == payload->adapter.spawnInstruction;
              });
          if (found != block.instructions.end()) {
            spawn = &*found;
            break;
          }
        }
      }
      if (payload == nullptr || adapter == nullptr ||
          *adapter != payload->adapter ||
          item.identity.owner != payload->adapter.id ||
          item.sourceBody != payload->adapter.sourceBody || spawn == nullptr ||
          spawn->hirValue != payload->adapter.spawnValue ||
          spawn->intrinsic != IntrinsicKind::ThreadSpawn ||
          spawn->synchronization.kind !=
              SynchronizationOperationKind::ThreadSpawn ||
          spawn->threadTaskAdapter != payload->adapter.id ||
          payload->adapter.taskType == SemanticType::Unknown ||
          payload->adapter.invocation != CallableInvocationCapability::Once ||
          !payload->adapter.catchesWorkerFailure ||
          !payload->adapter.catchesNativeExceptions) {
        addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
                 "thread-task adapter payload differs from its exact MIR "
                 "spawn operation",
                 item.sourceBody, std::nullopt, item.identity);
      }
    } else if (item.identity.kind ==
               LoweredGeneratedItemKind::ConcreteInstanceAdapter) {
      const auto *payload =
          std::get_if<LoweredConcreteInstanceAdapterItem>(&item.payload);
      const LoweredDeclaration *source =
          findDeclaration(program.declarations_, item.sourceDeclaration);
      bool valid = payload != nullptr && source != nullptr &&
                   ordinal(payload->kind) <
                       ordinal(LoweredConcreteInstanceAdapterKind::Count) &&
                   item.identity.owner == payload->body.owner &&
                   item.identity.ordinal == ordinal(payload->kind) + 1;
      if (valid &&
          payload->kind == LoweredConcreteInstanceAdapterKind::Function) {
        const auto *declaration =
            std::get_if<LoweredFunctionDeclaration>(&source->payload);
        const MirFunctionInstance *instance =
            program.mir_.findFunctionInstance(payload->body.owner);
        valid =
            declaration != nullptr && instance != nullptr &&
            payload->body.kind == MirBodyKind::Function &&
            declaration->id == payload->declaration &&
            instance->declaration == payload->declaration &&
            instance->owner.value_or(0) == payload->ownerClassInstance &&
            instance->definitionKind == MirDefinitionKind::Source &&
            instance->mayRaiseDefinedFailure == payload->mayRaiseDefinedFailure;
      } else if (valid && payload->kind ==
                              LoweredConcreteInstanceAdapterKind::Constructor) {
        const auto *declaration =
            std::get_if<LoweredConstructorDeclaration>(&source->payload);
        const MirConstructorInstance *instance =
            program.mir_.findConstructorInstance(payload->body.owner);
        const LoweredConstructorInstance *concrete =
            program.findConstructorInstance(payload->body.owner);
        valid =
            declaration != nullptr && instance != nullptr &&
            concrete != nullptr &&
            payload->body.kind == MirBodyKind::Constructor &&
            declaration->id == payload->declaration &&
            concrete->declaration == payload->declaration &&
            instance->owner == payload->ownerClassInstance &&
            instance->definitionKind == MirDefinitionKind::Source &&
            instance->mayRaiseDefinedFailure == payload->mayRaiseDefinedFailure;
      }
      if (!valid) {
        addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
                 "concrete-instance adapter payload differs from its exact "
                 "generic declaration and MIR body",
                 std::nullopt, item.sourceDeclaration, item.identity);
      }
    }
    for (const LoweredGeneratedItemIdentity &dependency : item.dependencies) {
      if (findGenerated(program.generatedItems_, dependency) == nullptr) {
        addIssue(issues, LoweredProgramIssueKind::MissingGeneratedItem,
                 "generated-item dependency is absent", std::nullopt,
                 std::nullopt, item.identity);
      }
    }
  }

  std::vector<LoweredGeneratedItemIdentity> roots;
  for (const LoweredBody &body : program.bodies_) {
    roots.insert(roots.end(), body.requiredGeneratedItems.begin(),
                 body.requiredGeneratedItems.end());
  }
  for (const LoweredDeclaration &declaration : program.declarations_) {
    roots.insert(roots.end(), declaration.requiredGeneratedItems.begin(),
                 declaration.requiredGeneratedItems.end());
  }
  std::sort(roots.begin(), roots.end(), generatedLess);
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  std::vector<LoweredGeneratedItemIdentity> reachable;
  std::function<void(const LoweredGeneratedItemIdentity &)> markReachable =
      [&](const LoweredGeneratedItemIdentity &identity) {
        if (containsGenerated(reachable, identity)) {
          return;
        }
        reachable.push_back(identity);
        if (const LoweredGeneratedItem *item =
                findGenerated(program.generatedItems_, identity)) {
          for (const LoweredGeneratedItemIdentity &dependency :
               item->dependencies) {
            markReachable(dependency);
          }
        }
      };
  for (const LoweredGeneratedItemIdentity &root : roots) {
    markReachable(root);
  }
  for (const LoweredGeneratedItem &item : program.generatedItems_) {
    if (!containsGenerated(reachable, item.identity)) {
      addIssue(issues, LoweredProgramIssueKind::OrphanGeneratedItem,
               "generated item is not rooted by a body or declaration",
               std::nullopt, std::nullopt, item.identity);
    }
  }

  std::vector<LoweredGeneratedItemIdentity> visiting;
  std::vector<LoweredGeneratedItemIdentity> visited;
  std::function<void(const LoweredGeneratedItem &)> visit =
      [&](const LoweredGeneratedItem &item) {
        if (containsGenerated(visited, item.identity)) {
          return;
        }
        if (containsGenerated(visiting, item.identity)) {
          addIssue(issues,
                   LoweredProgramIssueKind::CyclicGeneratedItemDependency,
                   "generated-item dependency graph contains a cycle",
                   std::nullopt, std::nullopt, item.identity);
          return;
        }
        visiting.push_back(item.identity);
        for (const LoweredGeneratedItemIdentity &dependency :
             item.dependencies) {
          if (const LoweredGeneratedItem *target =
                  findGenerated(program.generatedItems_, dependency)) {
            visit(*target);
          }
        }
        visiting.erase(
            std::remove(visiting.begin(), visiting.end(), item.identity),
            visiting.end());
        visited.push_back(item.identity);
      };
  if (uniqueItems) {
    for (const LoweredGeneratedItem &item : program.generatedItems_) {
      visit(item);
    }
  }

  const std::uint64_t currentSeal =
      fingerprint(LoweredProgramPrinter().print(program));
  if (!program.constructionSeal_ || *program.constructionSeal_ != currentSeal) {
    addIssue(issues, LoweredProgramIssueKind::InvalidConstructionSeal,
             "lowered program differs from its immutable construction seal");
  }
  return issues;
}

} // namespace lang
