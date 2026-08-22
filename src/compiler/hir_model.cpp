#include "gti/hir.h"

#include <algorithm>
#include <unordered_set>

namespace lang {

namespace {

[[nodiscard]] bool sameSpan(const SourceSpan &left, const SourceSpan &right) {
  return left.source == right.source && left.start == right.start &&
         left.end == right.end && left.line == right.line;
}

[[nodiscard]] bool exactHostedFailure(const DefinedFailureOperation &operation,
                                      DefinedFailureCode code,
                                      DefinedFailureDetail detail,
                                      SourceUnitId sourceUnit,
                                      const SourceSpan &anchor) {
  if (operation.propagation != FailurePropagationKind::None ||
      operation.localOrigins.size() != 1) {
    return false;
  }
  const DefinedFailureOrigin &origin = operation.localOrigins.front();
  return origin.sourceUnit == sourceUnit && origin.start == anchor.start &&
         origin.end == anchor.end && origin.line == anchor.line &&
         origin.outcomes.size() == 1 &&
         origin.outcomes.front() ==
             DefinedFailureOutcome{.code = code, .detail = detail};
}

[[nodiscard]] bool
materializableProgramConstant(const std::optional<ConstantValue> &constant) {
  return constant &&
         !std::holds_alternative<ConstantCheckedIntegerResult>(*constant);
}

} // namespace

const HirValue *HirBody::findValue(HirValueId id) const {
  const auto found =
      std::find_if(values.begin(), values.end(),
                   [id](const HirValue &value) { return value.id == id; });
  return found == values.end() ? nullptr : &*found;
}

const HirStatement *HirBody::findStatement(HirStatementId id) const {
  const auto found = std::find_if(
      statements.begin(), statements.end(),
      [id](const HirStatement &statement) { return statement.id == id; });
  return found == statements.end() ? nullptr : &*found;
}

const HirLoan *HirBody::findLoan(SemanticLoanId id) const {
  const auto found =
      std::find_if(loans.begin(), loans.end(), [id](const HirLoan &loan) {
        return loan.semanticLoan == id;
      });
  return found == loans.end() ? nullptr : &*found;
}

std::size_t HirProgram::valueCount() const {
  std::size_t count = moduleBody.values.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.values.size();
    count += classInstance.staticFieldInitializers.values.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.values.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.values.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.values.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.values.size();
  }
  return count;
}

std::size_t HirProgram::statementCount() const {
  std::size_t count = moduleBody.statements.size();
  for (const HirClassInstance &classInstance : classes) {
    count += classInstance.fieldInitializers.statements.size();
    count += classInstance.staticFieldInitializers.statements.size();
  }
  for (const HirFunctionInstance &function : functions) {
    count += function.body.statements.size();
  }
  for (const HirConstructorInstance &constructor : constructors) {
    count += constructor.body.statements.size();
  }
  for (const HirDestructorInstance &destructor : destructors) {
    count += destructor.body.statements.size();
  }
  for (const HirLambda &lambda : lambdas) {
    count += lambda.body.statements.size();
  }
  return count;
}

const std::vector<HirValueId> &
HirProgram::valueIdsForSource(const Expr &source) const {
  static const std::vector<HirValueId> empty;
  const auto found = sourceValueIds.find(&source);
  return found == sourceValueIds.end() ? empty : found->second;
}

