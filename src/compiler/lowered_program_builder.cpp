#include "gti/lowered_program_builder.h"
#include "gti/mir_verifier.h"

#include "gti/ast.h"
#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/lowered_program_printer.h"
#include "gti/semantic_model.h"
#include "lowered_program_closure.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace lang {
namespace {

using lowered_detail::addIssue;
using lowered_detail::bodyRole;
using lowered_detail::DerivedGeneratedGraph;
using lowered_detail::deriveGeneratedGraph;
using lowered_detail::findBody;
using lowered_detail::findDeclaration;
using lowered_detail::fingerprint;

[[nodiscard]] LoweredBodyDefinitionKind
bodyDefinition(MirDefinitionKind definition) {
  switch (definition) {
  case MirDefinitionKind::Source:
    return LoweredBodyDefinitionKind::Source;
  case MirDefinitionKind::RuntimeBinding:
    return LoweredBodyDefinitionKind::RuntimeBinding;
  case MirDefinitionKind::Declaration:
    return LoweredBodyDefinitionKind::Declaration;
  }
  return LoweredBodyDefinitionKind::Count;
}

[[nodiscard]] const HirClassInstance *findHirClass(const HirProgram &hir,
                                                   std::size_t id) {
  return id == 0 || id > hir.classInstances().size()
             ? nullptr
             : &hir.classInstances()[id - 1];
}

[[nodiscard]] SourceSpan bodySource(const HirProgram &hir,
                                    MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::Module:
    return {};
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (const HirClassInstance *instance = findHirClass(hir, address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Function:
    if (const HirFunctionInstance *instance =
            hir.findFunctionInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Constructor:
    if (const HirConstructorInstance *instance =
            hir.findConstructorInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  case MirBodyKind::Destructor:
    if (const HirDestructorInstance *instance =
            hir.findDestructorInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->tilde());
    }
    return {};
  case MirBodyKind::Lambda:
    if (const HirLambda *instance = hir.findLambda(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->bracket());
    }
    return {};
  case MirBodyKind::HostedStartup:
    if (const HirFunctionInstance *instance =
            hir.findFunctionInstance(address.owner);
        instance != nullptr && instance->source != nullptr) {
      return tokenSpan(instance->source->name());
    }
    return {};
  }
  return {};
}

[[nodiscard]] std::optional<LoweredBodyIdentity>
captureBodyIdentity(const MirProgram &mir, const HirProgram &hir,
                    MirBodyAddress address) {
  const MirBody *body = findMirBody(mir, address);
  if (body == nullptr || body->kind != address.kind) {
    return std::nullopt;
  }
  LoweredBodyIdentity result{.address = address,
                             .placeDomain = body->placeDomain,
                             .source = bodySource(hir, address)};
  switch (address.kind) {
  case MirBodyKind::Module:
    return address.owner == 0 ? std::optional{std::move(result)} : std::nullopt;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers: {
    const MirClassInstance *instance = mir.findClassInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.declaration = instance->declaration;
    result.concreteOwner = instance->id;
    return result;
  }
  case MirBodyKind::Function: {
    const MirFunctionInstance *instance =
        mir.findFunctionInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.declaration = instance->declaration;
    result.concreteOwner = instance->owner.value_or(0);
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Constructor: {
    const MirConstructorInstance *instance =
        mir.findConstructorInstance(address.owner);
    const HirConstructorInstance *source =
        hir.findConstructorInstance(address.owner);
    if (instance == nullptr || source == nullptr ||
        instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.declaration = source->declaration;
    result.concreteOwner = instance->owner;
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Destructor: {
    const MirDestructorInstance *instance =
        mir.findDestructorInstance(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = bodyDefinition(instance->definitionKind);
    result.concreteOwner = instance->owner;
    return result.definition == LoweredBodyDefinitionKind::Count
               ? std::nullopt
               : std::optional{std::move(result)};
  }
  case MirBodyKind::Lambda: {
    const MirLambdaInstance *instance = mir.findLambda(address.owner);
    if (instance == nullptr || instance->id != address.owner) {
      return std::nullopt;
    }
    result.definition = LoweredBodyDefinitionKind::Source;
    result.declaration = instance->declaration;
    return result;
  }
  case MirBodyKind::HostedStartup: {
    const std::optional<MirHostedStartupPlan> &startup =
        mir.hostedStartupPlan();
    if (!startup || address.owner != startup->entry ||
        mir.hostedStartup() != body) {
      return std::nullopt;
    }
    const MirFunctionInstance *entry = mir.findFunctionInstance(startup->entry);
    if (entry == nullptr || entry->entryKind == ProgramEntryKind::None) {
      return std::nullopt;
    }
    result.definition = LoweredBodyDefinitionKind::CompilerGenerated;
    result.declaration = entry->declaration;
    result.concreteOwner = entry->id;
    return result;
  }
  }
  return std::nullopt;
}

[[nodiscard]] std::vector<std::string> lowerNamePath(const NamePath &path) {
  std::vector<std::string> result;
  result.reserve(path.segments.size());
  for (const Token &segment : path.segments) {
    result.push_back(segment.lexeme);
  }
  return result;
}

[[nodiscard]] std::vector<LoweredGenericParameter>
lowerGenericParameters(const std::vector<GenericParameterInfo> &parameters) {
  std::vector<LoweredGenericParameter> result;
  result.reserve(parameters.size());
  for (const GenericParameterInfo &parameter : parameters) {
    result.push_back(
        {.id = parameter.id,
         .name = parameter.name.lexeme,
         .pack = parameter.pack,
         .value = parameter.value,
         .constraints = parameter.constraints,
         .constraintName = parameter.constraintName
                               ? lowerNamePath(*parameter.constraintName)
                               : std::vector<std::string>{},
         .constraintConcept = parameter.conceptId});
  }
  return result;
}

[[nodiscard]] std::vector<LoweredConceptRequirement>
lowerRequirements(const std::vector<AppliedConceptRequirement> &requirements) {
  std::vector<LoweredConceptRequirement> result;
  result.reserve(requirements.size());
  for (const AppliedConceptRequirement &requirement : requirements) {
    result.push_back({.conceptId = requirement.conceptId,
                      .arguments = requirement.arguments});
  }
  return result;
}

[[nodiscard]] std::vector<LoweredConstraintExpressionNode>
lowerRequirementExpression(
    const std::vector<AppliedConceptRequirement> &requirements) {
  const auto found =
      std::find_if(requirements.begin(), requirements.end(),
                   [](const AppliedConceptRequirement &requirement) {
                     return requirement.expression != nullptr;
                   });
  if (found == requirements.end()) {
    return {};
  }
  std::vector<LoweredConstraintExpressionNode> result;
  result.reserve(found->expression->nodes.size());
  for (const ConstraintExpressionNode &node : found->expression->nodes) {
    result.push_back({.kind = node.kind,
                      .application = node.application,
                      .left = node.left,
                      .right = node.right});
  }
  return result;
}

[[nodiscard]] std::size_t lowerRequirementRoot(
    const std::vector<AppliedConceptRequirement> &requirements) {
  const auto found =
      std::find_if(requirements.begin(), requirements.end(),
                   [](const AppliedConceptRequirement &requirement) {
                     return requirement.expression != nullptr;
                   });
  return found == requirements.end() ? ConstraintExpressionNode::invalidIndex
                                     : found->expression->root;
}

[[nodiscard]] std::vector<LoweredParameter>
lowerParameters(const std::vector<Parameter> &parameters,
                const std::vector<SemanticType> &types,
                const SemanticModel &semantics) {
  std::vector<LoweredParameter> result;
  result.reserve(parameters.size());
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const Parameter &parameter = parameters[index];
    const BindingInfo *binding = semantics.findBinding(parameter);
    result.push_back(
        {.symbol = binding == nullptr ? 0 : binding->symbol,
         .name = parameter.name.lexeme,
         .type = index < types.size() ? types[index] : SemanticType::Unknown,
         .access = binding == nullptr ? AccessMode::ReadOnly : binding->access,
         .mutability = parameter.mutability,
         .pack = parameter.pack.has_value(),
         .hasDefault = parameter.hasDefault(),
         .explicitlyMoved = binding != nullptr && binding->explicitlyMoved,
         .source = tokenSpan(parameter.name)});
  }
  return result;
}

[[nodiscard]] std::vector<LoweredCallableParameter> lowerCallableParameters(
    const std::vector<CallableParameterContract> &parameters) {
  std::vector<LoweredCallableParameter> result;
  result.reserve(parameters.size());
  for (const CallableParameterContract &parameter : parameters) {
    LoweredCallableParameter lowered{
        .parameterIndex = parameter.parameterIndex,
        .genericParameter = parameter.genericParameter,
        .callableType = parameter.callableType,
        .access = parameter.access,
        .boundary = parameter.boundary,
        .ownedTransport = parameter.ownedTransport};
    lowered.signatures.reserve(parameter.signatures.size());
    for (const CallableSignatureRequirement &signature : parameter.signatures) {
      lowered.signatures.push_back({.returnType = signature.returnType,
                                    .parameterTypes = signature.parameterTypes,
                                    .capability = signature.capability});
    }
    lowered.forwardings.reserve(parameter.forwardings.size());
    for (const CallableForwardingRequirement &forwarding :
         parameter.forwardings) {
      lowered.forwardings.push_back(
          {.function = forwarding.function,
           .parameterIndex = forwarding.parameterIndex});
    }
    result.push_back(std::move(lowered));
  }
  return result;
}

[[nodiscard]] std::optional<MirCAbiRecordLayout>
lowerCAbiLayout(const ClassTypeInfo &info, const SemanticModel &semantics) {
  if (!info.cAbiLayout) {
    return std::nullopt;
  }
  MirCAbiRecordLayout result{.sizeBytes = info.cAbiLayout->sizeBytes,
                             .abiAlignmentBytes =
                                 info.cAbiLayout->abiAlignmentBytes};
  result.fields.reserve(info.cAbiLayout->fields.size());
  for (const CAbiRecordFieldLayout &field : info.cAbiLayout->fields) {
    const BindingInfo *binding =
        field.declaration == nullptr
            ? nullptr
            : semantics.findBinding(*field.declaration);
    result.fields.push_back({.field = binding == nullptr ? 0 : binding->symbol,
                             .type = field.type,
                             .offsetBytes = field.offsetBytes,
                             .sizeBytes = field.sizeBytes,
                             .abiAlignmentBytes = field.abiAlignmentBytes});
  }
  return result;
}

[[nodiscard]] std::optional<MirUnionLayout>
lowerUnionLayout(const ClassTypeInfo &info, const SemanticModel &semantics) {
  if (!info.unionLayout) {
    return std::nullopt;
  }
  MirUnionLayout result{.sizeBytes = info.unionLayout->sizeBytes,
                        .abiAlignmentBytes =
                            info.unionLayout->abiAlignmentBytes};
  result.fields.reserve(info.unionLayout->fields.size());
  for (const UnionFieldLayout &field : info.unionLayout->fields) {
    const BindingInfo *binding =
        field.declaration == nullptr
            ? nullptr
            : semantics.findBinding(*field.declaration);
    result.fields.push_back({.field = binding == nullptr ? 0 : binding->symbol,
                             .type = field.type,
                             .sizeBytes = field.sizeBytes,
                             .abiAlignmentBytes = field.abiAlignmentBytes});
  }
  return result;
}

[[nodiscard]] AccessModifier baseAccess(const ClassDecl &declaration,
                                        std::size_t index) {
  if (index >= declaration.bases().size() ||
      !declaration.bases()[index].access) {
    return AccessModifier::Public;
  }
  return declaration.bases()[index].access->kind == TokenKind::PRIVATE
             ? AccessModifier::Private
             : AccessModifier::Public;
}

class DeclarationCollector final {
public:
  DeclarationCollector(const SemanticModel &semantics, const TargetInfo &target)
      : semantics_(semantics), target_(target) {}

  [[nodiscard]] std::vector<LoweredDeclaration>
  collect(const StmtList &declarations) {
    collectList(declarations, 0, 0, {});
    return std::move(result_);
  }

private:
  void collectList(const StmtList &statements, LoweredDeclarationId parent,
                   ClassId ownerClass,
                   const std::vector<std::string> &namespaceScope) {
    for (const StmtPtr &statement : statements) {
      if (statement != nullptr) {
        collectStatement(*statement, parent, ownerClass, namespaceScope);
      }
    }
  }

  void collectStatement(const Stmt &statement, LoweredDeclarationId parent,
                        ClassId ownerClass,
                        const std::vector<std::string> &namespaceScope) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(&statement)) {
      if (const StmtList *active = conditional->activeBranch(target_)) {
        collectList(*active, parent, ownerClass, namespaceScope);
      }
      return;
    }
    if (const auto *group = stmtAs<MutableFieldGroupDecl>(&statement)) {
      collectList(group->members(), parent, ownerClass, namespaceScope);
      return;
    }
    if (stmtAs<StaticAssertDecl>(&statement) != nullptr) {
      return;
    }

    LoweredDeclaration row;
    row.id = result_.size() + 1;
    row.parent = parent;
    row.ownerClass = ownerClass;
    row.ordinal = ++nextOrdinal_[parent];
    row.namespaceScope = namespaceScope;

    const StmtList *children = nullptr;
    std::vector<std::string> childNamespace = namespaceScope;
    ClassId childOwner = ownerClass;
    if (const auto *declaration = stmtAs<NamespaceDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Namespace;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      children = &declaration->declarations();
      childNamespace.push_back(row.name);
    } else if (const auto *declaration =
                   stmtAs<NamespaceAliasDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::NamespaceAlias;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.payload = LoweredNamespaceAliasDeclaration{
          .target = lowerNamePath(declaration->target())};
    } else if (const auto *declaration = stmtAs<TypeAliasDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::TypeAlias;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      if (const TypeAliasInfo *info = semantics_.findTypeAlias(*declaration)) {
        row.payload = LoweredTypeAliasDeclaration{
            .sourceUnit = info->sourceUnit,
            .qualifiedName = info->qualifiedName,
            .type = info->type,
            .compilerPrivate = info->compilerPrivate};
      }
    } else if (const auto *declaration = stmtAs<ClassDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Class;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const ClassTypeInfo *info = semantics_.findClassType(*declaration)) {
        row.semanticIdentity = info->id;
        row.namespaceScope = info->namespaceScope;
        childNamespace = info->namespaceScope;
        childOwner = info->id;
        LoweredClassDeclaration payload{
            .id = info->id,
            .sourceUnit = info->sourceUnit,
            .qualifiedName = info->qualifiedName,
            .kind = info->kind,
            .exactSpecializationPrimary = info->exactSpecializationPrimary,
            .exactTypeArguments = info->exactTypeArguments,
            .exactValueArguments = info->exactValueArguments,
            .genericParameters =
                lowerGenericParameters(info->genericParameters),
            .traits = info->traits,
            .transferPolicy = info->transferPolicy,
            .sharePolicy = info->sharePolicy,
            .cAbiLayout = lowerCAbiLayout(*info, semantics_),
            .unionLayout = lowerUnionLayout(*info, semantics_),
            .compilerCapability = info->compilerCapability,
            .forwardDeclaration = declaration->isForwardDeclaration(),
            .abstract = info->abstract,
            .polymorphic = info->polymorphic,
            .cAbiRecord = info->cAbiRecord,
            .cOpaqueHandle = info->cOpaqueHandle,
            .compilerPrivate = info->compilerPrivate};
        payload.bases.reserve(info->bases.size());
        for (std::size_t index = 0; index < info->bases.size(); ++index) {
          payload.bases.push_back({.type = info->bases[index].type,
                                   .access = baseAccess(*declaration, index),
                                   .interface = info->bases[index].interface});
        }
        if (const ClassLifecycleInfo *lifecycle =
                semantics_.findClassLifecycle(*declaration)) {
          payload.defaultConstructor = lifecycle->defaultConstructor;
          payload.copyConstructor = lifecycle->copyConstructor;
          payload.moveConstructor = lifecycle->moveConstructor;
          payload.copyAssignment = lifecycle->copyAssignment;
          payload.moveAssignment = lifecycle->moveAssignment;
          payload.destructor = lifecycle->destructor;
          payload.requiresActiveDropState = lifecycle->requiresActiveDropState;
        }
        row.payload = std::move(payload);
      }
      children = &declaration->members();
    } else if (const auto *declaration = stmtAs<EnumDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Enum;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      if (const EnumTypeInfo *info = semantics_.findEnumType(*declaration)) {
        row.semanticIdentity = info->id;
        LoweredEnumDeclaration payload{.id = info->id,
                                       .sourceUnit = info->sourceUnit,
                                       .qualifiedName = info->qualifiedName,
                                       .underlyingType = info->underlyingType,
                                       .payload = info->payload,
                                       .compilerPrivate =
                                           info->compilerPrivate};
        payload.enumerators.reserve(info->enumerators.size());
        for (const EnumeratorInfo &enumerator : info->enumerators) {
          payload.enumerators.push_back(
              {.name = enumerator.declaration == nullptr
                           ? std::string{}
                           : enumerator.declaration->name.lexeme,
               .value = enumerator.value,
               .variantIndex = enumerator.variantIndex,
               .payloadTypes = enumerator.payloadTypes,
               .source = enumerator.declaration == nullptr
                             ? SourceSpan{}
                             : tokenSpan(enumerator.declaration->name)});
        }
        row.payload = std::move(payload);
      }
    } else if (const auto *declaration = stmtAs<FunctionDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Function;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const FunctionInfo *info = semantics_.findFunction(*declaration)) {
        row.semanticIdentity = info->id;
        row.payload = LoweredFunctionDeclaration{
            .id = info->id,
            .sourceUnit = info->sourceUnit,
            .ownerClass = info->ownerClass,
            .qualifiedName = info->qualifiedName,
            .returnType = info->returnType,
            .parameters = lowerParameters(declaration->parameters(),
                                          info->parameterTypes, semantics_),
            .genericParameters =
                lowerGenericParameters(info->genericParameters),
            .requirements = lowerRequirements(info->requirements),
            .requirementExpression =
                lowerRequirementExpression(info->requirements),
            .requirementRoot = lowerRequirementRoot(info->requirements),
            .callableParameters =
                lowerCallableParameters(info->callableParameters),
            .requiredParameterCount = info->requiredParameterCount,
            .entryKind = info->entryKind,
            .entryArgumentAppendFunction = info->entryArgumentAppendFunction,
            .receiverMutability = declaration->receiverMutability(),
            .returnMutability = declaration->returnMutability(),
            .overloadedOperator =
                declaration->operatorName()
                    ? std::optional<OverloadedOperator>{declaration
                                                            ->operatorName()
                                                            ->kind}
                    : std::nullopt,
            .definitionKind = declaration->runtimeBinding()
                                  ? MirDefinitionKind::RuntimeBinding
                              : declaration->body() != nullptr
                                  ? MirDefinitionKind::Source
                                  : MirDefinitionKind::Declaration,
            .linkage = info->linkage,
            .externalSymbol = info->externalSymbol,
            .cArrayCountParameter = info->cArrayCountParameter,
            .intrinsic = info->intrinsic,
            .returnBorrowOrigin = info->returnBorrowOrigin,
            .returnBorrowParameter = info->returnBorrowParameter,
            .returnBorrowAccess = info->returnBorrowAccess,
            .returnBorrowPlace = info->returnBorrowPlace,
            .virtualRoots = info->virtualRoots,
            .parameterPack = info->parameterPack,
            .staticMember = info->staticMember,
            .internalLinkage = info->internalLinkage,
            .constexprFunction = info->constexprFunction,
            .virtualMethod = info->virtualMethod,
            .pureVirtual = info->pureVirtual,
            .overrideMethod = info->overrideMethod,
            .hasRequiresClause = declaration->requiresClause().has_value(),
            .compilerPrivate = info->compilerPrivate};
      }
    } else if (const auto *declaration = stmtAs<ConstructorDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Constructor;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const ConstructorInfo *info =
              semantics_.findConstructor(*declaration)) {
        row.semanticIdentity = info->id;
        row.payload = LoweredConstructorDeclaration{
            .id = info->id,
            .owner = info->owner,
            .kind = info->kind,
            .access = info->access,
            .genericParameters =
                lowerGenericParameters(info->genericParameters),
            .requirements = lowerRequirements(info->requirements),
            .requirementExpression =
                lowerRequirementExpression(info->requirements),
            .requirementRoot = lowerRequirementRoot(info->requirements),
            .parameters = lowerParameters(declaration->parameters(),
                                          info->parameterTypes, semantics_),
            .requiredParameterCount = info->requiredParameterCount,
            .borrowParameter = info->borrowParameter,
            .borrowAccess = info->borrowAccess,
            .specifier =
                declaration->specifier()
                    ? std::optional<
                          SpecialMemberSpecifierKind>{declaration->specifier()
                                                          ->kind}
                    : std::nullopt,
            .hasRequiresClause = declaration->requiresClause().has_value(),
            .constexprConstructor = info->constexprConstructor,
            .compilerPrivate = info->compilerPrivate};
      }
    } else if (const auto *declaration = stmtAs<DestructorDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Destructor;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->tilde());
      if (const DestructorInfo *info =
              semantics_.findDestructor(*declaration)) {
        row.semanticIdentity = info->owner;
        row.payload = LoweredDestructorDeclaration{
            .owner = info->owner,
            .access = info->access,
            .specifier =
                declaration->specifier()
                    ? std::optional<
                          SpecialMemberSpecifierKind>{declaration->specifier()
                                                          ->kind}
                    : std::nullopt};
      }
    } else if (const auto *declaration = stmtAs<VariableDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Storage;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      if (const BindingInfo *info = semantics_.findBinding(*declaration)) {
        row.semanticIdentity = info->symbol;
        row.payload = LoweredStorageDeclaration{
            .symbol = info->symbol,
            .ownerClass = ownerClass,
            .type = info->type,
            .access = info->access,
            .traits = info->traits,
            .constant = info->constant,
            .mutability = declaration->mutability(),
            .staticStorage = info->staticStorage,
            .internalLinkage = info->internalLinkage,
            .constexprStorage = declaration->isConstexpr(),
            .hasInitializer = declaration->initializer() != nullptr};
      }
    } else if (const auto *declaration =
                   stmtAs<AccessSpecifierDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Access;
      row.name = declaration->keyword().lexeme;
      row.source = tokenSpan(declaration->keyword());
      row.payload = LoweredAccessDeclaration{.access = declaration->modifier()};
    } else if (const auto *declaration = stmtAs<ExternCDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::LanguageLinkage;
      row.name = declaration->language().lexeme;
      row.source = tokenSpan(declaration->keyword());
      row.payload =
          LoweredLanguageLinkageDeclaration{.linkage = LanguageLinkage::C};
      children = &declaration->declarations();
    } else if (const auto *declaration = stmtAs<ConceptDecl>(&statement)) {
      row.kind = LoweredDeclarationKind::Concept;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->typeParameters().empty();
    } else if (const auto *declaration = stmtAs<EmptyStmt>(&statement)) {
      row.kind = LoweredDeclarationKind::Empty;
      row.source = tokenSpan(declaration->semicolon());
    } else {
      row.kind = LoweredDeclarationKind::Other;
    }

    result_.push_back(std::move(row));
    const LoweredDeclarationId id = result_.back().id;
    if (children != nullptr) {
      collectList(*children, id, childOwner, childNamespace);
    }
  }

