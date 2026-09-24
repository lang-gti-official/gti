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
         !std::holds_alternative<ConstantCheckedIntegerResult>(*constant) &&
         !std::holds_alternative<ConstantAggregateValue>(*constant);
}

[[nodiscard]] bool containsUnresolvedGeneric(const CompileTimeValue &value) {
  return value.kind == CompileTimeValue::Parameter;
}

[[nodiscard]] bool containsUnresolvedGeneric(const SemanticType &type) {
  if (type.kind == SemanticType::TypeParameter ||
      (type.kind == SemanticType::TypePack && !type.concretePack) ||
      type.arrayLengthParameterId != 0 ||
      std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  }) ||
      std::any_of(type.lambdaEnclosingClassValues.begin(),
                  type.lambdaEnclosingClassValues.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  }) ||
      std::any_of(type.lambdaEnclosingFunctionValues.begin(),
                  type.lambdaEnclosingFunctionValues.end(),
                  [](const CompileTimeValue &value) {
                    return containsUnresolvedGeneric(value);
                  })) {
    return true;
  }
  const auto containsType = [](const SemanticType &argument) {
    return containsUnresolvedGeneric(argument);
  };
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     containsType) ||
         std::any_of(type.lambdaEnclosingClassTypes.begin(),
                     type.lambdaEnclosingClassTypes.end(), containsType) ||
         std::any_of(type.lambdaEnclosingFunctionTypes.begin(),
                     type.lambdaEnclosingFunctionTypes.end(), containsType);
}

