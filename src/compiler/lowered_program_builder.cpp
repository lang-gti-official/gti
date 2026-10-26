#include "gti/lowered_program_builder.h"
#include "gti/mir_verifier.h"
#include "gti/support.h"

#include "gti/ast.h"
#include "gti/failure_metadata.h"
#include "gti/hir.h"
#include "gti/lowered_program_printer.h"
#include "gti/semantic_model.h"
#include "lowered_program_closure.h"
#include "mir_source_debug.h"
#include "mir_verification_support.h"

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
using lowered_detail::constructionSeal;
using lowered_detail::DerivedGeneratedGraph;
using lowered_detail::deriveGeneratedGraph;
using lowered_detail::findBody;
using lowered_detail::findDeclaration;

[[nodiscard]] const HirBody *debugHirBody(const HirProgram &hir,
                                          MirBodyAddress address) {
  switch (address.kind) {
  case MirBodyKind::ThreadLocalInitializer:
    for (const auto &storage : hir.threadLocalStorage())
      if (storage.symbol == address.owner)
        return &storage.initializer;
    return nullptr;
  case MirBodyKind::Module:
    return &hir.module();
  case MirBodyKind::Function:
    if (const auto *instance = hir.findFunctionInstance(address.owner))
      return &instance->body;
    break;
  case MirBodyKind::Constructor:
    if (const auto *instance = hir.findConstructorInstance(address.owner))
      return &instance->body;
    break;
  case MirBodyKind::Destructor:
    if (const auto *instance = hir.findDestructorInstance(address.owner))
      return &instance->body;
    break;
  case MirBodyKind::Lambda:
    if (const auto *instance = hir.findLambda(address.owner))
      return &instance->body;
    break;
  case MirBodyKind::FieldInitializers:
  case MirBodyKind::StaticFieldInitializers:
    if (address.owner != 0 && address.owner <= hir.classInstances().size()) {
      const auto &instance = hir.classInstances()[address.owner - 1];
      return address.kind == MirBodyKind::FieldInitializers
                 ? &instance.fieldInitializers
                 : &instance.staticFieldInitializers;
    }
    break;
  case MirBodyKind::HostedStartup:
    break;
  }
  return nullptr;
}

[[nodiscard]] std::vector<LoweredSourceLocation>
captureSourceLocations(const HirBody *hir, const MirBody &mir) {
  std::vector<LoweredSourceLocation> result;
  if (hir == nullptr)
    return result;
  std::unordered_map<HirStatementId, SourceSpan> statements;
  std::unordered_map<HirValueId, SourceSpan> values;
  for (const HirStatement &statement : hir->statements) {
    if (statement.source == nullptr || !statement.source->extent())
      continue;
    const SourceSpan &span = *statement.source->extent();
    if (span.source.empty() || span.source.front() == '<' ||
        span.start >= span.end || span.line < 1)
      continue;
    statements.emplace(statement.id, span);
    std::vector<HirValueId> pending;
    for (const auto root :
         {statement.value, statement.condition, statement.increment}) {
      if (root)
        pending.push_back(*root);
    }
    while (!pending.empty()) {
      const HirValueId id = pending.back();
      pending.pop_back();
      if (!values.emplace(id, span).second)
        continue;
      if (const HirValue *value = hir->findValue(id)) {
        pending.insert(pending.end(), value->operands.begin(),
                       value->operands.end());
        pending.insert(pending.end(), value->sequencedInputs.begin(),
                       value->sequencedInputs.end());
      }
    }
  }
  const auto source = [&](HirStatementId statement,
                          HirValueId value) -> const SourceSpan * {
    if (const auto found = statements.find(statement);
        found != statements.end())
      return &found->second;
    if (const auto found = values.find(value); found != values.end())
      return &found->second;
    return nullptr;
  };
  for (const MirBlock &block : mir.blocks) {
    for (const MirInstruction &instruction : block.instructions) {
      if (instruction.tombstone)
        continue;
      // Ownership/failure routing and transport scaffolding do not acquire a
      // user statement merely because they retain a provenance identity.
      switch (instruction.kind) {
      case MirInstructionKind::Compute:
      case MirInstructionKind::Load:
      case MirInstructionKind::Initialize:
      case MirInstructionKind::Assign:
      case MirInstructionKind::Modify:
      case MirInstructionKind::Call:
      case MirInstructionKind::Construct:
        if (const SourceSpan *span =
                source(instruction.hirStatement, instruction.hirValue))
          result.push_back({block.id, instruction.id, *span});
        break;
      default:
        break;
      }
    }
    const MirTerminator &terminator = block.terminator;
    if (terminator.kind == MirTerminatorKind::Return ||
        terminator.kind == MirTerminatorKind::Branch ||
        terminator.kind == MirTerminatorKind::Switch ||
        terminator.kind == MirTerminatorKind::Invoke) {
      if (const SourceSpan *span =
              source(terminator.hirStatement, terminator.hirValue))
        result.push_back({block.id, 0, *span});
    }
  }
  return result;
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
  case MirBodyKind::ThreadLocalInitializer:
    for (const auto &storage : hir.threadLocalStorage())
      if (storage.symbol == address.owner && storage.source != nullptr)
        return tokenSpan(storage.source->name());
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
        instance != nullptr) {
      if (instance->source != nullptr)
        return tokenSpan(instance->source->name());
      if (instance->generatedDefault) {
        const auto *owner = findHirClass(hir, instance->owner);
        if (owner != nullptr && owner->source != nullptr)
          return tokenSpan(owner->source->name());
      }
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
  case MirBodyKind::ThreadLocalInitializer:
    if (const auto *storage = mir.findThreadLocalStorage(address.owner);
        storage != nullptr && &storage->initializer == body) {
      result.definition = LoweredBodyDefinitionKind::Source;
      result.declaration = storage->symbol;
      return result;
    }
    return std::nullopt;
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
         .constraintConcept = parameter.conceptId,
         .typeArgumentPath = parameter.typeArgumentPath});
  }
  return result;
}