  const SemanticModel &semantics_;
  const TargetInfo &target_;
  std::vector<LoweredDeclaration> result_;
  std::unordered_map<LoweredDeclarationId, std::size_t> nextOrdinal_;
};
[[nodiscard]] bool frontendInstancesMatch(const SemanticModel &semantics,
                                          const HirProgram &hir,
                                          const MirProgram &mir,
                                          std::string &detail) {
  const auto reject = [&](std::string message) {
    detail = std::move(message);
    return false;
  };
  if (semantics.analysisSeal() != hir.analysisSeal()) {
    return reject("semantic and HIR analyzed-program seals differ");
  }
  if (semantics.executionProfile() != hir.executionProfile() ||
      hir.executionProfile() != mir.executionProfile() ||
      semantics.placeSnapshot() != hir.module().placeDomain.snapshot ||
      hir.module().placeDomain != mir.module().placeDomain ||
      hir.classInstances().size() != mir.classInstances().size() ||
      hir.functionInstances().size() != mir.functionInstances().size() ||
      hir.nativeCallbackAdapters().size() !=
          mir.nativeCallbackAdapters().size() ||
      hir.threadTaskAdapters().size() != mir.threadTaskAdapters().size() ||
      hir.constructorInstances().size() != mir.constructorInstances().size() ||
      hir.destructorInstances().size() != mir.destructorInstances().size() ||
      hir.lambdaInstances().size() != mir.lambdaInstances().size() ||
      std::count_if(
          hir.enumDeclarations().begin(), hir.enumDeclarations().end(),
          [](const HirEnum &enumeration) { return enumeration.payload; }) !=
          static_cast<std::ptrdiff_t>(mir.payloadEnums().size())) {
    return reject("frontend profile, place domain, or instance counts differ");
  }
  for (const HirEnum &source : hir.enumDeclarations()) {
    if (!source.payload) {
      continue;
    }
    const MirPayloadEnum *lowered = mir.findPayloadEnum(source.declaration);
    if (lowered == nullptr || source.sourceUnit != lowered->sourceUnit ||
        source.traits != lowered->traits ||
        source.requiresActiveCleanup != lowered->requiresActiveCleanup ||
        source.enumerators.size() != lowered->variants.size()) {
      return reject("payload enum " + std::to_string(source.declaration) +
                    " differs between HIR and MIR");
    }
    for (std::size_t index = 0; index < source.enumerators.size(); ++index) {
      const HirEnumerator &hirVariant = source.enumerators[index];
      const MirPayloadEnumVariant &mirVariant = lowered->variants[index];
      if (hirVariant.variantIndex != mirVariant.variantIndex ||
          hirVariant.payloadTypes != mirVariant.payloadTypes ||
          hirVariant.payloadRequiresActiveCleanup !=
              mirVariant.payloadRequiresActiveCleanup) {
        return reject("payload enum " + std::to_string(source.declaration) +
                      " variant " + std::to_string(index) +
                      " differs between HIR and MIR");
      }
    }
  }
  for (std::size_t index = 0; index < hir.classInstances().size(); ++index) {
    const HirClassInstance &source = hir.classInstances()[index];
    const MirClassInstance &lowered = mir.classInstances()[index];
    const ClassTypeInfo *info = source.source == nullptr
                                    ? nullptr
                                    : semantics.findClassType(*source.source);
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.type != lowered.type || source.kind != lowered.kind ||
        source.fieldInitializers.placeDomain !=
            lowered.fieldInitializers.placeDomain ||
        source.staticFieldInitializers.placeDomain !=
            lowered.staticFieldInitializers.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("class instance " + std::to_string(index + 1) + " differs");
    }
  }
  for (std::size_t index = 0; index < hir.functionInstances().size(); ++index) {
    const HirFunctionInstance &source = hir.functionInstances()[index];
    const MirFunctionInstance &lowered = mir.functionInstances()[index];
    const FunctionInfo *info = source.source == nullptr
                                   ? nullptr
                                   : semantics.findFunction(*source.source);
    const MirDefinitionKind expectedDefinition =
        source.source == nullptr           ? MirDefinitionKind::Declaration
        : source.source->runtimeBinding()  ? MirDefinitionKind::RuntimeBinding
        : source.source->body() != nullptr ? MirDefinitionKind::Source
                                           : MirDefinitionKind::Declaration;
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.typeArguments != lowered.typeArguments ||
        source.valueArguments != lowered.valueArguments ||
        source.owner != lowered.owner ||
        source.returnType != lowered.returnType ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.entryKind != lowered.entryKind ||
        source.entryArgumentAppendTarget != lowered.entryArgumentAppendTarget ||
        source.staticMember != lowered.staticMember ||
        source.constexprFunction != lowered.constexprFunction ||
        source.linkage != lowered.linkage ||
        source.externalSymbol != lowered.externalSymbol ||
        source.body.placeDomain != lowered.body.placeDomain ||
        lowered.definitionKind != expectedDefinition || info == nullptr ||
        info->id != source.declaration || info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("function instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.constructorInstances().size();
       ++index) {
    const HirConstructorInstance &source = hir.constructorInstances()[index];
    const MirConstructorInstance &lowered = mir.constructorInstances()[index];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        source.typeArguments != lowered.typeArguments ||
        source.valueArguments != lowered.valueArguments ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("constructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.destructorInstances().size();
       ++index) {
    const HirDestructorInstance &source = hir.destructorInstances()[index];
    const MirDestructorInstance &lowered = mir.destructorInstances()[index];
    if (source.id != lowered.id || source.owner != lowered.owner ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("destructor instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.lambdaInstances().size(); ++index) {
    const HirLambda &source = hir.lambdaInstances()[index];
    const MirLambdaInstance &lowered = mir.lambdaInstances()[index];
    if (source.id != lowered.id || source.declaration != lowered.declaration ||
        source.type != lowered.type ||
        source.returnType != lowered.returnType ||
        source.parameterTypes != lowered.parameterTypes ||
        source.parameterBindings != lowered.parameterBindings ||
        source.body.placeDomain != lowered.body.placeDomain) {
      return reject("lambda instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.nativeCallbackAdapters().size();
       ++index) {
    const HirNativeCallbackAdapter &source =
        hir.nativeCallbackAdapters()[index];
    const MirNativeCallbackAdapter &lowered =
        mir.nativeCallbackAdapters()[index];
    if (source.id != lowered.id || source.target != lowered.target ||
        source.type != lowered.type) {
      return reject("native callback adapter " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.threadTaskAdapters().size();
       ++index) {
    const HirThreadTaskAdapter &source = hir.threadTaskAdapters()[index];
    const MirThreadTaskAdapter &lowered = mir.threadTaskAdapters()[index];
    const HirFunctionInstance *functionTarget =
        source.functionTarget ? hir.findFunctionInstance(*source.functionTarget)
                              : nullptr;
    const HirLambda *lambdaTarget =
        source.lambdaTarget ? hir.findLambda(*source.lambdaTarget) : nullptr;
    const SemanticType resultType =
        functionTarget != nullptr ? functionTarget->returnType
        : lambdaTarget != nullptr ? lambdaTarget->returnType
                                  : SemanticType::Unknown;
    if (source.id != lowered.id || source.taskType != lowered.taskType ||
        resultType != lowered.resultType ||
        source.spawnValue != lowered.spawnValue ||
        source.functionTarget != lowered.functionTarget ||
        source.lambdaTarget != lowered.lambdaTarget ||
        source.targetInvocation != lowered.targetInvocation ||
        source.invocation != lowered.invocation) {
      return reject("thread task adapter " + std::to_string(index + 1) +
                    " differs");
    }
  }
  return true;
}
} // namespace
LoweredProgramBuild LoweredProgramBuilder::build(
    const Program &program, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &sourceMir,
    const MirProgram &optimizedMir,
    const std::vector<MirTransformRecord> &transformations,
    const TargetInfo &target) const {
  LoweredProgramBuild build;
  if (!target.supported()) {
    addIssue(build.issues, LoweredProgramIssueKind::UnsupportedTarget,
             "lowered program requires a supported target data layout");
  }
  if (!semantics.analysisSeal().matchesTarget(target) ||
      !semantics.analysisSeal().matchesProgram(program, target)) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             "supplied Program or target differs from the semantic analysis "
             "seal");
  }
  const HirProgramPlanVerificationResult hirPlans =
      verifyHirProgramPlans(semantics, hir);
  if (!hirPlans.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             hirPlans.errors.empty() ? "semantic and HIR program plans differ"
                                     : hirPlans.errors.front());
  }
  const MirVerificationResult sourceVerification = verifyMirProgram(sourceMir);
  if (!sourceMir.valid() || !sourceVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidSourceMir,
             sourceVerification.errors.empty()
                 ? "source MIR is not marked valid"
                 : sourceVerification.errors.front().message);
  }
  const MirVerificationResult optimizedVerification =
      verifyMirProgram(optimizedMir);
  if (!optimizedMir.valid() || !optimizedVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimizedMir,
             optimizedVerification.errors.empty()
                 ? "optimized MIR is not marked valid"
                 : optimizedVerification.errors.front().message);
  }
  const FailureMetadataVerificationResult failureVerification =
      verifyFailureMetadata(optimizedMir.failureMetadata());
  if (!failureVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidFailureMetadata,
             failureVerification.errors.empty()
                 ? "optimized MIR failure metadata is invalid"
                 : failureVerification.errors.front());
  }
  const MirVerificationResult optimization =
      verifyMirOptimizationCoherence(sourceMir, optimizedMir, transformations);
  if (!optimization.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimization,
             optimization.errors.empty()
                 ? "optimized MIR differs from the source MIR contract"
                 : optimization.errors.front().message);
  }
  std::string frontendMismatch;
  if (!frontendInstancesMatch(semantics, hir, sourceMir, frontendMismatch)) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             std::move(frontendMismatch));
  }
  if (!build.issues.empty()) {
    return build;
  }

  LoweredProgram lowered;
  lowered.target_ = target;
  lowered.mir_ = optimizedMir;
  lowered.declarations_ =
      DeclarationCollector(semantics, target).collect(program.declarations());
  for (LoweredDeclaration &declaration : lowered.declarations_) {
    auto *enumeration =
        std::get_if<LoweredEnumDeclaration>(&declaration.payload);
    if (enumeration == nullptr || !enumeration->payload) {
      continue;
    }
    const auto hirEnumeration = std::find_if(
        hir.enumDeclarations().begin(), hir.enumDeclarations().end(),
        [&](const HirEnum &candidate) {
          return candidate.declaration == enumeration->id;
        });
    const MirPayloadEnum *mirEnumeration =
        optimizedMir.findPayloadEnum(enumeration->id);
    if (hirEnumeration == hir.enumDeclarations().end() ||
        mirEnumeration == nullptr ||
        hirEnumeration->enumerators.size() != enumeration->enumerators.size() ||
        mirEnumeration->variants.size() != enumeration->enumerators.size()) {
      addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
               "payload enum lacks matching HIR/MIR lifecycle authority");
      continue;
    }
    enumeration->traits = mirEnumeration->traits;
    enumeration->requiresActiveCleanup = mirEnumeration->requiresActiveCleanup;
    for (std::size_t index = 0; index < enumeration->enumerators.size();
         ++index) {
      enumeration->enumerators[index].payloadRequiresActiveCleanup =
          mirEnumeration->variants[index].payloadRequiresActiveCleanup;
    }
  }

  lowered.symbols_.reserve(semantics.database().symbols().size());
  for (const SymbolRecord &symbol : semantics.database().symbols()) {
    lowered.symbols_.push_back(
        {.id = symbol.id,
         .kind = symbol.kind,
         .name = symbol.name,
         .qualifiedName = symbol.qualifiedName,
         .sourceUnit = symbol.sourceUnit,
         .nameSource = symbol.nameSpan,
         .declarationSource = symbol.declarationSpan,
         .definitionSource = symbol.definitionSpan,
         .type = symbol.type,
         .traits = symbol.traits,
         .access = symbol.access,
         .mutableBinding = symbol.mutableBinding,
         .defaultLibrary = symbol.defaultLibrary,
         .staticMember = symbol.staticMember,
         .internalLinkage = symbol.internalLinkage,
         .generated = symbol.generated,
         .compilerPrivate = symbol.compilerPrivate,
         .instanceDependentType = symbol.instanceDependentType});
  }

  lowered.classInstances_.reserve(hir.classInstances().size());
  for (const HirClassInstance &instance : hir.classInstances()) {
    lowered.classInstances_.push_back(
        {.id = instance.id,
         .sourceUnit = instance.sourceUnit,
         .declaration = instance.declaration,
         .typeArguments = instance.typeArguments,
         .valueArguments = instance.valueArguments,
         .source = instance.source == nullptr
                       ? SourceSpan{}
                       : tokenSpan(instance.source->name())});
  }
  lowered.functionInstances_.reserve(hir.functionInstances().size());
  for (const HirFunctionInstance &instance : hir.functionInstances()) {
    lowered.functionInstances_.push_back(
        {.id = instance.id,
         .sourceUnit = instance.sourceUnit,
         .declaration = instance.declaration,
         .owner = instance.owner,
         .typeArguments = instance.typeArguments,
         .valueArguments = instance.valueArguments,
         .instantiationSource = instance.instantiationSite,
         .source = instance.source == nullptr
                       ? SourceSpan{}
                       : tokenSpan(instance.source->name())});
  }
  lowered.constructorInstances_.reserve(hir.constructorInstances().size());
  for (const HirConstructorInstance &instance : hir.constructorInstances()) {
    lowered.constructorInstances_.push_back(
        {.id = instance.id,
         .sourceUnit = instance.sourceUnit,
         .declaration = instance.declaration,
         .owner = instance.owner,
         .typeArguments = instance.typeArguments,
         .valueArguments = instance.valueArguments,
         .parameterTypes = instance.parameterTypes,
         .instantiationSource = instance.instantiationSite,
         .source = instance.source == nullptr
                       ? SourceSpan{}
                       : tokenSpan(instance.source->name())});
  }
  lowered.destructorInstances_.reserve(hir.destructorInstances().size());
  for (const HirDestructorInstance &instance : hir.destructorInstances()) {
    lowered.destructorInstances_.push_back(
        {.id = instance.id,
         .sourceUnit = instance.sourceUnit,
         .owner = instance.owner,
         .source = instance.source == nullptr
                       ? SourceSpan{}
                       : tokenSpan(instance.source->tilde())});
  }
  lowered.lambdaInstances_.reserve(hir.lambdaInstances().size());
  for (const HirLambda &instance : hir.lambdaInstances()) {
    LoweredLambdaInstance loweredLambda{
        .id = instance.id,
        .declaration = instance.declaration,
        .type = instance.type,
        .returnType = instance.returnType,
        .parameters = instance.source == nullptr
                          ? std::vector<LoweredParameter>{}
                          : lowerParameters(instance.source->parameters(),
                                            instance.parameterTypes, semantics),
        .traits = instance.traits,
        .source = instance.source == nullptr
                      ? SourceSpan{}
                      : tokenSpan(instance.source->bracket())};
    loweredLambda.captures.reserve(instance.captures.size());
    for (std::size_t index = 0; index < instance.captures.size(); ++index) {
      const LambdaCaptureInfo &capture = instance.captures[index];
      loweredLambda.captures.push_back(
          {.symbol = capture.bindingSymbol,
           .name = capture.capture.lexeme,
           .type = capture.type,
           .traits = capture.traits,
           .mode = capture.mode,
           .requiresActiveCleanup =
               index < instance.captureRequiresActiveCleanup.size() &&
               instance.captureRequiresActiveCleanup[index]});
    }
    lowered.lambdaInstances_.push_back(std::move(loweredLambda));
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(optimizedMir)) {
    const std::optional<LoweredBodyIdentity> identity =
        captureBodyIdentity(optimizedMir, hir, address);
    if (!identity) {
      addIssue(build.issues, LoweredProgramIssueKind::InvalidBodyInventory,
               "optimized MIR body lacks a stable lowered identity", address);
      continue;
    }
    lowered.bodies_.push_back(
        {.identity = *identity, .role = bodyRole(optimizedMir, *identity)});
  }

  DerivedGeneratedGraph graph = deriveGeneratedGraph(
      optimizedMir, lowered.declarations_, lowered.constructorInstances_);
  lowered.generatedItems_ = std::move(graph.items);
  for (auto &[address, roots] : graph.bodyRoots) {
    if (LoweredBody *body = findBody(lowered.bodies_, address)) {
      body->requiredGeneratedItems = std::move(roots);
    }
  }
  for (auto &[declaration, roots] : graph.declarationRoots) {
    if (LoweredDeclaration *row =
            findDeclaration(lowered.declarations_, declaration)) {
      row->requiredGeneratedItems = std::move(roots);
    }
  }
  if (!build.issues.empty()) {
    return build;
  }

  lowered.constructionSeal_ =
      fingerprint(LoweredProgramPrinter().print(lowered));
  build.issues = verifyLoweredProgram(lowered);
  if (build.issues.empty()) {
    build.program.emplace(std::move(lowered));
  }
  return build;
}
} // namespace lang