HirProgramPlanVerificationResult
verifyHirProgramPlans(const SemanticModel &semantics,
                      const HirProgram &program) {
  HirProgramPlanVerificationResult result;
  if (program.analysisSeal() != semantics.analysisSeal()) {
    result.errors.emplace_back(
        "HIR semantic-analysis seal differs from the semantic model");
  }
  for (std::size_t index = 0; index < program.nativeCallbackAdapters().size();
       ++index) {
    const HirNativeCallbackAdapter &adapter =
        program.nativeCallbackAdapters()[index];
    const HirFunctionInstance *target =
        program.findFunctionInstance(adapter.target);
    const FunctionInfo *declaration =
        target == nullptr ? nullptr
                          : semantics.findFunction(target->declaration);
    const SemanticType *returnType = adapter.type.nativeFunctionReturnType();
    const std::span<const SemanticType> parameterTypes =
        adapter.type.nativeFunctionParameterTypes();
    if (adapter.id != index + 1 || target == nullptr ||
        target->owner.has_value() || target->source == nullptr ||
        target->linkage != LanguageLinkage::Gti || declaration == nullptr ||
        declaration->ownerClass != 0 || returnType == nullptr ||
        target->returnType != *returnType ||
        target->parameterTypes.size() != parameterTypes.size() ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameterTypes.begin())) {
      result.errors.emplace_back(
          "native callback adapter has no exact free-function target");
    }
  }
  const auto verifyNativeCallbackValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.kind != HirValueKind::NativeCallback) {
        if (value.nativeCallbackAdapter) {
          result.errors.emplace_back(
              "non-callback HIR value carries callback adapter metadata");
        }
        continue;
      }
      const HirNativeCallbackAdapter *adapter =
          value.nativeCallbackAdapter
              ? program.findNativeCallbackAdapter(*value.nativeCallbackAdapter)
              : nullptr;
      const NativeFunctionConversionInfo *conversion =
          value.source == nullptr
              ? nullptr
              : semantics.findNativeFunctionConversion(*value.source);
      const HirFunctionInstance *target =
          adapter == nullptr ? nullptr
                             : program.findFunctionInstance(adapter->target);
      if (adapter == nullptr || conversion == nullptr || target == nullptr ||
          !value.operands.empty() || value.info.type != adapter->type ||
          conversion->type != adapter->type ||
          conversion->function != target->declaration ||
          conversion->declaration != target->source) {
        result.errors.emplace_back(
            "native callback HIR value differs from semantic conversion");
      }
    }
  };
  verifyNativeCallbackValues(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyNativeCallbackValues(instance.fieldInitializers);
    verifyNativeCallbackValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyNativeCallbackValues(instance.body);
  }
  const ProgramInitializationPlan &semanticInitialization =
      semantics.programInitializationPlan();
  const HirProgramInitializationPlan &initialization =
      program.programInitializationPlan();
  if (initialization.unitOrder != semanticInitialization.unitOrder) {
    result.errors.emplace_back(
        "program initialization source-unit order differs from semantics");
  }
  if (initialization.steps.size() != semanticInitialization.steps.size()) {
    result.errors.emplace_back(
        "program initialization step count differs from semantics");
  }
  std::vector<HirStatementId> expectedRoots;
  std::unordered_set<HirBindingId> expectedBindings;
  std::unordered_set<HirStatementId> expectedStatements;
  std::vector<HirValueId> initializerValues;
  const std::size_t stepCount = std::min(initialization.steps.size(),
                                         semanticInitialization.steps.size());
  for (std::size_t index = 0; index < stepCount; ++index) {
    const ProgramInitializationStep &semantic =
        semanticInitialization.steps[index];
    const HirProgramInitializationStep &step = initialization.steps[index];
    if (step.id != semantic.id || step.sourceUnit != semantic.sourceUnit ||
        step.kind != semantic.kind || step.role != semantic.role ||
        step.source != semantic.declaration || step.symbol != semantic.symbol ||
        step.requiresActiveCleanup != semantic.requiresActiveCleanup) {
      result.errors.emplace_back(
          "program initialization step identity differs from semantics");
    }
    const HirBinding *binding = nullptr;
    const auto bindingFound = std::find_if(
        program.module().bindings.begin(), program.module().bindings.end(),
        [&](const HirBinding &candidate) {
          return candidate.id == step.binding;
        });
    if (bindingFound != program.module().bindings.end()) {
      binding = &*bindingFound;
    }
    const BindingInfo *semanticBinding =
        semantic.declaration == nullptr
            ? nullptr
            : semantics.findBinding(*semantic.declaration);
    if (step.binding == 0 || binding == nullptr ||
        binding->variable != semantic.declaration ||
        semanticBinding == nullptr || binding->info != *semanticBinding ||
        binding->info.symbol != semantic.symbol ||
        !expectedBindings.insert(step.binding).second) {
      result.errors.emplace_back(
          "program initialization step has no exact module binding");
    }
    if (step.kind == ProgramStorageKind::NamespaceGlobal) {
      if (step.ownerClass != 0 || semantic.ownerClass != 0) {
        result.errors.emplace_back(
            "namespace-global initialization step has a class owner");
      }
    } else {
      const HirClassInstance *owner =
          step.ownerClass == 0 ||
                  step.ownerClass > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[step.ownerClass - 1];
      if (owner == nullptr || owner->declaration != semantic.ownerClass) {
        result.errors.emplace_back(
            "static-field initialization step has the wrong class instance");
      } else {
        const auto field =
            std::find_if(owner->staticFields.begin(), owner->staticFields.end(),
                         [&](const HirClassField &candidate) {
                           return candidate.declaration == semantic.declaration;
                         });
        if (field == owner->staticFields.end() || semanticBinding == nullptr ||
            field->binding != step.binding ||
            field->initializer != step.initializer ||
            field->info != *semanticBinding ||
            field->requiresActiveCleanup != semantic.requiresActiveCleanup) {
          result.errors.emplace_back(
              "static-field metadata differs from its module plan step");
        }
        if (!owner->staticFieldInitializers.bindings.empty() ||
            !owner->staticFieldInitializers.values.empty() ||
            !owner->staticFieldInitializers.statements.empty() ||
            !owner->staticFieldInitializers.roots.empty()) {
          result.errors.emplace_back(
              "planned static field is duplicated in a class initializer body");
        }
        if (owner->staticFieldInitializers.placeDomain.snapshot !=
                program.module().placeDomain.snapshot ||
            owner->staticFieldInitializers.placeDomain.body == 0 ||
            owner->staticFieldInitializers.placeDomain ==
                program.module().placeDomain) {
          result.errors.emplace_back("planned static field has no distinct "
                                     "coherent empty-body domain");
        }
      }
    }
    if (step.role == ProgramInitializationStepRole::DataOnly) {
      if (step.initializer || step.statement != 0) {
        result.errors.emplace_back(
            "data-only program storage has an executable HIR root");
      }
      continue;
    }
    expectedRoots.push_back(step.statement);
    expectedStatements.insert(step.statement);
    if (step.initializer) {
      initializerValues.push_back(*step.initializer);
    }
    const HirStatement *statement =
        program.module().findStatement(step.statement);
    const HirValue *initializer =
        step.initializer ? program.module().findValue(*step.initializer)
                         : nullptr;
    if (!step.initializer || initializer == nullptr ||
        initializer->source != semantic.declaration->initializer().get()) {
      result.errors.emplace_back(
          "executable program initializer has no exact source HIR value");
    }
    if (step.statement == 0 || statement == nullptr ||
        statement->kind != HirStatementKind::Variable ||
        statement->source != semantic.declaration ||
        statement->binding != step.binding ||
        statement->value != step.initializer) {
      result.errors.emplace_back(
          "executable program initializer has no exact module statement");
    }
  }
  if (program.module().roots != expectedRoots) {
    result.errors.emplace_back(
        "module roots do not exactly match executable initialization steps");
  }
  if (program.module().bindings.size() != expectedBindings.size() ||
      std::any_of(program.module().bindings.begin(),
                  program.module().bindings.end(),
                  [&](const HirBinding &binding) {
                    return !expectedBindings.contains(binding.id);
                  })) {
    result.errors.emplace_back(
        "module binding inventory contains unplanned program storage");
  }
  if (program.module().statements.size() != expectedStatements.size() ||
      std::any_of(program.module().statements.begin(),
                  program.module().statements.end(),
                  [&](const HirStatement &statement) {
                    return !expectedStatements.contains(statement.id);
                  })) {
    result.errors.emplace_back(
        "module statement inventory contains an unplanned initializer");
  }

  std::unordered_set<HirValueId> reachableValues;
  std::vector<HirValueId> pendingValues = initializerValues;
  while (!pendingValues.empty()) {
    const HirValueId id = pendingValues.back();
    pendingValues.pop_back();
    if (id == 0 || !reachableValues.insert(id).second) {
      continue;
    }
    const HirValue *value = program.module().findValue(id);
    if (value == nullptr) {
      result.errors.emplace_back(
          "module initializer references a missing HIR value");
      continue;
    }
    pendingValues.insert(pendingValues.end(), value->operands.begin(),
                         value->operands.end());
  }
  if (program.module().values.size() != reachableValues.size() ||
      std::any_of(program.module().values.begin(),
                  program.module().values.end(), [&](const HirValue &value) {
                    return !reachableValues.contains(value.id);
                  })) {
    result.errors.emplace_back(
        "module value inventory is not exactly reachable from planned "
        "initializer roots");
  }
  std::unordered_map<HirValueId, std::vector<const HirValue *>> valueParents;
  for (const HirValue &parent : program.module().values) {
    for (const HirValueId operand : parent.operands) {
      valueParents[operand].push_back(&parent);
    }
  }
  const auto exactSyntheticCommon = [](const HirValue &value) {
    return value.source == nullptr &&
           value.unsafeOperation == UnsafeOperationKind::None &&
           value.operands.empty() && value.parameterTypes.empty() &&
           !value.operation && !value.constant &&
           !value.programConstantSubstitution &&
           value.intrinsic == IntrinsicKind::None &&
           value.synchronization == SynchronizationOperation{} &&
           value.definedFailure.empty() &&
           value.borrowOrigin == BorrowOriginKind::None &&
           value.borrowArgument == 0 &&
           value.borrowAccess == AccessMode::ReadOnly && !value.borrowPlace &&
           !value.storedReferenceAccess &&
           value.dispatch == CallDispatch::Static &&
           value.dispatchOwner == SemanticType::Unknown && !value.receiver &&
           !value.callPlan && value.packFoldSymbol == 0 &&
           value.packFoldParameter == 0 && value.packFoldFunction == 0 &&
           value.packFoldArgument == 0 && value.packFoldElements.empty() &&
           value.packExpansionElements.empty() && !value.functionTarget &&
           !value.nativeCallbackAdapter && !value.constructorTarget &&
           value.constructorKind == ConstructorKind::Ordinary &&
           !value.lambdaTarget && value.callableArguments.empty() &&
           !value.callableBoundary && !value.callableInvocation &&
           !value.enumOwner && !value.enumValue && !value.enumVariant &&
           !value.payloadIndex && !value.ownership && !value.dropObligation;
  };
  const auto exactSyntheticValue = [&](const HirValue &value) {
    const auto parents = valueParents.find(value.id);
    if (!exactSyntheticCommon(value) || parents == valueParents.end() ||
        parents->second.size() != 1) {
      return false;
    }
    const HirValue &parent = *parents->second.front();
    if (value.kind == HirValueKind::Literal) {
      const auto *literal = dynamic_cast<const LiteralExpr *>(parent.source);
      return literal != nullptr && parent.constructorTarget &&
             std::holds_alternative<std::nullptr_t>(literal->value()) &&
             parent.parameterTypes ==
                 std::vector<SemanticType>{SemanticType::NullPtr} &&
             parent.operands == std::vector<HirValueId>{value.id} &&
             value.info == ExpressionInfo{.type = SemanticType::NullPtr,
                                          .category = ValueCategory::Value,
                                          .access = AccessMode::ReadOnly,
                                          .traits = semanticTraits(
                                              SemanticType::NullPtr)} &&
             value.symbol == 0 && value.literal &&
             std::holds_alternative<std::nullptr_t>(*value.literal) &&
             !value.place && value.fullExpression == parent.fullExpression;
    }
    const auto *assignment = dynamic_cast<const Assign *>(parent.source);
    const SymbolId symbol =
        assignment == nullptr ? 0 : semantics.findResolvedSymbol(*assignment);
    const SymbolRecord *record = semantics.database().findSymbol(symbol);
    const PlaceKey *semanticPlace =
        assignment == nullptr ? nullptr : semantics.findPlace(*assignment);
    if (assignment == nullptr || record == nullptr ||
        semanticPlace == nullptr || !parent.functionTarget ||
        parent.receiver != value.id || parent.operands.empty() ||
        parent.operands.front() != value.id) {
      return false;
    }
    PlaceKey exactPlace = *semanticPlace;
    exactPlace.domain = program.module().placeDomain;
    return value.kind == (assignment->path().segments.size() == 1
                              ? HirValueKind::Variable
                              : HirValueKind::QualifiedName) &&
           value.info == ExpressionInfo{.type = record->type,
                                        .category = ValueCategory::Place,
                                        .access = record->mutableBinding
                                                      ? AccessMode::Mutable
                                                      : AccessMode::ReadOnly,
                                        .traits = record->traits} &&
           value.symbol == symbol && !value.literal &&
           value.place == exactPlace &&
           value.fullExpression == parent.fullExpression;
  };
  for (const HirValue &value : program.module().values) {
    if (!reachableValues.contains(value.id)) {
      continue;
    }
    if (value.source == nullptr) {
      if (!exactSyntheticValue(value)) {
        result.errors.emplace_back(
            "source-less module initializer value is not an exact admitted "
            "lowerer-owned shape");
      }
      continue;
    }
    const ExpressionInfo *semanticInfo =
        semantics.findExpression(*value.source);
    const std::optional<ConstantValue> semanticConstant =
        semantics.findConstant(*value.source);
    const bool semanticSubstitution =
        semantics.isProgramConstantSubstitution(*value.source);
    if (semanticInfo == nullptr || value.info != *semanticInfo ||
        value.constant != semanticConstant ||
        value.programConstantSubstitution != semanticSubstitution ||
        (value.programConstantSubstitution &&
         !materializableProgramConstant(value.constant))) {
      result.errors.emplace_back(
          "module initializer value differs from exact semantic "
          "constant/expression provenance");
    }
  }

  const std::optional<HostedProgramEntryPlan> &semanticHosted =
      semantics.hostedProgramEntryPlan();
  const std::optional<HirHostedProgramEntryPlan> &hosted =
      program.hostedProgramEntryPlan();
  if (semanticHosted.has_value() != hosted.has_value()) {
    result.errors.emplace_back(
        "hosted program-entry plan presence differs from semantics");
    return result;
  }
  if (!semanticHosted) {
    return result;
  }
  if (hosted->semanticEntry != semanticHosted->entry ||
      hosted->semanticAppendFunction != semanticHosted->appendFunction ||
      hosted->semanticVectorConstructor != semanticHosted->vectorConstructor ||
      hosted->semanticStringConstructor != semanticHosted->stringConstructor ||
      hosted->kind != semanticHosted->kind ||
      hosted->sourceUnit != semanticHosted->sourceUnit ||
      !sameSpan(hosted->mainAnchor, semanticHosted->mainAnchor) ||
      hosted->validateCount != semanticHosted->validateCount ||
      hosted->convertCount != semanticHosted->convertCount) {
    result.errors.emplace_back(
        "hosted HIR provenance differs from the semantic entry plan");
  }
  const HirFunctionInstance *entry =
      program.findFunctionInstance(hosted->entry);
  const FunctionInfo *semanticEntry =
      semantics.findFunction(semanticHosted->entry);
  if (entry == nullptr || entry->declaration != semanticHosted->entry ||
      semanticEntry == nullptr || entry->source != semanticEntry->declaration ||
      entry->entryKind != semanticHosted->kind || entry->owner ||
      !entry->typeArguments.empty() || !entry->valueArguments.empty() ||
      entry->returnType != SemanticType::Int32 ||
      entry->parameterTypes != semanticEntry->parameterTypes ||
      entry->source == nullptr ||
      !sameSpan(hosted->mainAnchor, tokenSpan(entry->source->name()))) {
    result.errors.emplace_back(
        "hosted entry plan references the wrong HIR function instance");
  }
  if (semanticHosted->kind == ProgramEntryKind::NoArguments) {
    if (hosted->appendFunction != 0 || hosted->vectorConstructor != 0 ||
        hosted->stringConstructor != 0 ||
        (entry != nullptr && entry->entryArgumentAppendTarget) ||
        semanticEntry == nullptr || !semanticEntry->parameterTypes.empty() ||
        !hosted->validateCount.empty() || !hosted->convertCount.empty()) {
      result.errors.emplace_back(
          "no-argument hosted entry contains owned-argument operations");
    }
    return result;
  }
  const HirFunctionInstance *append =
      program.findFunctionInstance(hosted->appendFunction);
  const HirConstructorInstance *vectorConstructor =
      program.findConstructorInstance(hosted->vectorConstructor);
  const HirConstructorInstance *stringConstructor =
      program.findConstructorInstance(hosted->stringConstructor);
  const SemanticType vectorType =
      semanticEntry != nullptr && semanticEntry->parameterTypes.size() == 2
          ? semanticEntry->parameterTypes[1]
          : SemanticType::Unknown;
  const SemanticType stringType =
      vectorType.kind == SemanticType::Class && vectorType.arguments.size() == 1
          ? vectorType.arguments.front()
          : SemanticType::Unknown;
  const auto exactOwner = [&](HirClassInstanceId owner,
                              const SemanticType &type) {
    return owner != 0 && owner <= program.classInstances().size() &&
           program.classInstances()[owner - 1].type == type &&
           program.classInstances()[owner - 1].typeArguments ==
               type.arguments &&
           program.classInstances()[owner - 1].valueArguments ==
               type.valueArguments;
  };
  const bool exactFailures =
      exactHostedFailure(hosted->validateCount,
                         DefinedFailureCode::HostedRuntimeContractFailure,
                         DefinedFailureDetail::NegativeArgumentCount,
                         hosted->sourceUnit, hosted->mainAnchor) &&
      exactHostedFailure(hosted->convertCount,
                         DefinedFailureCode::NumericConversionOutOfRange,
                         DefinedFailureDetail::HostedArgumentCount,
                         hosted->sourceUnit, hosted->mainAnchor);
  const bool exactAppend =
      entry != nullptr && entry->entryArgumentAppendTarget &&
      *entry->entryArgumentAppendTarget == hosted->appendFunction &&
      append != nullptr &&
      append->declaration == semanticHosted->appendFunction && append->owner &&
      exactOwner(*append->owner, vectorType) && append->typeArguments.empty() &&
      append->valueArguments.empty() &&
      append->returnType == SemanticType::Void &&
      append->parameterTypes == std::vector<SemanticType>{stringType} &&
      !append->staticMember;
  const bool exactVectorConstructor =
      vectorConstructor != nullptr &&
      vectorConstructor->declaration == semanticHosted->vectorConstructor &&
      exactOwner(vectorConstructor->owner, vectorType) &&
      vectorConstructor->typeArguments == vectorType.arguments &&
      vectorConstructor->valueArguments == vectorType.valueArguments &&
      vectorConstructor->parameterTypes.empty();
  const bool exactStringConstructor =
      stringConstructor != nullptr &&
      stringConstructor->declaration == semanticHosted->stringConstructor &&
      exactOwner(stringConstructor->owner, stringType) &&
      stringConstructor->typeArguments == stringType.arguments &&
      stringConstructor->valueArguments == stringType.valueArguments &&
      stringConstructor->parameterTypes ==
          std::vector<SemanticType>{SemanticType::StringView};
  if (append == nullptr || vectorConstructor == nullptr ||
      stringConstructor == nullptr || vectorType.kind != SemanticType::Class ||
      stringType.kind != SemanticType::Class || !exactAppend ||
      !exactVectorConstructor || !exactStringConstructor || !exactFailures) {
    result.errors.emplace_back(
        "owned-argument hosted entry lacks exact HIR startup targets");
  }
  return result;
}

} // namespace lang