[[nodiscard]] std::vector<LoweredConceptRequirement>
lowerRequirements(const std::vector<AppliedConceptRequirement> &requirements) {
  std::vector<LoweredConceptRequirement> result;
  result.reserve(requirements.size());
  for (const AppliedConceptRequirement &requirement : requirements) {
    LoweredConceptRequirement lowered{.conceptId = requirement.conceptId,
                                      .arguments = requirement.arguments};
    if (requirement.syntax) {
      lowered.source = tokenSpan(requirement.syntax->name.last());
      if (requirement.syntax->predicate) {
        lowered.kind = LoweredRequirementKind::Predicate;
      } else if (const auto &query = requirement.syntax->memberRequirement) {
        lowered.source = tokenSpan(query->keyword());
        lowered.requiresUnsafe = query->unsafeKeyword().has_value();
        if (query->isExpressionRequirement()) {
          lowered.kind = LoweredRequirementKind::Expression;
          lowered.expressionParameterCount = query->parameters().size();
          lowered.resultArgument = query->parameters().size();
          if (query->inferredResult()) {
            lowered.kind = LoweredRequirementKind::InferredExpression;
            if (query->borrowedFrom())
              lowered.inferredResultAccess = query->result().referenceMutability
                                                 ? AccessMode::Mutable
                                                 : AccessMode::ReadOnly;
          }
          if (const auto *loop =
                  stmtAs<RangeForStmt>(query->iteration().get())) {
            lowered.kind = LoweredRequirementKind::Iteration;
            lowered.mutableReceiver =
                loop->bindingMutability() == Mutability::Mutable &&
                loop->bindingType().reference.has_value();
          }
          if (query->borrowedFrom()) {
            for (std::size_t index = 0; index < query->bindings().size();
                 ++index)
              if (query->bindings()[index].lexeme ==
                  query->borrowedFrom()->lexeme)
                lowered.resultOriginParameter = index;
          }
        } else {
          lowered.kind = LoweredRequirementKind::Member;
          lowered.memberName = query->member().lexeme;
          lowered.mutableReceiver =
              query->receiver().referenceMutability.has_value();
        }
      }
    }
    result.push_back(std::move(lowered));
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

[[nodiscard]] std::vector<LoweredParameter>
lowerLambdaParameters(const HirLambda &lambda) {
  std::vector<LoweredParameter> result;
  if (!lambda.source)
    return result;
  const auto &parameters = lambda.source->parameters();
  result.reserve(parameters.size());
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const auto &parameter = parameters[index];
    const auto binding =
        std::find_if(lambda.body.bindings.begin(), lambda.body.bindings.end(),
                     [&](const HirBinding &candidate) {
                       return index < lambda.parameterBindings.size() &&
                              candidate.id == lambda.parameterBindings[index] &&
                              candidate.parameter == &parameter;
                     });
    // Preserve the lexical binding's concrete pack, including an empty pack.
    // The flattened function signature is a separate ABI contract; indexing
    // it with lexical parameter positions drops tails or invents Unknown.
    const auto *info =
        binding == lambda.body.bindings.end() ? nullptr : &binding->info;
    result.push_back({.symbol = info ? info->symbol : 0,
                      .name = parameter.name.lexeme,
                      .type = info ? info->type : SemanticType::Unknown,
                      .access = info ? info->access : AccessMode::ReadOnly,
                      .mutability = parameter.mutability,
                      .pack = parameter.pack.has_value(),
                      .hasDefault = parameter.hasDefault(),
                      .explicitlyMoved = info && info->explicitlyMoved,
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
  DeclarationCollector(const SemanticModel &semantics, const TargetInfo &target,
                       const HirProgram &hir)
      : semantics_(semantics), target_(target), hir_(hir) {
    for (const HirClassInstance &instance : hir.classInstances()) {
      classes_.insert(instance.declaration);
    }
    for (const HirFunctionInstance &instance : hir.functionInstances()) {
      functions_.insert(instance.declaration);
    }
    for (const HirConstructorInstance &instance : hir.constructorInstances()) {
      constructors_.insert(instance.declaration);
    }
  }

  [[nodiscard]] std::vector<LoweredDeclaration>
  collect(const StmtList &declarations) {
    collectList(declarations, 0, 0, {});
    return std::move(result_);
  }

private:
  [[nodiscard]] bool nonGenericClass(ClassId id) const {
    const ClassTypeInfo *owner = semantics_.findClassType(id);
    return owner != nullptr && owner->genericParameters.empty();
  }

  // A non-generic class, function, or constructor has exactly one possible
  // concrete instance. Without one, HIR discovery from the program roots did
  // not reach it, so no executable code can use it and it is not lowered.
  // Generic declarations remain: retained templates can mention them.
  [[nodiscard]] bool
  unreachableConcreteDeclaration(const Stmt &statement) const {
    if (const auto *declaration = stmtAs<ClassDecl>(&statement)) {
      const ClassTypeInfo *info = semantics_.findClassType(*declaration);
      return info != nullptr && info->genericParameters.empty() &&
             !classes_.contains(info->id);
    }
    if (const auto *declaration = stmtAs<FunctionDecl>(&statement)) {
      const FunctionInfo *info = semantics_.findFunction(*declaration);
      return info != nullptr && info->genericParameters.empty() &&
             (info->ownerClass == 0 || nonGenericClass(info->ownerClass)) &&
             !functions_.contains(info->id);
    }
    if (const auto *declaration = stmtAs<ConstructorDecl>(&statement)) {
      const ConstructorInfo *info = semantics_.findConstructor(*declaration);
      return info != nullptr && info->genericParameters.empty() &&
             nonGenericClass(info->owner) && !constructors_.contains(info->id);
    }
    return false;
  }

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
    // The qualified body contributes executable syntax to its canonical
    // member declaration, not another namespace-level declaration row.
    if (const auto *definition = stmtAs<FunctionDecl>(&statement);
        definition != nullptr && definition->definitionOwner()) {
      return;
    }
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
    if (stmtAs<StaticAssertDecl>(&statement) != nullptr ||
        unreachableConcreteDeclaration(statement)) {
      return;
    }
    if (const auto *declaration = stmtAs<EnumDecl>(&statement)) {
      const EnumTypeInfo *primary = semantics_.findEnumType(*declaration);
      for (const HirEnum &instance : hir_.enumDeclarations()) {
        if (instance.source != declaration || primary == nullptr)
          continue;
        LoweredDeclaration row;
        row.id = result_.size() + 1;
        row.parent = parent;
        row.ownerClass = ownerClass;
        row.ordinal = ++nextOrdinal_[parent];
        row.namespaceScope = namespaceScope;
        row.kind = LoweredDeclarationKind::Enum;
        row.name = declaration->name().lexeme;
        row.source = tokenSpan(declaration->name());
        row.semanticIdentity = instance.declaration;
        LoweredEnumDeclaration payload{
            .id = instance.declaration,
            .type = instance.type,
            .sourceUnit = instance.sourceUnit,
            .qualifiedName = instance.qualifiedName,
            .genericParameters =
                lowerGenericParameters(primary->genericParameters),
            .underlyingType = instance.underlyingType,
            .traits = instance.traits,
            .requiresActiveCleanup = instance.requiresActiveCleanup,
            .payload = instance.payload,
            .compilerPrivate = primary->compilerPrivate};
        for (const HirEnumerator &variant : instance.enumerators) {
          payload.enumerators.push_back(
              {.name = variant.source->name.lexeme,
               .value = variant.value,
               .variantIndex = variant.variantIndex,
               .payloadTypes = variant.payloadTypes,
               .payloadRequiresActiveCleanup =
                   variant.payloadRequiresActiveCleanup,
               .source = tokenSpan(variant.source->name)});
        }
        row.payload = std::move(payload);
        result_.push_back(std::move(row));
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
        row.semanticIdentity = info->id;
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
        row.generic = !info->genericParameters.empty();
        row.semanticIdentity = info->id;
        // Namespace scope is separate from the lexical class prefix in the
        // semantic qualified name. Nested class ownership is already retained
        // by parent/ownerClass; it must not become a namespace in a backend.
        // A namespace-qualified exact specialization can be written outside
        // that namespace; retain its resolved namespace rather than its AST
        // placement. Only lexical class owners need the enclosing prefix split.
        row.namespaceScope =
            info->enclosingClass == 0 ? info->namespaceScope : namespaceScope;
        childNamespace = row.namespaceScope;
        childOwner = info->id;
        LoweredClassDeclaration payload{
            .id = info->id,
            .sourceUnit = info->sourceUnit,
            .qualifiedName = info->qualifiedName,
            .kind = info->kind,
            .definitionFamilyPrimary = info->definitionFamilyPrimary,
            .exactSpecializationPrimary = info->exactSpecializationPrimary,
            .exactTypeArguments = info->exactTypeArguments,
            .exactValueArguments = info->exactValueArguments,
            .genericParameters =
                lowerGenericParameters(info->genericParameters),
            .genericArgumentPatterns = info->genericArgumentPatterns,
            .traits = info->traits,
            .transferPolicy = info->transferPolicy,
            .sharePolicy = info->sharePolicy,
            .transferCondition = info->transferCondition,
            .shareCondition = info->shareCondition,
            .cAbiLayout = lowerCAbiLayout(*info, semantics_),
            .unionLayout = lowerUnionLayout(*info, semantics_),
            .compilerCapability = info->compilerCapability,
            .forwardDeclaration = declaration->isForwardDeclaration(),
            .abstract = info->abstract,
            .polymorphic = info->polymorphic,
            .cAbiRecord = info->cAbiRecord,
            .cOpaqueHandle = info->cOpaqueHandle,
            .compilerPrivate = info->compilerPrivate,
            .reflectPrivate = info->reflectPrivate,
            .computedTypeProjection = info->computedTypeProjection};
        payload.bases.reserve(info->bases.size());
        for (std::size_t index = 0; index < info->bases.size(); ++index) {
          payload.bases.push_back({.type = info->bases[index].type,
                                   .access = baseAccess(*declaration, index),
                                   .interface = info->bases[index].interface});
        }
        if (const ClassLifecycleInfo *lifecycle =
                semantics_.findClassLifecycle(*declaration)) {
          payload.defaultConstructor = lifecycle->defaultConstructor;
          payload.generatedDefaultRequiresSchedule =
              lifecycle->generatedDefaultInitializers;
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
                              : info->bodyDeclaration()->body() != nullptr
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
            .requiresUnsafe = info->requiresUnsafe,
            .neverReturns = info->neverReturns,
            .tracksCaller = info->tracksCaller,
            .threadConfinedCallback = info->threadConfinedCallback,
            .cThreadOperation = info->cThreadOperation,
            .processMemoryOperation = info->processMemoryOperation,
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
            .requiresUnsafe = info->requiresUnsafe,
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
            .externalSymbol = semantics_.findForeignStorage(info->symbol)
                                  ? semantics_.findForeignStorage(info->symbol)
                                        ->externalSymbol
                                  : std::string{},
            .ownerClass = ownerClass,
            .type = info->type,
            .access = info->access,
            .traits = info->traits,
            .constant = info->constant,
            .mutability = declaration->mutability(),
            .staticStorage = info->staticStorage,
            .threadLocalStorage = info->threadLocal,
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
    if (stmtAs<ClassDecl>(&statement) != nullptr) {
      for (const auto &constructor : semantics_.designatedConstructors) {
        const auto *info = semantics_.findConstructor(*constructor);
        if (info != nullptr && info->owner == childOwner) {
          collectStatement(*constructor, id, childOwner, childNamespace);
        }
      }
    }
  }

  const SemanticModel &semantics_;
  const TargetInfo &target_;
  const HirProgram &hir_;
  std::unordered_set<ClassId> classes_;
  std::unordered_set<FunctionId> functions_;
  std::unordered_set<ConstructorId> constructors_;
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
      semantics.programKind() != hir.programKind() ||
      hir.programKind() != mir.programKind() ||
      semantics.foreignStorageDeclarations() !=
          hir.foreignStorageDeclarations() ||
      hir.foreignStorageDeclarations() != mir.foreignStorageDeclarations() ||
      hir.embeddingPlan().has_value() != mir.embeddingPlan().has_value() ||
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
  if (hir.embeddingPlan() && mir.embeddingPlan()) {
    if (hir.embeddingPlan()->abiVersion != mir.embeddingPlan()->abiVersion ||
        hir.embeddingPlan()->exports.size() !=
            mir.embeddingPlan()->exports.size()) {
      return reject("embedding plan differs between HIR and MIR");
    }
    for (std::size_t index = 0; index < hir.embeddingPlan()->exports.size();
         ++index) {
      const HirEmbeddingExport &source = hir.embeddingPlan()->exports[index];
      const MirEmbeddingExport &lowered = mir.embeddingPlan()->exports[index];
      if (source.semanticFunction != lowered.declaration ||
          source.function != lowered.function ||
          source.qualifiedName != lowered.qualifiedName ||
          source.symbol != lowered.symbol ||
          source.returnType != lowered.returnType ||
          source.parameterTypes != lowered.parameterTypes ||
          source.requestSource != lowered.requestSource) {
        return reject("embedding export " + std::to_string(index + 1) +
                      " differs between HIR and MIR");
      }
    }
  }
  for (const HirEnum &source : hir.enumDeclarations()) {
    if (!source.payload) {
      continue;
    }
    const MirPayloadEnum *lowered = mir.findPayloadEnum(source.type);
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
        source.traits != lowered.traits ||
        source.generatedDefaultRequiresSchedule !=
            lowered.generatedDefaultRequiresSchedule ||
        source.cOpaqueHandle != lowered.cOpaqueHandle ||
        source.fieldInitializers.placeDomain !=
            lowered.fieldInitializers.placeDomain ||
        source.staticFieldInitializers.placeDomain !=
            lowered.staticFieldInitializers.placeDomain ||
        info == nullptr || info->id != source.declaration ||
        info->declaration != source.source ||
        info->sourceUnit != source.sourceUnit ||
        info->cOpaqueHandle != source.cOpaqueHandle) {
      return reject("class instance " + std::to_string(index + 1) + " differs");
    }
    if (source.typeAliases.size() != lowered.typeAliases.size())
      return reject("class alias inventory differs");
    for (std::size_t alias = 0; alias < source.typeAliases.size(); ++alias) {
      if (source.typeAliases[alias].declaration !=
              lowered.typeAliases[alias].declaration ||
          source.typeAliases[alias].type != lowered.typeAliases[alias].type)
        return reject("class alias identity or concrete type differs");
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
        source.requiresUnsafe != lowered.requiresUnsafe ||
        source.neverReturns != lowered.neverReturns ||
        source.tracksCaller != lowered.tracksCaller ||
        source.threadConfinedCallback != lowered.threadConfinedCallback ||
        source.cThreadOperation != lowered.cThreadOperation ||
        (info != nullptr &&
         source.cThreadOperation != info->cThreadOperation) ||
        source.processMemoryOperation != lowered.processMemoryOperation ||
        (info != nullptr &&
         source.processMemoryOperation != info->processMemoryOperation) ||
        (info != nullptr && source.requiresUnsafe != info->requiresUnsafe) ||
        (info != nullptr && source.neverReturns != info->neverReturns) ||
        (info != nullptr && source.tracksCaller != info->tracksCaller) ||
        (info != nullptr &&
         source.threadConfinedCallback != info->threadConfinedCallback) ||
        source.linkage != lowered.linkage ||
        source.externalSymbol != lowered.externalSymbol ||
        source.body.placeDomain != lowered.body.placeDomain ||
        lowered.definitionKind != expectedDefinition || info == nullptr ||
        info->id != source.declaration ||
        info->bodyDeclaration() != source.source ||
        info->sourceUnit != source.sourceUnit) {
      return reject("function instance " + std::to_string(index + 1) +
                    " differs");
    }
  }
  for (std::size_t index = 0; index < hir.constructorInstances().size();
       ++index) {
    const HirConstructorInstance &source = hir.constructorInstances()[index];
    const MirConstructorInstance &lowered = mir.constructorInstances()[index];
    const HirClassInstance *owner = findHirClass(hir, source.owner);
    if (source.generatedDefault &&
        (source.source != nullptr || source.declaration != 0 ||
         !source.parameterTypes.empty() || owner == nullptr ||
         owner->defaultConstructor != SpecialMemberStatus::Generated ||
         !source.parameterBindings.empty() || !source.body.roots.empty() ||
         lowered.definitionKind != MirDefinitionKind::Source))
      return reject("generated constructor instance " +
                    std::to_string(index + 1) +
                    " has an invalid structural schedule");
    // A generated constructor has no user body roots, but its selected field
    // initializers may own nested result-region statements and locals.
    // verifyHirProgramPlans independently proves their exact reachable
    // inventory; an empty-statements check would reject that verified schedule.
    if (source.id != lowered.id || source.owner != lowered.owner ||
        source.generatedDefault != lowered.generatedDefault ||
        source.constexprConstructor != lowered.constexprConstructor ||
        source.requiresUnsafe != lowered.requiresUnsafe ||
        source.requiresUnsafe != (source.source != nullptr &&
                                  source.source->unsafeKeyword().has_value()) ||
        source.constexprConstructor !=
            (source.source != nullptr && source.source->isConstexpr()) ||
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
        source.type != lowered.type ||
        source.cThreadCallback != lowered.cThreadCallback) {
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
        source.invocation != lowered.invocation ||
        source.scoped != lowered.scoped ||
        source.scopeArgument != lowered.scopeArgument) {
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
    const TargetInfo &target, bool captureDebugLocations,
    LoweredProgramVerifiedInputs verified) const {
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
  const HirProgramPlanVerificationResult hirPlans = [&] {
    const PhaseTimeScope timeScope("lowered-verify-hir-plans");
    return verifyHirProgramPlans(semantics, hir);
  }();
  if (!hirPlans.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             hirPlans.errors.empty() ? "semantic and HIR program plans differ"
                                     : hirPlans.errors.front());
  }
  if (sourceMir.sourceDebugProfile() != hir.sourceDebugProfile() ||
      optimizedMir.sourceDebugProfile() != sourceMir.sourceDebugProfile())
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             "MIR source-debug profile differs from its frontend request");
  for (const auto address : enumerateMirBodyAddresses(sourceMir)) {
    const auto *sourceBody = debugHirBody(hir, address);
    const auto *mirBody = findMirBody(sourceMir, address);
    if (sourceBody && mirBody &&
        !mir_detail::sourceDebugMatchesHir(*sourceBody, *mirBody))
      addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
               "source MIR debug facts differ from exact accepted HIR facts");
  }
  const MirVerificationResult sourceVerification = [&] {
    if (verified.sourceMirVerified)
      return MirVerificationResult{};
    const PhaseTimeScope timeScope("lowered-verify-source-mir");
    return verifyMirProgram(sourceMir);
  }();
  if (!sourceMir.valid() || !sourceVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidSourceMir,
             sourceVerification.errors.empty()
                 ? "source MIR is not marked valid"
                 : sourceVerification.errors.front().message);
  }
  // Verification depends only on program content, and an unoptimized
  // program is identical to its source.
  const MirVerificationResult optimizedVerification = [&] {
    const PhaseTimeScope timeScope("lowered-verify-optimized-mir");
    return optimizedMir == sourceMir ? sourceVerification
                                     : verifyMirProgram(optimizedMir);
  }();
  if (!optimizedMir.valid() || !optimizedVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimizedMir,
             optimizedVerification.errors.empty()
                 ? "optimized MIR is not marked valid"
                 : optimizedVerification.errors.front().message);
  }
  const FailureMetadataVerificationResult failureVerification = [&] {
    const PhaseTimeScope timeScope("lowered-verify-failure-metadata");
    return verifyFailureMetadata(optimizedMir.failureMetadata());
  }();
  if (!failureVerification.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidFailureMetadata,
             failureVerification.errors.empty()
                 ? "optimized MIR failure metadata is invalid"
                 : failureVerification.errors.front());
  }
  const MirVerificationResult optimization = [&] {
    if (verified.optimizationCoherenceVerified)
      return MirVerificationResult{};
    const PhaseTimeScope timeScope("lowered-verify-optimization-coherence");
    return mir_detail::verifyVerifiedMirOptimizationCoherence(
        sourceMir, optimizedMir, transformations, sourceVerification.valid(),
        optimizedVerification.valid());
  }();
  if (!optimization.valid()) {
    addIssue(build.issues, LoweredProgramIssueKind::InvalidOptimization,
             optimization.errors.empty()
                 ? "optimized MIR differs from the source MIR contract"
                 : optimization.errors.front().message);
  }
  std::string frontendMismatch;
  const bool frontendMatches = [&] {
    const PhaseTimeScope timeScope("lowered-verify-frontend-instances");
    return frontendInstancesMatch(semantics, hir, sourceMir, frontendMismatch);
  }();
  if (!frontendMatches) {
    addIssue(build.issues, LoweredProgramIssueKind::FrontendMismatch,
             std::move(frontendMismatch));
  }
  if (!build.issues.empty()) {
    return build;
  }

  LoweredProgram lowered;
  lowered.target_ = target;
  lowered.mir_ = optimizedMir;
  lowered.programKind_ = optimizedMir.programKind();
  if (const std::optional<MirEmbeddingPlan> &embedding =
          optimizedMir.embeddingPlan()) {
    LoweredEmbeddingPlan plan{.abiVersion = embedding->abiVersion};
    plan.exports.reserve(embedding->exports.size());
    for (const MirEmbeddingExport &selected : embedding->exports) {
      plan.exports.push_back({.declaration = selected.declaration,
                              .function = selected.function,
                              .qualifiedName = selected.qualifiedName,
                              .symbol = selected.symbol,
                              .returnType = selected.returnType,
                              .parameterTypes = selected.parameterTypes,
                              .requestSource = selected.requestSource});
    }
    lowered.embeddingPlan_ = std::move(plan);
  }
  const PhaseTimeScope collectScope("lowered-collect");
  lowered.declarations_ = DeclarationCollector(semantics, target, hir)
                              .collect(program.declarations());
  for (LoweredDeclaration &declaration : lowered.declarations_) {
    auto *enumeration =
        std::get_if<LoweredEnumDeclaration>(&declaration.payload);
    if (enumeration == nullptr || !enumeration->payload) {
      continue;
    }
    const auto hirEnumeration = std::find_if(
        hir.enumDeclarations().begin(), hir.enumDeclarations().end(),
        [&](const HirEnum &candidate) {
          return candidate.type == enumeration->type;
        });
    const MirPayloadEnum *mirEnumeration =
        optimizedMir.findPayloadEnum(enumeration->type);
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

  lowered.symbols_.reserve(semantics.database().symbols().size() +
                           hir.instanceSymbols().size());
  const auto retainSymbol = [&](const SymbolRecord &symbol) {
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
  };
  for (const SymbolRecord &symbol : semantics.database().symbols()) {
    // Source unit 0 marks an identity reserved by late demand; it names no
    // declaration.
    if (symbol.sourceUnit != 0)
      retainSymbol(symbol);
  }
  for (const SymbolRecord &symbol : hir.instanceSymbols()) {
    retainSymbol(symbol);
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
        .parameters = lowerLambdaParameters(instance),
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
    if (captureDebugLocations) {
      lowered.bodies_.back().sourceLocations = captureSourceLocations(
          debugHirBody(hir, address), *findMirBody(optimizedMir, address));
    }
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

  // Checking a seal immediately after computing it from the same value would
  // print the whole program a second time without testing anything.
  lowered.indexDeclarations();
  build.issues = [&] {
    const PhaseTimeScope timeScope("lowered-verify-contents");
    return lowered_detail::verifyLoweredProgramContents(lowered);
  }();
  if (build.issues.empty()) {
    lowered.constructionSeal_ = constructionSeal(lowered);
    build.program.emplace(std::move(lowered));
  }
  return build;
}
} // namespace lang