[[nodiscard]] SemanticType referentOrSelf(const SemanticType &type) {
  return type.kind == SemanticType::Reference && type.arguments.size() == 1
             ? type.arguments.front()
             : type;
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
  for (const auto &instance : program.functionInstances()) {
    const auto *declaration = semantics.findFunction(instance.declaration);
    if (declaration == nullptr ||
        instance.requiresUnsafe != declaration->requiresUnsafe ||
        instance.cThreadOperation != declaration->cThreadOperation) {
      result.errors.emplace_back(
          "HIR function unsafe contract differs from its semantic declaration");
    }
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
        target->requiresUnsafe || target->linkage != LanguageLinkage::Gti ||
        declaration == nullptr || declaration->ownerClass != 0 ||
        returnType == nullptr || target->returnType != *returnType ||
        target->parameterTypes.size() != parameterTypes.size() ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameterTypes.begin()) ||
        adapter.cThreadCallback !=
            (program.executionProfile() == ExecutionProfile::Concurrent
                 ? adapter.type.cThreadCallbackKind()
                 : CThreadCallbackKind::None) ||
        (program.executionProfile() == ExecutionProfile::Concurrent &&
         adapter.cThreadCallback == CThreadCallbackKind::None)) {
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
          conversion->cThreadCallback != adapter->cThreadCallback ||
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

  const auto verifyFunctionItemValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.kind != HirValueKind::FunctionItem) {
        continue;
      }
      const FunctionItemSelectionInfo *selection =
          value.source == nullptr
              ? nullptr
              : semantics.findFunctionItemSelection(*value.source);
      const HirFunctionInstance *target =
          value.functionTarget
              ? program.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const FunctionInfo *declaration =
          target == nullptr ? nullptr
                            : semantics.findFunction(target->declaration);
      const SemanticType *returnType = value.info.type.functionItemReturnType();
      const std::span<const SemanticType> parameters =
          value.info.type.functionItemParameterTypes();
      if (selection == nullptr || target == nullptr || declaration == nullptr ||
          !value.operands.empty() ||
          value.info.category != ValueCategory::Value ||
          value.info.type != selection->type ||
          selection->capability != CallableInvocationCapability::Read ||
          value.info.type.functionId != target->declaration ||
          selection->function != target->declaration ||
          selection->declaration != target->source || target->owner ||
          target->linkage != LanguageLinkage::Gti || target->requiresUnsafe ||
          returnType == nullptr || *returnType != target->returnType ||
          parameters.size() != target->parameterTypes.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      target->parameterTypes.begin())) {
        result.errors.emplace_back(
            "function-item HIR value has no exact free-function identity");
      }
    }
  };
  verifyFunctionItemValues(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyFunctionItemValues(instance.fieldInitializers);
    verifyFunctionItemValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyFunctionItemValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyFunctionItemValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyFunctionItemValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyFunctionItemValues(instance.body);
  }

  std::vector<std::size_t> threadTaskRoots(program.threadTaskAdapters().size(),
                                           0);
  std::vector<SemanticType> threadTaskResults(
      program.threadTaskAdapters().size(), SemanticType::Unknown);
  const auto verifyThreadTaskValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.intrinsic != IntrinsicKind::ThreadSpawn) {
        if (value.threadTaskAdapter) {
          result.errors.emplace_back(
              "non-spawn HIR value carries a thread task adapter");
        }
        continue;
      }
      const HirThreadTaskAdapter *adapter =
          value.threadTaskAdapter
              ? program.findThreadTaskAdapter(*value.threadTaskAdapter)
              : nullptr;
      if (adapter == nullptr || adapter->spawnValue != value.id) {
        result.errors.emplace_back(
            "managed thread spawn has no exact HIR task adapter");
        continue;
      }
      const SemanticType *handleType =
          value.parameterTypes.size() == 2 &&
                  value.parameterTypes.front().kind ==
                      SemanticType::Reference &&
                  value.parameterTypes.front().arguments.size() == 1
              ? &value.parameterTypes.front().arguments.front()
              : nullptr;
      if (handleType == nullptr || handleType->kind != SemanticType::Class ||
          handleType->arguments.size() > 1) {
        result.errors.emplace_back(
            "managed thread spawn has no exact result handle type");
        continue;
      }
      const SemanticType taskResult = handleType->arguments.empty()
                                          ? SemanticType::Void
                                          : handleType->arguments.front();
      SemanticType &recorded = threadTaskResults[adapter->id - 1];
      if (recorded != SemanticType::Unknown && recorded != taskResult) {
        result.errors.emplace_back(
            "managed thread task adapter has conflicting result types");
        continue;
      }
      recorded = taskResult;
      ++threadTaskRoots[adapter->id - 1];
    }
  };
  verifyThreadTaskValues(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyThreadTaskValues(instance.fieldInitializers);
    verifyThreadTaskValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyThreadTaskValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyThreadTaskValues(instance.body);
  }

  for (std::size_t index = 0; index < program.threadTaskAdapters().size();
       ++index) {
    const HirThreadTaskAdapter &adapter = program.threadTaskAdapters()[index];
    const bool functionEntry = adapter.functionTarget.has_value();
    const bool lambdaEntry = adapter.lambdaTarget.has_value();
    bool exactEntry = functionEntry != lambdaEntry;
    if (functionEntry) {
      const HirFunctionInstance *target =
          program.findFunctionInstance(*adapter.functionTarget);
      const FunctionInfo *declaration =
          target == nullptr ? nullptr
                            : semantics.findFunction(target->declaration);
      const HirClassInstance *owner =
          target == nullptr || !target->owner ||
                  *target->owner > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[*target->owner - 1];
      const bool functionItemEntry = adapter.taskType.hasFunctionItemShape();
      exactEntry =
          exactEntry && target != nullptr && declaration != nullptr &&
          target->source != nullptr &&
          declaration->declaration == target->source &&
          target->returnType == threadTaskResults[index] &&
          target->parameterTypes.empty() &&
          (functionItemEntry
               ? owner == nullptr &&
                     adapter.taskType.functionId == target->declaration &&
                     adapter.targetInvocation ==
                         CallableInvocationCapability::Read
               : owner != nullptr && owner->type == adapter.taskType &&
                     target->source->operatorName().has_value() &&
                     target->source->operatorName()->kind ==
                         OverloadedOperator::Call &&
                     adapter.targetInvocation ==
                         callableInvocationCapability(
                             target->source->receiverMutability()));
    }
    if (lambdaEntry) {
      const HirLambda *target = program.findLambda(*adapter.lambdaTarget);
      exactEntry =
          exactEntry && target != nullptr && target->type == adapter.taskType &&
          target->returnType == threadTaskResults[index] &&
          target->parameterTypes.empty() &&
          adapter.targetInvocation == CallableInvocationCapability::Read;
    }
    if (adapter.id != index + 1 || adapter.spawnValue == 0 ||
        adapter.taskType == SemanticType::Unknown ||
        adapter.taskType.kind == SemanticType::TypeParameter ||
        threadTaskResults[index] == SemanticType::Unknown ||
        adapter.invocation != CallableInvocationCapability::Once ||
        program.executionProfile() != ExecutionProfile::Concurrent ||
        threadTaskRoots[index] != 1 || !exactEntry) {
      result.errors.emplace_back(
          "managed thread HIR task adapter contract is invalid");
    }
  }

  const auto verifyRangeIterations = [&](const HirBody &body) {
    for (const HirStatement &statement : body.statements) {
      const auto *source = stmtAs<RangeForStmt>(statement.source);
      if (statement.kind != HirStatementKind::RangeFor) {
        if (statement.rangeIteration) {
          result.errors.emplace_back(
              "non-range HIR statement carries range iteration metadata");
        }
        continue;
      }
      const ResolvedRangeIterationInfo *semantic =
          source == nullptr ? nullptr : semantics.findRangeIteration(*source);
      const HirRangeIteration *range =
          statement.rangeIteration ? &*statement.rangeIteration : nullptr;
      const auto bindingFor = [&](HirBindingId id) -> const HirBinding * {
        const auto found = std::find_if(
            body.bindings.begin(), body.bindings.end(),
            [&](const HirBinding &binding) { return binding.id == id; });
        return found == body.bindings.end() ? nullptr : &*found;
      };
      const HirBinding *owner =
          range == nullptr ? nullptr : bindingFor(range->owner);
      const auto exactBinding = [&](HirBindingId id,
                                    const VariableDecl *declaration) {
        const HirBinding *binding = bindingFor(id);
        return binding != nullptr && binding->variable == declaration;
      };
      const auto exactValue = [&](HirValueId id, const ExprPtr &expression) {
        const HirValue *value = body.findValue(id);
        return expression && value != nullptr &&
               value->source == expression.get();
      };
      const HirLoan *rangeLoan =
          range == nullptr ? nullptr : body.findLoan(range->rangeLoan);
      const HirLoan *elementLoan =
          range == nullptr ? nullptr : body.findLoan(range->elementLoan);
      const bool referenceElement =
          source != nullptr && source->elementBinding() != nullptr &&
          source->elementBinding()->type().reference.has_value();
      const HirBinding *rangeCarrier =
          range == nullptr ? nullptr : bindingFor(range->rangeCarrier);
      const HirBinding *iterator =
          range == nullptr ? nullptr : bindingFor(range->iterator);
      const HirBinding *sentinel =
          range == nullptr ? nullptr : bindingFor(range->sentinel);
      const HirBinding *element =
          range == nullptr ? nullptr : bindingFor(range->element);
      const bool exactConcreteTypes =
          range != nullptr && rangeCarrier != nullptr && iterator != nullptr &&
          sentinel != nullptr && element != nullptr &&
          range->rangeType == referentOrSelf(rangeCarrier->info.type) &&
          range->iteratorType == iterator->info.type &&
          range->sentinelType == sentinel->info.type &&
          range->elementType == referentOrSelf(element->info.type) &&
          range->elementAccess ==
              (element->info.type.kind == SemanticType::Reference
                   ? element->info.type.referenceAccess
                   : AccessMode::ReadOnly) &&
          !containsUnresolvedGeneric(range->rangeType) &&
          !containsUnresolvedGeneric(range->iteratorType) &&
          !containsUnresolvedGeneric(range->sentinelType) &&
          !containsUnresolvedGeneric(range->elementType);
      const bool genericSemantic =
          semantic != nullptr &&
          (containsUnresolvedGeneric(semantic->rangeType) ||
           containsUnresolvedGeneric(semantic->iteratorType) ||
           containsUnresolvedGeneric(semantic->sentinelType) ||
           containsUnresolvedGeneric(semantic->elementType));
      const bool exactLoanProvenance =
          semantic != nullptr && range != nullptr &&
          ((genericSemantic &&
            (range->rangeLoan != 0) == (semantic->rangeLoan != 0) &&
            (range->elementLoan != 0) == (semantic->elementLoan != 0) &&
            (range->rangeLoan == range->elementLoan) ==
                (semantic->rangeLoan == semantic->elementLoan)) ||
           (!genericSemantic && range->rangeLoan == semantic->rangeLoan &&
            range->elementLoan == semantic->elementLoan));
      const bool exactSemantic =
          semantic != nullptr && range != nullptr && exactConcreteTypes &&
          (genericSemantic || range->strategy == semantic->strategy) &&
          (genericSemantic || (range->rangeType == semantic->rangeType &&
                               range->iteratorType == semantic->iteratorType &&
                               range->sentinelType == semantic->sentinelType &&
                               range->elementType == semantic->elementType &&
                               range->arrayLength == semantic->arrayLength)) &&
          range->elementAccess == semantic->elementAccess &&
          exactLoanProvenance &&
          (genericSemantic ||
           (range->beginFunction == semantic->beginFunction &&
            range->endFunction == semantic->endFunction &&
            range->compareFunction == semantic->compareFunction &&
            range->dereferenceFunction == semantic->dereferenceFunction &&
            range->incrementFunction == semantic->incrementFunction)) &&
          range->ownsRangeTemporary == semantic->ownsRangeTemporary &&
          range->expectedPayloadProjection ==
              semantic->expectedPayloadProjection;
      const bool exactStructure =
          source != nullptr && range != nullptr && statement.body &&
          body.findStatement(*statement.body) != nullptr &&
          body.findStatement(range->coreLoop) != nullptr &&
          exactBinding(range->owner, source->ownerBinding()) &&
          exactBinding(range->rangeCarrier, source->rangeBinding()) &&
          exactBinding(range->iterator, source->iteratorBinding()) &&
          exactBinding(range->sentinel, source->sentinelBinding()) &&
          exactBinding(range->element, source->elementBinding()) &&
          exactValue(range->source, source->ownerBinding()->initializer()) &&
          exactValue(range->begin, source->iteratorBinding()->initializer()) &&
          exactValue(range->end, source->sentinelBinding()->initializer()) &&
          exactValue(range->comparison, source->coreLoop()->condition()) &&
          exactValue(range->elementAccessValue,
                     source->elementBinding()->initializer()) &&
          exactValue(range->increment, source->coreLoop()->increment());
      const bool exactLoans =
          rangeLoan != nullptr && rangeLoan->access == range->elementAccess &&
          std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                    range->rangeCarrier) != rangeLoan->carriers.end() &&
          (!referenceElement ||
           (elementLoan != nullptr &&
            elementLoan->access == range->elementAccess &&
            std::find(elementLoan->carriers.begin(),
                      elementLoan->carriers.end(),
                      range->element) != elementLoan->carriers.end() &&
            (range->elementAccess == AccessMode::ReadOnly ||
             (range->elementLoan != range->rangeLoan &&
              elementLoan->parent == range->rangeLoan)))) &&
          (range->elementAccess == AccessMode::ReadOnly ||
           range->strategy != RangeIterationStrategy::Structural ||
           std::find(rangeLoan->carriers.begin(), rangeLoan->carriers.end(),
                     range->iterator) != rangeLoan->carriers.end());
      const bool exactStrategy =
          range != nullptr &&
          ((range->strategy == RangeIterationStrategy::FixedArray &&
            range->rangeType.kind == SemanticType::Array &&
            range->rangeType.arrayLengthParameterId == 0 &&
            range->rangeType.arrayLength == range->arrayLength &&
            range->arrayLength != 0 &&
            body.findValue(range->elementAccessValue) != nullptr &&
            body.findValue(range->elementAccessValue)->kind ==
                HirValueKind::Index &&
            range->beginFunction == 0 && range->endFunction == 0) ||
           (range->strategy == RangeIterationStrategy::Structural &&
            range->beginFunction != 0 && range->endFunction != 0 &&
            range->compareFunction != 0 && range->dereferenceFunction != 0 &&
            range->incrementFunction != 0));
      const bool exactOwner =
          owner != nullptr &&
          (range == nullptr ||
           range->ownsRangeTemporary ==
               (owner->info.type.kind != SemanticType::Reference));
      const HirValue *sourceValue =
          range == nullptr ? nullptr : body.findValue(range->source);
      const bool exactExpectedPayloadProjection =
          range != nullptr &&
          (!range->expectedPayloadProjection ||
           (sourceValue != nullptr &&
            sourceValue->intrinsic == IntrinsicKind::ExpectedValue &&
            sourceValue->borrowOrigin == BorrowOriginKind::Receiver &&
            sourceValue->borrowAccess == AccessMode::ReadOnly &&
            owner != nullptr &&
            owner->info.type.kind == SemanticType::Reference));
      if (!exactSemantic || !exactStructure || !exactLoans || !exactStrategy ||
          !exactOwner || !exactExpectedPayloadProjection) {
        result.errors.emplace_back(
            "range HIR plan differs from semantic owner/loan/protocol facts");
      }
    }
  };
  verifyRangeIterations(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyRangeIterations(instance.fieldInitializers);
    verifyRangeIterations(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyRangeIterations(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyRangeIterations(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyRangeIterations(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyRangeIterations(instance.body);
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
  // The source-bearing ArrayInitializer rooting a (possibly nested)
  // empty-brace default expansion, reached by climbing the synthetic
  // levels. Every hop must be the exact single-consumer array shape.
  const auto defaultArrayRootInitializer =
      [&](const HirValue &value) -> const HirValue * {
    const HirValue *current = &value;
    for (std::size_t depth = 0; depth < program.module().values.size();
         ++depth) {
      const auto parents = valueParents.find(current->id);
      if (parents == valueParents.end() || parents->second.size() != 1) {
        return nullptr;
      }
      const HirValue &parent = *parents->second.front();
      if (parent.kind != HirValueKind::ArrayInitializer ||
          parent.info.type.kind != SemanticType::Array ||
          parent.info.type.arguments.size() != 1 ||
          parent.info.type.arguments.front() != current->info.type ||
          parent.info.type.arrayLengthParameterId != 0 ||
          parent.operands.size() != parent.info.type.arrayLength ||
          std::count(parent.operands.begin(), parent.operands.end(),
                     current->id) != 1) {
        return nullptr;
      }
      if (parent.source != nullptr) {
        return &parent;
      }
      current = &parent;
    }
    return nullptr;
  };
  const auto exactDefaultArrayElement = [&](const HirValue &value) {
    const auto parents = valueParents.find(value.id);
    if (value.source != nullptr ||
        value.kind != HirValueKind::DirectInitializer ||
        parents == valueParents.end() || parents->second.size() != 1) {
      return false;
    }
    const HirValue &parent = *parents->second.front();
    const HirValue *root = defaultArrayRootInitializer(value);
    const auto *initializer =
        root == nullptr ? nullptr : exprAs<ArrayInitializer>(root->source);
    const ResolvedConstructionInfo *construction =
        initializer == nullptr
            ? nullptr
            : semantics.findArrayElementConstruction(*initializer);
    const HirClassInstance *owner = nullptr;
    for (const HirClassInstance &candidate : program.classInstances()) {
      if (construction == nullptr ||
          candidate.type != construction->constructedType) {
        continue;
      }
      if (owner != nullptr) {
        return false;
      }
      owner = &candidate;
    }
    const HirConstructorInstance *target =
        value.constructorTarget
            ? program.findConstructorInstance(*value.constructorTarget)
            : nullptr;
    const HirClassInstance *targetOwner =
        target == nullptr || target->owner == 0 ||
                target->owner > program.classInstances().size()
            ? nullptr
            : &program.classInstances()[target->owner - 1];
    const bool exactTarget =
        construction != nullptr &&
        ((construction->generatedDefault && !value.constructorTarget) ||
         (!construction->generatedDefault && construction->constructor != 0 &&
          target != nullptr && targetOwner == owner &&
          target->declaration == construction->constructor));
    return construction != nullptr && owner != nullptr && exactTarget &&
           parent.kind == HirValueKind::ArrayInitializer &&
           parent.info.type.kind == SemanticType::Array &&
           parent.info.type.arguments.size() == 1 &&
           parent.info.type.arguments.front() == owner->type &&
           parent.info.type.arrayLengthParameterId == 0 &&
           parent.operands.size() == parent.info.type.arrayLength &&
           std::count(parent.operands.begin(), parent.operands.end(),
                      value.id) == 1 &&
           value.info == ExpressionInfo{.type = owner->type,
                                        .category = ValueCategory::Value,
                                        .access = AccessMode::ReadOnly,
                                        .traits = owner->traits} &&
           value.parameterTypes == construction->parameterTypes &&
           value.operands.size() == value.parameterTypes.size() &&
           value.constructorKind == construction->kind &&
           value.definedFailure.propagation ==
               (construction->constructor != 0 &&
                        !construction->generatedDefault
                    ? FailurePropagationKind::Constructor
                    : FailurePropagationKind::None);
  };
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
  // A synthetic nested level of an empty-brace fixed-array expansion: an
  // exact fixed-array value whose elements recurse toward the recorded leaf
  // construction of its rooted source initializer.
  const auto exactDefaultArrayLevel = [&](const HirValue &value) {
    if (value.source != nullptr ||
        value.kind != HirValueKind::ArrayInitializer ||
        value.info.type.kind != SemanticType::Array ||
        value.info.type.arguments.size() != 1 ||
        value.info.type.arrayLengthParameterId != 0 ||
        value.info.type.arrayLength == 0 ||
        value.operands.size() != value.info.type.arrayLength ||
        value.info.category != ValueCategory::Value ||
        value.info.access != AccessMode::ReadOnly ||
        value.unsafeOperation != UnsafeOperationKind::None ||
        !value.parameterTypes.empty() || value.operation || value.constant ||
        value.programConstantSubstitution ||
        value.intrinsic != IntrinsicKind::None ||
        value.synchronization != SynchronizationOperation{} ||
        !value.definedFailure.empty() ||
        value.borrowOrigin != BorrowOriginKind::None ||
        value.borrowArgument != 0 ||
        value.borrowAccess != AccessMode::ReadOnly || value.borrowPlace ||
        value.storedReferenceAccess || value.dispatch != CallDispatch::Static ||
        value.dispatchOwner != SemanticType::Unknown || value.receiver ||
        value.callPlan || value.packFoldSymbol != 0 ||
        value.packFoldParameter != 0 || value.packFoldFunction != 0 ||
        value.packFoldArgument != 0 || !value.packFoldElements.empty() ||
        !value.packExpansionElements.empty() || value.functionTarget ||
        value.nativeCallbackAdapter || value.constructorTarget ||
        value.constructorKind != ConstructorKind::Ordinary ||
        value.lambdaTarget || !value.callableArguments.empty() ||
        value.callableBoundary || value.callableInvocation || value.enumOwner ||
        value.enumValue || value.enumVariant || value.payloadIndex ||
        value.symbol != 0 || value.literal || value.place) {
      return false;
    }
    const HirValue *root = defaultArrayRootInitializer(value);
    const auto *initializer =
        root == nullptr ? nullptr : exprAs<ArrayInitializer>(root->source);
    return initializer != nullptr &&
           semantics.findArrayElementConstruction(*initializer) != nullptr;
  };
  const auto exactSyntheticValue = [&](const HirValue &value) {
    if (exactDefaultArrayElement(value) || exactDefaultArrayLevel(value)) {
      return true;
    }
    const auto parents = valueParents.find(value.id);
    if (!exactSyntheticCommon(value) || parents == valueParents.end() ||
        parents->second.size() != 1) {
      return false;
    }
    const HirValue &parent = *parents->second.front();
    if (value.kind == HirValueKind::Literal) {
      const auto *literal = exprAs<LiteralExpr>(parent.source);
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
    const auto *assignment = exprAs<Assign>(parent.source);
    const SymbolId symbol =
        assignment == nullptr ? 0 : semantics.findResolvedSymbol(*assignment);
    const SymbolRecord *record = semantics.database().findSymbol(symbol);
    const ResolvedOperatorInfo *operation =
        assignment == nullptr ? nullptr : semantics.findOperator(*assignment);
    const PlaceKey *semanticPlace =
        assignment == nullptr ? nullptr : semantics.findPlace(*assignment);
    if (assignment == nullptr || record == nullptr || operation == nullptr ||
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
           value.info == operation->receiverInfo && value.symbol == symbol &&
           !value.literal && value.place == exactPlace &&
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
