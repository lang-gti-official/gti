#include "gti/lowered_program.h"

#include "gti/ast.h"
#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/lowered_program_printer.h"
#include "gti/semantic_analyzer.h"

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

template <typename Enum>
[[nodiscard]] constexpr std::size_t ordinal(Enum value) {
  return static_cast<std::size_t>(value);
}

[[nodiscard]] bool generatedLess(const LoweredGeneratedItemIdentity &left,
                                 const LoweredGeneratedItemIdentity &right) {
  return std::tuple{ordinal(left.kind), left.owner, left.ordinal} <
         std::tuple{ordinal(right.kind), right.owner, right.ordinal};
}

[[nodiscard]] bool
containsGenerated(const std::vector<LoweredGeneratedItemIdentity> &items,
                  const LoweredGeneratedItemIdentity &identity) {
  return std::find(items.begin(), items.end(), identity) != items.end();
}

[[nodiscard]] const LoweredGeneratedItem *
findGenerated(const std::vector<LoweredGeneratedItem> &items,
              const LoweredGeneratedItemIdentity &identity) {
  const auto found =
      std::find_if(items.begin(), items.end(),
                   [&](const auto &item) { return item.identity == identity; });
  return found == items.end() ? nullptr : &*found;
}

[[nodiscard]] const LoweredBody *
findBody(const std::vector<LoweredBody> &bodies, MirBodyAddress address) {
  const auto found =
      std::find_if(bodies.begin(), bodies.end(), [&](const LoweredBody &body) {
        return body.identity.address == address;
      });
  return found == bodies.end() ? nullptr : &*found;
}

[[nodiscard]] LoweredBody *findBody(std::vector<LoweredBody> &bodies,
                                    MirBodyAddress address) {
  return const_cast<LoweredBody *>(findBody(std::as_const(bodies), address));
}

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

[[nodiscard]] bool isInitializerBody(MirBodyKind kind) {
  return kind == MirBodyKind::Module ||
         kind == MirBodyKind::FieldInitializers ||
         kind == MirBodyKind::StaticFieldInitializers;
}

[[nodiscard]] bool isCanonicalNoExecutionInitializer(const MirBody &body) {
  if (!isInitializerBody(body.kind) || body.returnType != SemanticType::Void ||
      body.entry != 1 || body.blocks.size() != 1 || !body.places.empty() ||
      !body.loans.empty() || !body.fullExpressions.empty() ||
      !body.cleanupBoundaries.empty() || !body.dropObligations.empty() ||
      !body.failureRecords.empty() || !body.values.empty() ||
      !body.valueUses.empty()) {
    return false;
  }
  MirBlock expected;
  expected.id = 1;
  expected.terminator.kind = MirTerminatorKind::Exit;
  expected.reachable = true;
  return body.blocks.front() == expected;
}

[[nodiscard]] bool
hasExecutableProgramInitialization(const MirProgram &program) {
  return std::any_of(
      program.programInitializationPlan().steps.begin(),
      program.programInitializationPlan().steps.end(), [](const auto &step) {
        return step.role == ProgramInitializationStepRole::Initializer;
      });
}

[[nodiscard]] std::size_t
programInitializationBodyCallCount(const MirProgram &program) {
  const MirBody *body = program.hostedStartup();
  if (body == nullptr) {
    return 0;
  }
  std::size_t result = 0;
  for (const MirBlock &block : body->blocks) {
    result += static_cast<std::size_t>(std::count_if(
        block.instructions.begin(), block.instructions.end(),
        [](const MirInstruction &instruction) {
          return instruction.kind == MirInstructionKind::CallBody &&
                 instruction.bodyTarget ==
                     MirBodyAddress{.kind = MirBodyKind::Module, .owner = 0};
        }));
  }
  return result;
}

[[nodiscard]] std::uint64_t fingerprint(std::string_view text) {
  constexpr std::uint64_t offset = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  std::uint64_t result = offset;
  for (const unsigned char byte : text) {
    result ^= byte;
    result *= prime;
  }
  return result;
}

void addIssue(
    std::vector<LoweredProgramIssue> &issues, LoweredProgramIssueKind kind,
    std::string detail, std::optional<MirBodyAddress> body = std::nullopt,
    std::optional<LoweredDeclarationId> declaration = std::nullopt,
    std::optional<LoweredGeneratedItemIdentity> generated = std::nullopt) {
  issues.push_back({.kind = kind,
                    .detail = std::move(detail),
                    .body = body,
                    .declaration = declaration,
                    .generatedItem = generated});
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

[[nodiscard]] LoweredBodyRole bodyRole(const MirProgram &program,
                                       const LoweredBodyIdentity &identity) {
  switch (identity.definition) {
  case LoweredBodyDefinitionKind::ImplicitSource: {
    if (identity.address.kind == MirBodyKind::Module) {
      return hasExecutableProgramInitialization(program)
                 ? LoweredBodyRole::SourceExecutable
                 : LoweredBodyRole::DataOnly;
    }
    const MirBody *body = findMirBody(program, identity.address);
    return body != nullptr && isCanonicalNoExecutionInitializer(*body)
               ? LoweredBodyRole::DataOnly
               : LoweredBodyRole::SourceExecutable;
  }
  case LoweredBodyDefinitionKind::Source:
  case LoweredBodyDefinitionKind::CompilerGenerated:
    return LoweredBodyRole::SourceExecutable;
  case LoweredBodyDefinitionKind::RuntimeBinding:
  case LoweredBodyDefinitionKind::Declaration:
    return LoweredBodyRole::AbiDeclaration;
  case LoweredBodyDefinitionKind::Count:
    return LoweredBodyRole::Count;
  }
  return LoweredBodyRole::Count;
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
                               : std::vector<std::string>{}});
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
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(&statement)) {
      if (const StmtList *active = conditional->activeBranch(target_)) {
        collectList(*active, parent, ownerClass, namespaceScope);
      }
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
    if (const auto *declaration =
            dynamic_cast<const NamespaceDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Namespace;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      children = &declaration->declarations();
      childNamespace.push_back(row.name);
    } else if (const auto *declaration =
                   dynamic_cast<const NamespaceAliasDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::NamespaceAlias;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.payload = LoweredNamespaceAliasDeclaration{
          .target = lowerNamePath(declaration->target())};
    } else if (const auto *declaration =
                   dynamic_cast<const TypeAliasDecl *>(&statement)) {
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
    } else if (const auto *declaration =
                   dynamic_cast<const ClassDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Class;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->genericParameters().empty();
      if (const ClassTypeInfo *info = semantics_.findClassType(*declaration)) {
        row.semanticIdentity = info->id;
        childOwner = info->id;
        LoweredClassDeclaration payload{
            .id = info->id,
            .sourceUnit = info->sourceUnit,
            .qualifiedName = info->qualifiedName,
            .kind = info->kind,
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
    } else if (const auto *declaration =
                   dynamic_cast<const EnumDecl *>(&statement)) {
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
    } else if (const auto *declaration =
                   dynamic_cast<const FunctionDecl *>(&statement)) {
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
            .compilerPrivate = info->compilerPrivate};
      }
    } else if (const auto *declaration =
                   dynamic_cast<const ConstructorDecl *>(&statement)) {
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
            .compilerPrivate = info->compilerPrivate};
      }
    } else if (const auto *declaration =
                   dynamic_cast<const DestructorDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Destructor;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->tilde());
      if (const DestructorInfo *info =
              semantics_.findDestructor(*declaration)) {
        row.semanticIdentity = info->owner;
        row.payload = LoweredDestructorDeclaration{.owner = info->owner,
                                                   .access = info->access};
      }
    } else if (const auto *declaration =
                   dynamic_cast<const VariableDecl *>(&statement)) {
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
                   dynamic_cast<const AccessSpecifierDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Access;
      row.name = declaration->keyword().lexeme;
      row.source = tokenSpan(declaration->keyword());
      row.payload = LoweredAccessDeclaration{.access = declaration->modifier()};
    } else if (const auto *declaration =
                   dynamic_cast<const ExternCDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::LanguageLinkage;
      row.name = declaration->language().lexeme;
      row.source = tokenSpan(declaration->keyword());
      row.payload =
          LoweredLanguageLinkageDeclaration{.linkage = LanguageLinkage::C};
      children = &declaration->declarations();
    } else if (const auto *declaration =
                   dynamic_cast<const ConceptDecl *>(&statement)) {
      row.kind = LoweredDeclarationKind::Concept;
      row.name = declaration->name().lexeme;
      row.source = tokenSpan(declaration->name());
      row.generic = !declaration->typeParameters().empty();
    } else if (const auto *declaration =
                   dynamic_cast<const EmptyStmt *>(&statement)) {
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

struct DerivedGeneratedGraph {
  std::vector<LoweredGeneratedItem> items;
  std::vector<
      std::pair<MirBodyAddress, std::vector<LoweredGeneratedItemIdentity>>>
      roots;
};

[[nodiscard]] DerivedGeneratedGraph
deriveGeneratedGraph(const MirProgram &program) {
  DerivedGeneratedGraph graph;
  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    graph.roots.emplace_back(address,
                             std::vector<LoweredGeneratedItemIdentity>{});
  }
  const auto root = [&](MirBodyAddress address,
                        LoweredGeneratedItemIdentity identity) {
    const auto found =
        std::find_if(graph.roots.begin(), graph.roots.end(),
                     [&](const auto &entry) { return entry.first == address; });
    if (found != graph.roots.end()) {
      found->second.push_back(identity);
    }
  };

  std::optional<LoweredGeneratedItemIdentity> initialization;
  if (hasExecutableProgramInitialization(program)) {
    initialization = LoweredGeneratedItemIdentity{
        .kind = LoweredGeneratedItemKind::ProgramInitialization};
    graph.items.push_back(
        {.identity = *initialization,
         .sourceBody = {.kind = MirBodyKind::Module, .owner = 0}});
    root({.kind = MirBodyKind::Module, .owner = 0}, *initialization);
  }

  if (const std::optional<MirHostedStartupPlan> &startup =
          program.hostedStartupPlan();
      startup && program.hostedStartup() != nullptr) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::HostedEntry, .owner = startup->entry};
    LoweredGeneratedItem item{.identity = identity,
                              .sourceBody = {.kind = MirBodyKind::HostedStartup,
                                             .owner = startup->entry}};
    if (initialization && programInitializationBodyCallCount(program) != 0) {
      item.dependencies.push_back(*initialization);
    }
    graph.items.push_back(std::move(item));
    root({.kind = MirBodyKind::HostedStartup, .owner = startup->entry},
         identity);
  }

  for (const MirNativeCallbackAdapter &adapter :
       program.nativeCallbackAdapters()) {
    const LoweredGeneratedItemIdentity identity{
        .kind = LoweredGeneratedItemKind::NativeInteropAdapter,
        .owner = adapter.id};
    graph.items.push_back(
        {.identity = identity,
         .sourceBody = {.kind = MirBodyKind::Function, .owner = adapter.target},
         .payload = LoweredNativeCallbackItem{.adapter = adapter}});
  }

  for (const MirBodyAddress address : enumerateMirBodyAddresses(program)) {
    const MirBody *body = findMirBody(program, address);
    if (body == nullptr) {
      continue;
    }
    for (const MirBlock &block : body->blocks) {
      for (const MirInstruction &instruction : block.instructions) {
        if (instruction.operation != MirOperation::NativeCallback ||
            !instruction.nativeCallbackAdapter) {
          continue;
        }
        root(address, {.kind = LoweredGeneratedItemKind::NativeInteropAdapter,
                       .owner = *instruction.nativeCallbackAdapter});
      }
    }
  }

  for (auto &[address, roots] : graph.roots) {
    static_cast<void>(address);
    std::sort(roots.begin(), roots.end(), generatedLess);
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  }
  for (LoweredGeneratedItem &item : graph.items) {
    std::sort(item.dependencies.begin(), item.dependencies.end(),
              generatedLess);
    item.dependencies.erase(
        std::unique(item.dependencies.begin(), item.dependencies.end()),
        item.dependencies.end());
  }
  std::sort(
      graph.items.begin(), graph.items.end(),
      [](const LoweredGeneratedItem &left, const LoweredGeneratedItem &right) {
        return generatedLess(left.identity, right.identity);
      });
  return graph;
}

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
      hir.constructorInstances().size() != mir.constructorInstances().size() ||
      hir.destructorInstances().size() != mir.destructorInstances().size() ||
      hir.lambdaInstances().size() != mir.lambdaInstances().size()) {
    return reject("frontend profile, place domain, or instance counts differ");
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
  return true;
}

[[nodiscard]] bool
validGeneratedIdentity(const LoweredGeneratedItemIdentity &identity) {
  if (ordinal(identity.kind) >= ordinal(LoweredGeneratedItemKind::Count)) {
    return false;
  }
  if (identity.kind == LoweredGeneratedItemKind::ProgramInitialization) {
    return identity.owner == 0 && identity.ordinal == 0;
  }
  if (identity.kind == LoweredGeneratedItemKind::HostedEntry ||
      identity.kind == LoweredGeneratedItemKind::NativeInteropAdapter) {
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
    return false;
  }
  return true;
}

[[nodiscard]] bool
validGenericParameters(const std::vector<LoweredGenericParameter> &parameters) {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const LoweredGenericParameter &parameter = parameters[index];
    if (parameter.id == 0 || parameter.name.empty() ||
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
    if (payload == nullptr || payload->id == 0 ||
        declaration.semanticIdentity != payload->id ||
        declaration.name.empty() || payload->qualifiedName.empty() ||
        declaration.generic != !payload->genericParameters.empty() ||
        !validGenericParameters(payload->genericParameters) ||
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
                     !std::all_of(enumerator.payloadTypes.begin(),
                                  enumerator.payloadTypes.end(), resolvedType);
            })) {
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
                      return requirement.conceptId == 0 ||
                             !std::all_of(requirement.arguments.begin(),
                                          requirement.arguments.end(),
                                          resolvedType);
                    })) {
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

const LoweredBody *LoweredProgram::findBody(MirBodyAddress address) const {
  return lang::findBody(bodies_, address);
}

const LoweredDeclaration *
LoweredProgram::findDeclaration(LoweredDeclarationId id) const {
  return id == 0 || id > declarations_.size() ? nullptr
                                              : &declarations_[id - 1];
}

const LoweredClassDeclaration *
LoweredProgram::findClassDeclaration(ClassId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredEnumDeclaration *
LoweredProgram::findEnumDeclaration(EnumId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredEnumDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredFunctionDeclaration *
LoweredProgram::findFunctionDeclaration(FunctionId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredFunctionDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredConstructorDeclaration *
LoweredProgram::findConstructorDeclaration(ConstructorId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredConstructorDeclaration>(&declaration.payload);
        payload != nullptr && payload->id == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredStorageDeclaration *
LoweredProgram::findStorageDeclaration(SymbolId id) const {
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredStorageDeclaration>(&declaration.payload);
        payload != nullptr && payload->symbol == id) {
      return payload;
    }
  }
  return nullptr;
}

const LoweredGenericParameter *
LoweredProgram::findGenericParameter(GenericParameterId id) const {
  const auto find = [id](const std::vector<LoweredGenericParameter> &items) {
    const auto found = std::find_if(
        items.begin(), items.end(),
        [id](const LoweredGenericParameter &item) { return item.id == id; });
    return found == items.end() ? nullptr : &*found;
  };
  for (const LoweredDeclaration &declaration : declarations_) {
    if (const auto *payload =
            std::get_if<LoweredClassDeclaration>(&declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    } else if (const auto *payload = std::get_if<LoweredFunctionDeclaration>(
                   &declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    } else if (const auto *payload = std::get_if<LoweredConstructorDeclaration>(
                   &declaration.payload)) {
      if (const LoweredGenericParameter *parameter =
              find(payload->genericParameters)) {
        return parameter;
      }
    }
  }
  return nullptr;
}

const LoweredSymbol *LoweredProgram::findSymbol(SymbolId id) const {
  const auto found = std::find_if(
      symbols_.begin(), symbols_.end(),
      [id](const LoweredSymbol &symbol) { return symbol.id == id; });
  return found == symbols_.end() ? nullptr : &*found;
}

const LoweredClassInstance *
LoweredProgram::findClassInstance(HirClassInstanceId id) const {
  return id == 0 || id > classInstances_.size() ? nullptr
                                                : &classInstances_[id - 1];
}

const LoweredFunctionInstance *
LoweredProgram::findFunctionInstance(HirFunctionInstanceId id) const {
  return id == 0 || id > functionInstances_.size()
             ? nullptr
             : &functionInstances_[id - 1];
}

const LoweredConstructorInstance *
LoweredProgram::findConstructorInstance(HirConstructorInstanceId id) const {
  return id == 0 || id > constructorInstances_.size()
             ? nullptr
             : &constructorInstances_[id - 1];
}

const LoweredDestructorInstance *
LoweredProgram::findDestructorInstance(HirDestructorInstanceId id) const {
  return id == 0 || id > destructorInstances_.size()
             ? nullptr
             : &destructorInstances_[id - 1];
}

const LoweredLambdaInstance *
LoweredProgram::findLambdaInstance(HirLambdaId id) const {
  return id == 0 || id > lambdaInstances_.size() ? nullptr
                                                 : &lambdaInstances_[id - 1];
}

const LoweredGeneratedItem *LoweredProgram::findGeneratedItem(
    const LoweredGeneratedItemIdentity &identity) const {
  return findGenerated(generatedItems_, identity);
}

LoweredProgramBuild LoweredProgramBuilder::build(
    const Program &program, const SemanticModel &semantics,
    const HirProgram &hir, const MirProgram &sourceMir,
    const MirProgram &optimizedMir, const TargetInfo &target) const {
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
      verifyMirOptimizationCoherence(sourceMir, optimizedMir);
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

  lowered.symbols_.reserve(semantics.database().symbols().size());
  for (const SymbolRecord &symbol : semantics.database().symbols()) {
    lowered.symbols_.push_back({.id = symbol.id,
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
                                .compilerPrivate = symbol.compilerPrivate});
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

  DerivedGeneratedGraph graph = deriveGeneratedGraph(optimizedMir);
  lowered.generatedItems_ = std::move(graph.items);
  for (auto &[address, roots] : graph.roots) {
    if (LoweredBody *body = findBody(lowered.bodies_, address)) {
      body->requiredGeneratedItems = std::move(roots);
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

std::vector<LoweredProgramIssue>
verifyLoweredProgram(const LoweredProgram &program) {
  std::vector<LoweredProgramIssue> issues;
  if (!program.target_.supported()) {
    addIssue(issues, LoweredProgramIssueKind::UnsupportedTarget,
             "lowered program has an unsupported target data layout");
  }
  if (program.target_.executionProfile != program.mir_.executionProfile()) {
    addIssue(issues, LoweredProgramIssueKind::FrontendMismatch,
             "lowered target and MIR execution profiles differ");
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
        symbolRequiresType(symbol.kind) && !resolvedType(symbol.type);
    if (symbol.id == 0 || symbol.name.empty() || unresolvedType ||
        !symbolIdentities.emplace(symbol.id, index).second) {
      addIssue(issues, LoweredProgramIssueKind::InvalidSymbolInventory,
               "lowered symbol has an absent/duplicate identity, empty name, "
               "or unresolved required type");
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
    const bool ownerValid =
        !instance.owner || (*instance.owner != 0 &&
                            *instance.owner <= program.classInstances_.size());
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.declaration != mir.declaration ||
        instance.owner != mir.owner ||
        instance.typeArguments != mir.typeArguments ||
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
    if (instance.id != index + 1 || instance.id != mir.id ||
        instance.owner != mir.owner || instance.owner == 0 ||
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

  const DerivedGeneratedGraph expected = deriveGeneratedGraph(program.mir_);
  if (program.generatedItems_ != expected.items) {
    addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
             "generated-item inventory differs from exact MIR-derived "
             "contracts");
  }
  for (const auto &[address, roots] : expected.roots) {
    const LoweredBody *body = findBody(program.bodies_, address);
    if (body == nullptr || body->requiredGeneratedItems != roots) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "body generated-item roots differ from exact MIR operations",
               address);
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
    if (findMirBody(program.mir_, item.sourceBody) == nullptr ||
        !std::is_sorted(item.dependencies.begin(), item.dependencies.end(),
                        generatedLess) ||
        std::adjacent_find(item.dependencies.begin(),
                           item.dependencies.end()) !=
            item.dependencies.end()) {
      addIssue(issues, LoweredProgramIssueKind::InvalidGeneratedItemInventory,
               "generated item source or dependency order is invalid",
               item.sourceBody, std::nullopt, item.identity);
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
               "generated item is not rooted by an executable body",
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
