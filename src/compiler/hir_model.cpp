#include "gti/hir.h"

#include <algorithm>
#include <unordered_set>

namespace lang {

const HirValue *HirBody::valueForSource(const Expr &source) const {
  for (auto value = values.rbegin(); value != values.rend(); ++value)
    if (value->source == &source)
      return &*value;
  for (auto alias = directInitializations.rbegin();
       alias != directInitializations.rend(); ++alias)
    if (alias->source == &source)
      return findValue(alias->value);
  for (auto alias = sourceAliases.rbegin(); alias != sourceAliases.rend();
       ++alias)
    if (alias->source == &source)
      return findValue(alias->value);
  return nullptr;
}

const HirValue *HirBody::initializerValueForSource(const Expr &source) const {
  for (const HirContextualConstruction &construction : contextualConstructions)
    if (construction.source == &source)
      return findValue(construction.value);
  return valueForSource(source);
}

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

// The call a contextual conversion adds after the value it converts: both
// share one source, and the call's receiver is that value. The source's own
// records describe the converted value, not this call.
[[nodiscard]] bool contextualConversionCall(const HirBody &body,
                                            const HirValue &value,
                                            const SemanticModel &semantics) {
  if (value.kind != HirValueKind::Call || !value.functionTarget ||
      !value.receiver || value.source == nullptr ||
      semantics.findContextualConversion(*value.source) == nullptr) {
    return false;
  }
  const HirValue *converted = body.findValue(*value.receiver);
  return converted != nullptr && converted->source == value.source;
}

[[nodiscard]] bool
materializableProgramConstant(const std::optional<ConstantValue> &constant) {
  return constant && !std::holds_alternative<ConstantAggregateValue>(*constant);
}

[[nodiscard]] bool containsUnresolvedGeneric(const CompileTimeValue &value) {
  return value.kind != CompileTimeValue::UInt64;
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
  std::unordered_map<SymbolId, const SymbolRecord *> instanceSymbols;
  for (const SymbolRecord &symbol : program.instanceSymbols()) {
    if (symbol.id == 0 ||
        semantics.database().findSymbol(symbol.id) != nullptr ||
        !instanceSymbols.emplace(symbol.id, &symbol).second ||
        symbol.name.empty() || symbol.sourceUnit == 0 ||
        symbol.nameSpan.end <= symbol.nameSpan.start) {
      result.errors.emplace_back(
          "HIR concrete symbol inventory has invalid or overlapping identity");
    }
  }
  const auto verifyConcreteBindings = [&](const HirBody &body) {
    for (const HirBinding &binding : body.bindings) {
      const auto found = instanceSymbols.find(binding.info.symbol);
      if (found != instanceSymbols.end() &&
          (found->second->type != binding.info.type ||
           found->second->traits != binding.info.traits ||
           found->second->mutableBinding !=
               (binding.info.access == AccessMode::Mutable))) {
        result.errors.emplace_back(
            "HIR concrete binding differs from its semantic symbol record");
      }
    }
  };
  verifyConcreteBindings(program.module());
  for (const auto &instance : program.functionInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.constructorInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.destructorInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.lambdaInstances())
    verifyConcreteBindings(instance.body);
  for (const auto &instance : program.classInstances()) {
    const ClassTypeInfo *owner = semantics.findClassType(instance.declaration);
    if (owner == nullptr ||
        owner->typeAliases.size() != instance.typeAliases.size()) {
      result.errors.emplace_back(
          "HIR class alias inventory differs from semantics");
      continue;
    }
    GenericSubstitution substitution;
    std::size_t typeIndex = 0, valueIndex = 0;
    for (const auto &parameter : owner->genericParameters) {
      if (!parameter.typeArgumentPath.empty()) {
        if (auto type = projectGenericTypeArgument(
                instance.typeArguments, parameter.typeArgumentPath,
                parameter.id, parameter.pack))
          substitution.types.emplace(parameter.id, std::move(*type));
      } else if (parameter.value) {
        if (valueIndex < instance.valueArguments.size())
          substitution.values.emplace(parameter.id,
                                      instance.valueArguments[valueIndex++]);
      } else if (parameter.pack) {
        std::vector<SemanticType> elements(
            instance.typeArguments.begin() +
                static_cast<std::ptrdiff_t>(typeIndex),
            instance.typeArguments.end());
        substitution.types.emplace(
            parameter.id,
            SemanticType::concreteTypePack(parameter.id, std::move(elements)));
        typeIndex = instance.typeArguments.size();
      } else if (typeIndex < instance.typeArguments.size()) {
        substitution.types.emplace(parameter.id,
                                   instance.typeArguments[typeIndex++]);
      }
    }
    for (std::size_t index = 0; index < instance.typeAliases.size(); ++index) {
      const auto &alias = instance.typeAliases[index];
      const TypeAliasInfo *primary =
          semantics.findTypeAlias(*owner->typeAliases[index]);
      if (primary == nullptr || alias.source != owner->typeAliases[index] ||
          alias.declaration != primary->id || alias.declaration == 0 ||
          containsUnresolvedGeneric(alias.type) ||
          alias.type !=
              substituteSemanticType(primary->type, substitution, &semantics))
        result.errors.emplace_back(
            "HIR class alias differs from its exact semantic owner/type");
    }
  }
  if (program.analysisSeal() != semantics.analysisSeal()) {
    result.errors.emplace_back(
        "HIR semantic-analysis seal differs from the semantic model");
  }
  if (program.programKind() != semantics.programKind()) {
    result.errors.emplace_back(
        "HIR program kind differs from the semantic model");
  }
  const std::optional<SemanticEmbeddingPlan> &semanticEmbedding =
      semantics.embeddingPlan();
  const std::optional<HirEmbeddingPlan> &embedding = program.embeddingPlan();
  if (semanticEmbedding.has_value() != embedding.has_value()) {
    result.errors.emplace_back(
        "embedding plan presence differs from semantics");
  } else if (semanticEmbedding && embedding) {
    if (program.programKind() != ProgramKind::Library ||
        embedding->abiVersion != embeddingAbiVersionV1 ||
        embedding->abiVersion != semanticEmbedding->abiVersion ||
        embedding->exports.empty() ||
        embedding->exports.size() != semanticEmbedding->exports.size()) {
      result.errors.emplace_back(
          "embedding HIR plan has invalid program or ABI provenance");
    }
    std::string previousSymbol;
    for (std::size_t index = 0; index < embedding->exports.size() &&
                                index < semanticEmbedding->exports.size();
         ++index) {
      const HirEmbeddingExport &lowered = embedding->exports[index];
      const SemanticEmbeddingExport &semantic =
          semanticEmbedding->exports[index];
      const HirFunctionInstance *instance =
          program.findFunctionInstance(lowered.function);
      const FunctionInfo *declaration =
          semantics.findFunction(lowered.semanticFunction);
      if ((index != 0 && previousSymbol >= lowered.symbol) ||
          lowered.semanticFunction != semantic.function ||
          lowered.qualifiedName != semantic.qualifiedName ||
          lowered.symbol != semantic.symbol ||
          lowered.returnType != semantic.returnType ||
          lowered.parameterTypes != semantic.parameterTypes ||
          !sameSpan(lowered.requestSource, semantic.requestSource) ||
          instance == nullptr || declaration == nullptr ||
          instance->id != lowered.function ||
          instance->declaration != lowered.semanticFunction ||
          instance->source != declaration->declaration || instance->owner ||
          !instance->typeArguments.empty() ||
          !instance->valueArguments.empty() ||
          instance->returnType != lowered.returnType ||
          instance->parameterTypes != lowered.parameterTypes ||
          instance->entryKind != ProgramEntryKind::None ||
          instance->linkage != LanguageLinkage::Gti) {
        result.errors.emplace_back(
            "embedding HIR export differs from its exact semantic function");
      }
      previousSymbol = lowered.symbol;
    }
  }
  if (program.programKind() == ProgramKind::Executable && embedding) {
    result.errors.emplace_back("executable HIR carries an embedding plan");
  }
  if (program.programKind() == ProgramKind::Library &&
      (!embedding || program.hostedProgramEntryPlan() ||
       !program.nativeCallbackAdapters().empty())) {
    result.errors.emplace_back(
        "library HIR lacks its export plan or retains an unsafe hosted root");
  }
  const auto verifyNativeInvocations = [&](const HirBody &body) {
    for (const auto &value : body.values) {
      if (!value.nativeCalleeType)
        continue;
      const auto &signature = *value.nativeCalleeType;
      const auto parameters = signature.nativeFunctionParameterTypes();
      const auto *callee =
          value.receiver ? body.findValue(*value.receiver) : nullptr;
      const bool expandedArguments =
          value.receiver && value.operands.size() > 1 &&
          value.operands.front() == *value.receiver &&
          std::any_of(value.operands.begin() + 1, value.operands.end(),
                      [&](HirValueId id) {
                        const HirValue *argument = body.findValue(id);
                        return argument &&
                               argument->kind == HirValueKind::PackExpansion &&
                               argument->info.type.kind ==
                                   SemanticType::TypePack &&
                               argument->info.type.concretePack;
                      });
      const bool orderedPlan =
          value.receiver && value.callPlan && value.callPlan->receiver &&
          value.callPlan->receiver->value == *value.receiver &&
          value.callPlan->receiver->type == signature &&
          value.callPlan->receiver->kind == HirCallInputKind::Value &&
          value.callPlan->arguments.size() == parameters.size();
      if (value.kind != HirValueKind::Call ||
          !signature.hasNativeFunctionShape() || !callee ||
          callee->info.type != signature || value.functionTarget ||
          value.constructorTarget || value.lambdaTarget ||
          value.nativeCallbackAdapter || (!orderedPlan && !expandedArguments) ||
          value.unsafeOperation != UnsafeOperationKind::ForeignPointerCall ||
          value.info.type != *signature.nativeFunctionReturnType() ||
          value.parameterTypes.size() != parameters.size() ||
          !std::equal(parameters.begin(), parameters.end(),
                      value.parameterTypes.begin())) {
        result.errors.emplace_back(
            "native indirect invocation lost its exact signature and callee "
            "plan");
      }
    }
  };
  verifyNativeInvocations(program.module());
  for (const auto &function : program.functionInstances())
    verifyNativeInvocations(function.body);
  for (const auto &constructor : program.constructorInstances())
    verifyNativeInvocations(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    verifyNativeInvocations(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    verifyNativeInvocations(lambda.body);
  for (const auto &instance : program.functionInstances()) {
    const auto *declaration = semantics.findFunction(instance.declaration);
    if (declaration == nullptr ||
        instance.requiresUnsafe != declaration->requiresUnsafe ||
        instance.neverReturns != declaration->neverReturns ||
        instance.tracksCaller != declaration->tracksCaller ||
        instance.cThreadOperation != declaration->cThreadOperation ||
        instance.processMemoryOperation !=
            declaration->processMemoryOperation) {
      result.errors.emplace_back(
          "HIR function unsafe contract differs from its semantic declaration");
    }
  }
  for (const auto &instance : program.classInstances()) {
    const auto *lifecycle = instance.source != nullptr
                                ? semantics.findClassLifecycle(*instance.source)
                                : nullptr;
    const bool requiresSchedule =
        lifecycle != nullptr && lifecycle->generatedDefaultInitializers;
    if (instance.generatedDefaultRequiresSchedule != requiresSchedule ||
        (requiresSchedule &&
         instance.defaultConstructor != SpecialMemberStatus::Generated))
      result.errors.emplace_back(
          "HIR generated-default obligation differs from semantics");
  }
  for (const auto &instance : program.constructorInstances()) {
    const auto *declaration = instance.source != nullptr
                                  ? semantics.findConstructor(*instance.source)
                                  : nullptr;
    if (instance.requiresUnsafe !=
        (declaration != nullptr && declaration->requiresUnsafe))
      result.errors.emplace_back(
          "constructor unsafe contract differs from semantics");
    if ((instance.declaration == 0) != instance.generatedDefault ||
        (instance.source == nullptr) != instance.generatedDefault)
      result.errors.emplace_back(
          "constructor source identity disagrees with its generated status");
    if (!instance.generatedDefault)
      continue;
    const auto *owner =
        instance.owner != 0 && instance.owner <= program.classInstances().size()
            ? &program.classInstances()[instance.owner - 1]
            : nullptr;
    const auto *lifecycle = owner != nullptr && owner->source != nullptr
                                ? semantics.findClassLifecycle(*owner->source)
                                : nullptr;
    if (lifecycle != nullptr && lifecycle->generatedDefaultInitializers) {
      bool exact =
          instance.parameterTypes.empty() &&
          instance.parameterBindings.empty() &&
          instance.typeArguments.empty() && instance.valueArguments.empty() &&
          instance.body.statements.empty() && instance.body.roots.empty() &&
          lifecycle->defaultConstructor == SpecialMemberStatus::Generated;
      std::size_t position = 0;
      const auto base = std::find_if(owner->bases.begin(), owner->bases.end(),
                                     [](const HirBaseInstance &candidate) {
                                       return !candidate.interface;
                                     });
      if (base != owner->bases.end()) {
        const auto *baseOwner =
            base->instance != 0 &&
                    base->instance <= program.classInstances().size()
                ? &program.classInstances()[base->instance - 1]
                : nullptr;
        const auto *baseLifecycle =
            baseOwner != nullptr && baseOwner->source != nullptr
                ? semantics.findClassLifecycle(*baseOwner->source)
                : nullptr;
        const auto *initializer = instance.initializers.empty()
                                      ? nullptr
                                      : &instance.initializers.front();
        const auto *target =
            initializer != nullptr && initializer->constructorTarget
                ? program.findConstructorInstance(
                      *initializer->constructorTarget)
                : nullptr;
        const auto *selected = target != nullptr && target->source != nullptr
                                   ? semantics.findConstructor(*target->source)
                                   : nullptr;
        exact &= initializer != nullptr &&
                 initializer->kind == ConstructorInitializerTargetKind::Base &&
                 initializer->source == nullptr &&
                 initializer->targetType == base->type &&
                 initializer->base == base->instance &&
                 initializer->field == 0 &&
                 initializer->explicitArgumentCount == 0 &&
                 !initializer->storesReference &&
                 !initializer->ownedParameter && baseLifecycle != nullptr &&
                 initializer->generatedDefault ==
                     (baseLifecycle->defaultConstructor ==
                      SpecialMemberStatus::Generated);
        if (initializer != nullptr && target != nullptr)
          exact &=
              target->owner == base->instance &&
              initializer->generatedDefault == target->generatedDefault &&
              (target->generatedDefault ||
               (selected != nullptr && selected->requiredParameterCount == 0));
        else if (initializer != nullptr)
          exact &= initializer->generatedDefault &&
                   initializer->arguments.empty() && base->instance != 0 &&
                   base->instance <= program.classInstances().size() &&
                   !program.classInstances()[base->instance - 1]
                        .generatedDefaultRequiresSchedule;
        ++position;
      }
      std::vector<HirValueId> selectedValues;
      for (const auto &initializer : instance.initializers)
        selectedValues.insert(selectedValues.end(),
                              initializer.arguments.begin(),
                              initializer.arguments.end());
      exact &= selectedValues == instance.initializerValues &&
               instance.initializers.size() == position + owner->fields.size();
      for (const auto &field : owner->fields) {
        const Expr *selectedSource =
            field.declaration == nullptr
                ? nullptr
                : (field.declaration->initializer()
                       ? field.declaration->initializer().get()
                       : semantics.implicitFieldInitializer(*field.declaration)
                             .get());
        const auto *initializer = position < instance.initializers.size()
                                      ? &instance.initializers[position]
                                      : nullptr;
        const auto *value =
            initializer != nullptr && initializer->arguments.size() == 1
                ? instance.body.findValue(initializer->arguments.front())
                : nullptr;
        exact &=
            field.declaration != nullptr && selectedSource != nullptr &&
            initializer != nullptr &&
            initializer->kind == ConstructorInitializerTargetKind::Field &&
            initializer->source == nullptr &&
            initializer->field == field.info.symbol &&
            initializer->targetType == field.info.type &&
            initializer->explicitArgumentCount == 1 && !initializer->base &&
            !initializer->constructorTarget && !initializer->generatedDefault &&
            !initializer->storesReference && !initializer->ownedParameter &&
            value != nullptr &&
            instance.body.initializerValueForSource(*selectedSource) == value &&
            (value->info.type == field.info.type ||
             isImplicitScalarInitializationConversion(field.info.type,
                                                      value->info.type) ||
             isImplicitExpectedInitializationConversion(field.info.type,
                                                        value->info.type));
        ++position;
      }
      if (!exact)
        result.errors.emplace_back(
            "generated constructor differs from its selected declaration "
            "initializers");
      continue;
    }
    result.errors.emplace_back(
        "generated constructor has no selected semantic initializer schedule");
  }
  const auto verifyDirectInitializations = [&](const HirBody &body) {
    std::unordered_set<const Expr *> seen;
    for (const auto &alias : body.directInitializations) {
      const auto *value = body.findValue(alias.value);
      const Expr *input = nullptr;
      const Binary *comma = alias.source == nullptr
                                ? nullptr
                                : semantics.builtinComma(*alias.source);
      if (const auto *group = exprAs<Grouping>(alias.source))
        input = group->expression().get();
      else if (comma != nullptr)
        input = comma->right().get();
      else if (const auto *call = exprAs<Call>(alias.source);
               call != nullptr && call->arguments().size() == 1)
        input = call->arguments().front().get();
      else if (const auto *direct = exprAs<DirectInitializer>(alias.source);
               direct != nullptr && direct->arguments().size() == 1)
        input = direct->arguments().front().get();
      const Expr *producer = alias.producer;
      for (std::size_t depth = 0; depth < body.directInitializations.size();
           ++depth) {
        const auto nested = std::find_if(
            body.directInitializations.begin(),
            body.directInitializations.end(),
            [&](const auto &entry) { return entry.source == producer; });
        if (nested == body.directInitializations.end())
          break;
        producer = nested->producer;
      }
      const auto *ids = alias.source == nullptr
                            ? nullptr
                            : &program.valueIdsForSource(*alias.source);
      const auto *selected = alias.source == nullptr
                                 ? nullptr
                                 : semantics.findConstruction(*alias.source);
      const bool grouping =
          exprAs<Grouping>(alias.source) != nullptr || comma != nullptr;
      // A comma alias's producer sequences exactly the comma's left operand
      // before it begins.
      const HirValue *discarded = comma == nullptr || comma->left() == nullptr
                                      ? nullptr
                                      : body.valueForSource(*comma->left());
      const bool sequencedLeft =
          comma == nullptr ||
          (value != nullptr && discarded != nullptr &&
           std::count(value->sequencedInputs.begin(),
                      value->sequencedInputs.end(), discarded->id) == 1);
      const auto &proof = alias.constructionProof;
      const bool exactConstruction =
          grouping ? !proof
                   : proof && proof->source() == alias.source &&
                         proof->producer() == alias.producer &&
                         proof->type() == alias.type &&
                         proof->snapshot() ==
                             semantics.analysisSeal().programSnapshot;
      const auto *sourceCall = exprAs<Call>(alias.source);
      const auto *call =
          sourceCall == nullptr ? nullptr : semantics.findCall(*sourceCall);
      const bool ordinaryCall =
          sourceCall != nullptr &&
          ((call != nullptr &&
            call->intrinsic != IntrinsicKind::ExactTypeParameterConstruction &&
            call->intrinsic !=
                IntrinsicKind::DefaultTypeParameterConstruction) ||
           semantics.findLambdaCall(*sourceCall) != nullptr ||
           semantics.findDeferredCallableCall(*sourceCall) != nullptr ||
           semantics.findOperator(*sourceCall) != nullptr);
      const bool executableSource = std::any_of(
          body.values.begin(), body.values.end(), [&](const auto &candidate) {
            return candidate.source == alias.source;
          });
      // A primary generic record may precede concrete reanalysis. When its
      // exact construction is retained, its direct-initialization decision
      // must agree; the concrete value/type and source identity are mandatory.
      const bool semanticMatch =
          !ordinaryCall && !executableSource && exactConstruction &&
          (selected == nullptr || selected->directInitialization || grouping);
      const bool fresh =
          value != nullptr && (value->kind == HirValueKind::Call ||
                               value->kind == HirValueKind::DirectInitializer ||
                               value->functionTarget.has_value());
      if (alias.source == nullptr || !seen.insert(alias.source).second ||
          input == nullptr || input != alias.producer || value == nullptr ||
          value->source != producer ||
          (alias.type.kind != SemanticType::Class &&
           (!grouping || alias.type.kind != SemanticType::Expected)) ||
          !sequencedLeft || value->info.type != alias.type ||
          value->info.category != ValueCategory::Value ||
          (!grouping && (!fresh || value->intrinsic == IntrinsicKind::Move)) ||
          !semanticMatch || ids == nullptr ||
          std::find(ids->begin(), ids->end(), alias.value) == ids->end())
        result.errors.emplace_back(
            "object result alias lost its exact source/type, sequencing "
            "and shared value identity");
    }
  };
  const auto verifyGeneratedConstructionValues = [&](const HirBody &body) {
    verifyDirectInitializations(body);
    for (const auto &value : body.values) {
      if (!value.defaultArguments.empty()) {
        const std::vector<Parameter> *parameters = nullptr;
        if (const auto *target =
                value.constructorTarget
                    ? program.findConstructorInstance(*value.constructorTarget)
                    : nullptr;
            target && target->source)
          parameters = &target->source->parameters();
        else if (const auto *target =
                     value.functionTarget
                         ? program.findFunctionInstance(*value.functionTarget)
                         : nullptr;
                 target) {
          // The canonical declaration owns defaults; source may instead be
          // an out-of-line executable definition that omits those defaults.
          const auto *declaration = semantics.findFunction(target->declaration);
          if (declaration && declaration->declaration)
            parameters = &declaration->declaration->parameters();
        }
        bool exact = parameters &&
                     value.defaultArguments.size() <= parameters->size() &&
                     value.defaultArguments.size() <= value.operands.size();
        if (exact) {
          const auto count = value.defaultArguments.size();
          exact = std::equal(value.defaultArguments.begin(),
                             value.defaultArguments.end(),
                             value.operands.end() - count);
          for (std::size_t index = 0; exact && index < count; ++index) {
            const auto &parameter =
                (*parameters)[parameters->size() - count + index];
            const auto *input = body.findValue(value.defaultArguments[index]);
            const auto *expression =
                parameter.defaultArgument
                    ? parameter.defaultArgument->expression.get()
                    : nullptr;
            exact = input && expression && input->source == expression &&
                    input->kind != HirValueKind::PackExpansion;
          }
        }
        if (!exact)
          result.errors.emplace_back(
              "caller-expanded defaults lost their exact declaration suffix");
      }
      for (std::size_t index = 0; index < value.operands.size(); ++index)
        if (const auto *input = body.findValue(value.operands[index]);
            input && input->kind == HirValueKind::PackExpansion &&
            index + 1 < value.operands.size() &&
            (value.defaultArguments.size() !=
                 value.operands.size() - index - 1 ||
             !std::equal(value.defaultArguments.begin(),
                         value.defaultArguments.end(),
                         value.operands.begin() + index + 1)))
          result.errors.emplace_back(
              "non-final argument pack lost its exact default suffix");
      if ((value.kind != HirValueKind::Call &&
           value.kind != HirValueKind::DirectInitializer) ||
          contextualConversionCall(body, value, semantics))
        continue;
      const auto *construction = value.source != nullptr
                                     ? semantics.findConstruction(*value.source)
                                     : nullptr;
      const auto *type =
          construction != nullptr && construction->generatedDefault
              ? semantics.findClassType(construction->constructedType.classId)
              : nullptr;
      const auto *lifecycle =
          type != nullptr && type->declaration != nullptr
              ? semantics.findClassLifecycle(*type->declaration)
              : nullptr;
      if (lifecycle == nullptr || !lifecycle->generatedDefaultInitializers)
        continue;
      const auto *target =
          value.constructorTarget
              ? program.findConstructorInstance(*value.constructorTarget)
              : nullptr;
      const auto *owner =
          target != nullptr && target->owner != 0 &&
                  target->owner <= program.classInstances().size()
              ? &program.classInstances()[target->owner - 1]
              : nullptr;
      const bool inPlace =
          value.intrinsic == IntrinsicKind::ObjectConstruct ||
          value.intrinsic == IntrinsicKind::OptionalStorageConstruct;
      bool exactResult = owner != nullptr && value.info.type == owner->type &&
                         value.intrinsic == IntrinsicKind::None;
      if (inPlace && owner != nullptr && !value.parameterTypes.empty()) {
        const auto &storage = value.parameterTypes.front();
        const auto *storageOwner =
            storage.kind == SemanticType::Class
                ? semantics.findClassType(storage.classId)
                : nullptr;
        exactResult =
            value.info.type == SemanticType::Void &&
            storage.arguments.size() == 1 &&
            storage.arguments.front() == owner->type &&
            (value.intrinsic == IntrinsicKind::ObjectConstruct
                 ? storage.kind == SemanticType::RawPointer
                 : storageOwner != nullptr &&
                       storageOwner->compilerCapability ==
                           CompilerCapabilityTypeKind::OptionalStorage);
      }
      const auto propagation =
          value.intrinsic == IntrinsicKind::OptionalStorageConstruct
              ? FailurePropagationKind::None
              : FailurePropagationKind::Constructor;
      // Concrete reanalysis owns generic argument substitution. The base
      // semantic record may still be the primary's C<T>; retain its exact
      // declaration identity and validate the concrete value/storage-to-owner
      // relationship instead of comparing C<T> with C<int>.
      const bool exactOwner =
          owner != nullptr &&
          (containsUnresolvedGeneric(construction->constructedType)
               ? owner->type.kind == SemanticType::Class &&
                     owner->type.classId ==
                         construction->constructedType.classId &&
                     !containsUnresolvedGeneric(owner->type)
               : owner->type == construction->constructedType);
      if (target == nullptr || !target->generatedDefault || owner == nullptr ||
          !exactOwner || !exactResult ||
          value.definedFailure.propagation != propagation)
        result.errors.emplace_back("generated construction value lost its "
                                   "selected constructor and failure edge");
    }
  };
  verifyGeneratedConstructionValues(program.module());
  for (const auto &owner : program.classInstances()) {
    verifyGeneratedConstructionValues(owner.fieldInitializers);
    verifyGeneratedConstructionValues(owner.staticFieldInitializers);
  }
  for (const auto &function : program.functionInstances())
    verifyGeneratedConstructionValues(function.body);
  for (const auto &constructor : program.constructorInstances())
    verifyGeneratedConstructionValues(constructor.body);
  for (const auto &destructor : program.destructorInstances())
    verifyGeneratedConstructionValues(destructor.body);
  for (const auto &lambda : program.lambdaInstances())
    verifyGeneratedConstructionValues(lambda.body);
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
                 ? (target->threadConfinedCallback
                        ? CThreadCallbackKind::ThreadConfined
                        : adapter.type.cThreadCallbackKind())
                 : CThreadCallbackKind::None) ||
        (program.executionProfile() == ExecutionProfile::Concurrent &&
         adapter.cThreadCallback == CThreadCallbackKind::None)) {
      result.errors.emplace_back(
          "native callback adapter has no exact free-function target");
    }
    if (target && declaration &&
        target->threadConfinedCallback != declaration->threadConfinedCallback) {
      result.errors.emplace_back(
          "native callback target differs from its semantic entry contract");
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
  // A contextual construction is the declared value its initializer's own
  // value is consumed by: exactly the semantic selection, with no result
  // borrow, the initializer first and the selected defaults after it.
  const auto verifyContextualConstructions = [&](const HirBody &body) {
    std::unordered_set<HirValueId> listed;
    for (const HirContextualConstruction &entry :
         body.contextualConstructions) {
      const HirValue *value = body.findValue(entry.value);
      const HirValue *argument = value == nullptr || value->operands.empty()
                                     ? nullptr
                                     : body.findValue(value->operands.front());
      const ResolvedConstructionInfo *construction =
          entry.source == nullptr
              ? nullptr
              : semantics.findContextualConstruction(*entry.source);
      const HirConstructorInstance *target =
          value != nullptr && value->constructorTarget
              ? program.findConstructorInstance(*value->constructorTarget)
              : nullptr;
      const HirClassInstance *owner =
          target == nullptr || target->owner == 0 ||
                  target->owner > program.classInstances().size()
              ? nullptr
              : &program.classInstances()[target->owner - 1];
      // Concrete reanalysis owns generic argument substitution: a generic
      // body's base record is still the primary's C<T>. Retain its exact
      // constructor identity and check the concrete instance it selected.
      const bool genericRecord =
          construction != nullptr &&
          (containsUnresolvedGeneric(construction->constructedType) ||
           std::any_of(construction->parameterTypes.begin(),
                       construction->parameterTypes.end(),
                       [](const SemanticType &type) {
                         return containsUnresolvedGeneric(type);
                       }));
      const bool exactOwner =
          owner != nullptr && construction != nullptr &&
          (genericRecord ? owner->type.kind == SemanticType::Class &&
                               owner->type.classId ==
                                   construction->constructedType.classId &&
                               !containsUnresolvedGeneric(owner->type)
                         : owner->type == construction->constructedType);
      const bool exactParameters =
          value != nullptr && construction != nullptr && target != nullptr &&
          (genericRecord
               ? value->parameterTypes == target->parameterTypes &&
                     std::none_of(value->parameterTypes.begin(),
                                  value->parameterTypes.end(),
                                  [](const SemanticType &type) {
                                    return containsUnresolvedGeneric(type);
                                  })
               : value->parameterTypes == construction->parameterTypes);
      if (value == nullptr || construction == nullptr || target == nullptr ||
          owner == nullptr || !listed.insert(entry.value).second ||
          value->kind != HirValueKind::DirectInitializer ||
          value->source != nullptr || argument == nullptr ||
          argument->source != entry.source ||
          body.valueForSource(*entry.source) != argument ||
          construction->constructor == 0 || construction->generatedDefault ||
          construction->kind != ConstructorKind::Ordinary ||
          construction->borrowOrigin != BorrowOriginKind::None ||
          !construction->borrowFields.empty() ||
          target->declaration != construction->constructor || !exactOwner ||
          value->info != ExpressionInfo{.type = owner->type,
                                        .category = ValueCategory::Value,
                                        .access = AccessMode::ReadOnly,
                                        .traits = owner->traits} ||
          !exactParameters ||
          value->operands.size() != value->parameterTypes.size() ||
          value->operands.size() != 1 + construction->defaultArguments.size() ||
          value->defaultArguments.size() !=
              construction->defaultArguments.size() ||
          !std::equal(value->defaultArguments.begin(),
                      value->defaultArguments.end(),
                      value->operands.begin() + 1) ||
          value->constructorKind != ConstructorKind::Ordinary ||
          value->definedFailure.propagation !=
              FailurePropagationKind::Constructor ||
          value->borrowOrigin != BorrowOriginKind::None ||
          !value->borrowFields.empty() || !value->callPlan ||
          value->callPlan->receiver ||
          value->callPlan->arguments.size() != value->operands.size()) {
        result.errors.emplace_back(
            "contextual construction HIR value differs from its semantic "
            "construction");
      }
    }
  };
  verifyContextualConstructions(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyContextualConstructions(instance.fieldInitializers);
    verifyContextualConstructions(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyContextualConstructions(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyContextualConstructions(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyContextualConstructions(instance.body);
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

  // Propagation through `std::tryable` keeps the semantic selection: each
  // step calls the selected block function on the one operand, in the same
  // full expression, and the residual return constructs through the selected
  // implicit constructor or is the residual itself. A generic instance's plan
  // lives in that instance's own analysis, so the program model supplies the
  // selected identities only for a non-generic body; the shape is checked for
  // both.
  const auto verifyPropagation = [&](const HirBody &body,
                                     const HirValue &value) {
    const auto *unary =
        value.source == nullptr ? nullptr : exprAs<Unary>(value.source);
    const ResolvedTryablePropagationInfo *selected =
        unary == nullptr ? nullptr : semantics.findTryablePropagation(*unary);
    if (value.kind != HirValueKind::Propagation || !value.propagation ||
        unary == nullptr || unary->oper().kind != TokenKind::QUESTION) {
      result.errors.emplace_back(
          "tryable propagation lost its selected semantic plan");
      return;
    }
    const HirPropagationPlan &plan = *value.propagation;
    const HirValue *operand = body.findValue(plan.operand);
    const HirValue *test = body.findValue(plan.test);
    const HirValue *output = body.findValue(plan.output);
    const HirValue *residual = body.findValue(plan.residual);
    const HirValue *residualReturn = plan.residualReturn == 0
                                         ? nullptr
                                         : body.findValue(plan.residualReturn);
    std::vector<HirValueId> operands{plan.operand, plan.test, plan.output,
                                     plan.residual};
    if (plan.residualReturn != 0) {
      operands.push_back(plan.residualReturn);
    }
    if (operand == nullptr || test == nullptr || output == nullptr ||
        residual == nullptr ||
        (plan.residualReturn != 0 && residualReturn == nullptr) ||
        value.operands != operands) {
      result.errors.emplace_back(
          "tryable propagation references missing protocol steps");
      return;
    }
    const bool movedTransport =
        plan.movedPlace != 0 && operand->kind == HirValueKind::Move &&
        operand->operands == std::vector<HirValueId>{plan.movedPlace};
    const auto movesPlace = [&](HirValueId id) {
      const HirValue *move = body.findValue(id);
      return move != nullptr && move->kind == HirValueKind::Move &&
             move->operands == std::vector<HirValueId>{plan.movedPlace} &&
             move->info.type == operand->info.type &&
             move->fullExpression == value.fullExpression;
    };
    const bool passiveEnumOperand =
        operand->info.type.kind == SemanticType::Enum &&
        std::any_of(program.enumDeclarations().begin(),
                    program.enumDeclarations().end(),
                    [&](const HirEnum &enumeration) {
                      return enumeration.type == operand->info.type &&
                             !enumeration.requiresActiveCleanup;
                    });
    const auto selectedCall = [&](const HirValue &call,
                                  const ResolvedCallInfo *step, bool consumes) {
      const HirFunctionInstance *target =
          call.functionTarget
              ? program.findFunctionInstance(*call.functionTarget)
              : nullptr;
      HirCallInputKind kind =
          !plan.consumesOperand
              ? (consumes ? HirCallInputKind::CopyValue
                          : HirCallInputKind::ReadBorrow)
              : (consumes ? HirCallInputKind::MoveValue
                          : HirCallInputKind::ReadTemporaryBorrow);
      // An enum without cleanup passes by value, as in any by-value call.
      if (consumes && passiveEnumOperand) {
        kind = HirCallInputKind::Value;
      }
      const auto movesTemporary = [&](HirValueId id) {
        const HirValue *move = body.findValue(id);
        return move != nullptr && move->kind == HirValueKind::Move &&
               move->operands == std::vector<HirValueId>{operand->id} &&
               move->info.type == operand->info.type &&
               move->fullExpression == value.fullExpression;
      };
      bool argument = call.operands.size() == 1 &&
                      (plan.consumesOperand && consumes
                           ? movesTemporary(call.operands.front())
                           : call.operands.front() == operand->id);
      if (movedTransport) {
        kind = !consumes            ? HirCallInputKind::ReadBorrow
               : passiveEnumOperand ? HirCallInputKind::Value
                                    : HirCallInputKind::MoveValue;
        argument = call.operands.size() == 1 &&
                   (consumes ? movesPlace(call.operands.front())
                             : call.operands.front() == plan.movedPlace);
      }
      return call.kind == HirValueKind::Call && call.source == nullptr &&
             call.intrinsic == IntrinsicKind::None && target != nullptr &&
             (step == nullptr || target->declaration == step->function) &&
             argument && !call.receiver && call.callPlan &&
             !call.callPlan->receiver && call.callPlan->arguments.size() == 1 &&
             call.callPlan->arguments.front().value == call.operands.front() &&
             call.callPlan->arguments.front().kind == kind &&
             (step == nullptr || call.info.type == step->returnType) &&
             call.fullExpression == value.fullExpression;
    };
    const HirConstructorInstance *construction =
        residualReturn != nullptr && residualReturn->constructorTarget
            ? program.findConstructorInstance(
                  *residualReturn->constructorTarget)
            : nullptr;
    // The residual is the return value itself, its implicit construction
    // (residual first, then the selected defaults), or the receiver of its
    // implicit conversion operator's call.
    const bool constructsReturn =
        selected != nullptr
            ? selected->returnConstruction.has_value()
            : residualReturn != nullptr &&
                  residualReturn->kind == HirValueKind::DirectInitializer;
    const bool convertsReturn =
        selected != nullptr ? selected->returnConversion.has_value()
                            : residualReturn != nullptr &&
                                  residualReturn->kind == HirValueKind::Call;
    const HirFunctionInstance *conversion =
        residualReturn != nullptr && residualReturn->functionTarget
            ? program.findFunctionInstance(*residualReturn->functionTarget)
            : nullptr;
    const bool returnsResidual =
        constructsReturn
            ? residualReturn != nullptr &&
                  residualReturn->kind == HirValueKind::DirectInitializer &&
                  residualReturn->source == nullptr &&
                  construction != nullptr &&
                  (selected == nullptr ||
                   construction->declaration ==
                       selected->returnConstruction->constructor) &&
                  !residualReturn->operands.empty() &&
                  residualReturn->operands.front() == residual->id &&
                  residualReturn->operands.size() ==
                      1 + residualReturn->defaultArguments.size() &&
                  std::equal(residualReturn->defaultArguments.begin(),
                             residualReturn->defaultArguments.end(),
                             residualReturn->operands.begin() + 1) &&
                  residualReturn->fullExpression == value.fullExpression &&
                  residualReturn->callPlan &&
                  !residualReturn->callPlan->receiver &&
                  residualReturn->callPlan->arguments.size() ==
                      residualReturn->operands.size()
        : convertsReturn
            ? residualReturn != nullptr &&
                  residualReturn->kind == HirValueKind::Call &&
                  residualReturn->source == nullptr && conversion != nullptr &&
                  (selected == nullptr ||
                   conversion->declaration ==
                       selected->returnConversion->function) &&
                  residualReturn->operands ==
                      std::vector<HirValueId>{residual->id} &&
                  residualReturn->receiver == residual->id &&
                  residualReturn->fullExpression == value.fullExpression &&
                  residualReturn->callPlan &&
                  residualReturn->callPlan->receiver &&
                  residualReturn->callPlan->receiver->value == residual->id &&
                  residualReturn->callPlan->arguments.empty()
            : plan.residualReturn == 0;
    if ((selected != nullptr &&
         (plan.consumesOperand != selected->consumesOperand ||
          operand->info.type != selected->operandType ||
          value.info.type != selected->resultType ||
          residual->info.type != selected->residualType)) ||
        plan.consumesOperand !=
            (operand->info.category == ValueCategory::Value) ||
        (plan.movedPlace != 0 && !movedTransport) ||
        value.info.category != ValueCategory::Value ||
        !selectedCall(*test, selected ? &selected->test : nullptr, false) ||
        test->info.type != SemanticType::Bool ||
        !selectedCall(*output, selected ? &selected->output : nullptr, true) ||
        output->info.type != value.info.type ||
        !selectedCall(*residual, selected ? &selected->residual : nullptr,
                      true) ||
        !returnsResidual) {
      result.errors.emplace_back(
          "tryable propagation lost its selected block calls, operand "
          "transport, or residual return");
    }
  };

  const auto verifyCallableValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.kind == HirValueKind::Propagation || value.propagation) {
        verifyPropagation(body, value);
      }
      if (value.kind == HirValueKind::ExpectedPropagation ||
          value.expectedPropagation) {
        if (value.kind != HirValueKind::ExpectedPropagation ||
            !value.expectedPropagation) {
          result.errors.emplace_back(
              "expected propagation lost its selected plan");
        } else {
          const auto &plan = *value.expectedPropagation;
          const auto *operand = body.findValue(plan.operand);
          const auto *success = body.findValue(plan.successExtraction);
          const auto *error = body.findValue(plan.errorExtraction);
          const auto *transport = body.findValue(plan.errorValue);
          const auto *unexpected = body.findValue(plan.unexpected);
          const bool carrier =
              operand != nullptr &&
              operand->info.type.kind == SemanticType::Expected &&
              operand->info.type.arguments.size() == 2;
          if (!carrier || success == nullptr || error == nullptr ||
              transport == nullptr || unexpected == nullptr) {
            result.errors.emplace_back(
                "expected propagation references missing alternatives");
          } else {
            const auto &payload = operand->info.type.arguments.front();
            const bool borrowed =
                payload.kind == SemanticType::Reference &&
                payload.arguments.size() == 1 &&
                payload.referenceAccess == AccessMode::ReadOnly;
            const SemanticType resultType =
                borrowed ? payload.arguments.front() : payload;
            const auto extraction = [&](const HirValue &part,
                                        IntrinsicKind intrinsic,
                                        const SemanticType &type) {
              return part.kind == HirValueKind::Call &&
                     part.intrinsic == intrinsic &&
                     part.receiver == operand->id &&
                     part.operands == std::vector<HirValueId>{operand->id} &&
                     part.info.type == type &&
                     part.fullExpression == value.fullExpression;
            };
            if (plan.moves !=
                    (operand->info.category == ValueCategory::Value) ||
                value.info.type != resultType ||
                value.info.category !=
                    (borrowed ? ValueCategory::Place : ValueCategory::Value) ||
                value.info.access != AccessMode::ReadOnly ||
                !extraction(*success, IntrinsicKind::ExpectedValue,
                            resultType) ||
                !extraction(*error, IntrinsicKind::ExpectedError,
                            operand->info.type.arguments[1]) ||
                (borrowed &&
                 (plan.successValue != plan.successExtraction ||
                  success->info.category != ValueCategory::Place ||
                  success->info.access != AccessMode::ReadOnly ||
                  success->borrowOrigin != BorrowOriginKind::Receiver ||
                  success->borrowAccess != AccessMode::ReadOnly)) ||
                unexpected->kind != HirValueKind::Unexpected ||
                unexpected->operands !=
                    std::vector<HirValueId>{plan.errorValue} ||
                (plan.moves
                     ? transport->kind != HirValueKind::Move ||
                           transport->operands !=
                               std::vector<HirValueId>{plan.errorExtraction}
                     : plan.errorValue != plan.errorExtraction)) {
              result.errors.emplace_back(
                  "expected propagation lost its selected extraction or "
                  "transfer contract");
            }
          }
        }
      }
      const HirFunctionInstance *contractTarget =
          value.kind == HirValueKind::Call && value.functionTarget
              ? program.findFunctionInstance(*value.functionTarget)
              : nullptr;
      const bool panic = value.kind == HirValueKind::Call &&
                         value.intrinsic == IntrinsicKind::Panic;
      const bool expectedTerminal =
          panic || (contractTarget != nullptr && contractTarget->neverReturns);
      const bool expectedTracking =
          panic || (contractTarget != nullptr && contractTarget->tracksCaller);
      const CallerLocationOrigin *origin =
          value.source == nullptr ? nullptr
                                  : semantics.findCallerLocation(*value.source);
      if (value.neverReturns != expectedTerminal ||
          value.tracksCaller != expectedTracking ||
          value.callerLocation.has_value() != expectedTracking ||
          (value.callerLocation &&
           (origin == nullptr || *value.callerLocation != *origin))) {
        result.errors.emplace_back(
            "HIR call terminal/caller contract differs from its selected "
            "declaration or semantic source origin");
      }
      std::unordered_set<HirValueId> sequenced;
      for (const HirValueId inputId : value.sequencedInputs) {
        const HirValue *input = body.findValue(inputId);
        if (input == nullptr || input->neverReturns ||
            input->fullExpression != value.fullExpression ||
            value.programConstantSubstitution ||
            !sequenced.insert(inputId).second) {
          result.errors.emplace_back(
              "sequenced HIR inputs must be distinct normally completing "
              "evaluations in the same full expression");
        }
        std::unordered_set<HirValueId> visited;
        std::vector<HirValueId> pending{inputId};
        while (!pending.empty()) {
          const HirValueId id = pending.back();
          pending.pop_back();
          if (id == value.id) {
            result.errors.emplace_back("sequenced HIR inputs contain a cycle");
            break;
          }
          if (!visited.insert(id).second) {
            continue;
          }
          const HirValue *dependency = body.findValue(id);
          if (dependency == nullptr) {
            result.errors.emplace_back(
                "sequenced HIR input references a missing value");
            continue;
          }
          pending.insert(pending.end(), dependency->operands.begin(),
                         dependency->operands.end());
          pending.insert(pending.end(), dependency->conditionalInputs.begin(),
                         dependency->conditionalInputs.end());
          pending.insert(pending.end(), dependency->sequencedInputs.begin(),
                         dependency->sequencedInputs.end());
          if (dependency->receiver) {
            pending.push_back(*dependency->receiver);
          }
        }
      }
      const auto *sourceCall = exprAs<Call>(value.source);
      const auto *transform =
          sourceCall == nullptr ? nullptr
                                : semantics.findExpectedCombinator(*sourceCall);
      if (!value.conditionalInputs.empty() ||
          (value.kind == HirValueKind::Conditional && transform != nullptr)) {
        const auto *member = sourceCall == nullptr
                                 ? nullptr
                                 : exprAs<Get>(sourceCall->callee().get());
        const auto ungroup = [](const Expr *expression) {
          while (const auto *grouping = exprAs<Grouping>(expression)) {
            expression = grouping->expression().get();
          }
          return expression;
        };
        const auto *receiver = value.conditionalInputs.size() == 2
                                   ? body.findValue(value.conditionalInputs[0])
                                   : nullptr;
        const auto *mapper = value.conditionalInputs.size() == 2
                                 ? body.findValue(value.conditionalInputs[1])
                                 : nullptr;
        if (value.kind != HirValueKind::Conditional || transform == nullptr ||
            member == nullptr || sourceCall->arguments().size() != 1 ||
            value.operands.size() != 3 || receiver == nullptr ||
            mapper == nullptr || receiver->id == mapper->id ||
            receiver->id == value.id || mapper->id == value.id ||
            receiver->fullExpression != value.fullExpression ||
            mapper->fullExpression != value.fullExpression ||
            ungroup(receiver->source) != ungroup(member->object().get()) ||
            ungroup(mapper->source) !=
                ungroup(sourceCall->arguments().front().get())) {
          result.errors.emplace_back("conditional eager inputs differ from the "
                                     "selected source call order");
        }
        if (transform != nullptr && value.operands.size() == 3) {
          const HirValue *success = body.findValue(value.operands[1]);
          const HirValue *failure = body.findValue(value.operands[2]);
          // The owning semantic record can be from a generic primary. Check
          // the concrete HIR relationship instead of comparing its resolved
          // result to an unsubstituted base-model type parameter.
          const bool ownedSource =
              receiver != nullptr &&
              receiver->info.type.kind == SemanticType::Expected &&
              receiver->info.type.arguments.size() == 2;
          const bool expectedResult =
              value.info.type.kind == SemanticType::Expected &&
              value.info.type.arguments.size() == 2;
          const bool borrowedSuccess =
              ownedSource &&
              receiver->info.type.arguments.front().kind ==
                  SemanticType::Reference &&
              receiver->info.type.arguments.front().arguments.size() == 1;
          const bool validResult =
              ownedSource &&
              (transform->kind == ExpectedCombinatorKind::Expect
                   ? borrowedSuccess
                         ? value.info.type ==
                                   receiver->info.type.arguments.front()
                                       .arguments.front() &&
                               value.info.category == ValueCategory::Place &&
                               value.info.access == AccessMode::ReadOnly
                         : value.info.type ==
                               receiver->info.type.arguments.front()
                   : expectedResult &&
                         value.info.type.arguments
                                 [transform->kind ==
                                          ExpectedCombinatorKind::TransformError
                                      ? 0
                                      : 1] ==
                             receiver->info.type.arguments
                                 [transform->kind ==
                                          ExpectedCombinatorKind::TransformError
                                      ? 0
                                      : 1]);
          if (!validResult || success == nullptr || failure == nullptr) {
            result.errors.emplace_back(
                "expected conditional lost its selected result or arms");
          } else if (transform->kind == ExpectedCombinatorKind::Expect) {
            if (borrowedSuccess &&
                (success->kind != HirValueKind::Call ||
                 success->intrinsic != IntrinsicKind::ExpectedValue ||
                 success->borrowOrigin != BorrowOriginKind::Receiver ||
                 success->borrowAccess != AccessMode::ReadOnly ||
                 success->info.category != ValueCategory::Place ||
                 success->info.access != AccessMode::ReadOnly ||
                 success->info.type != value.info.type ||
                 success->operands != std::vector<HirValueId>{receiver->id} ||
                 success->receiver != receiver->id)) {
              result.errors.emplace_back("borrowed expect success lost its "
                                         "read-only reference extraction");
            }
            if (failure->intrinsic != IntrinsicKind::Panic ||
                failure->info.type != SemanticType::Void ||
                !failure->neverReturns || !failure->tracksCaller ||
                failure->operands.size() != 1 || mapper == nullptr ||
                failure->operands.front() != mapper->id ||
                failure->parameterTypes !=
                    std::vector<SemanticType>{SemanticType::StringView} ||
                failure->source != sourceCall ||
                failure->definedFailure.localOrigins.empty()) {
              result.errors.emplace_back("expected expect failure arm lost its "
                                         "selected terminal panic");
            }
          } else if (transform->kind == ExpectedCombinatorKind::Transform &&
                     expectedResult &&
                     value.info.type.arguments.front() == SemanticType::Void) {
            const HirValue *mapped =
                success->neverReturns ? success
                : success->sequencedInputs.size() == 1
                    ? body.findValue(success->sequencedInputs.front())
                    : nullptr;
            const HirFunctionInstance *function =
                mapped != nullptr && mapped->functionTarget
                    ? program.findFunctionInstance(*mapped->functionTarget)
                    : nullptr;
            const HirLambda *lambda =
                mapped != nullptr && mapped->lambdaTarget
                    ? program.findLambda(*mapped->lambdaTarget)
                    : nullptr;
            if (success->kind != HirValueKind::Call ||
                (!success->neverReturns &&
                 success->info.type != value.info.type) ||
                mapped == nullptr || mapped->kind != HirValueKind::Call ||
                mapped->info.type != SemanticType::Void ||
                mapped->source != sourceCall ||
                (transform->mapper != 0 &&
                 (function == nullptr ||
                  function->declaration != transform->mapper)) ||
                (transform->lambda != 0 &&
                 (lambda == nullptr ||
                  lambda->declaration != transform->lambda))) {
              result.errors.emplace_back(
                  "void expected transformation must complete its selected "
                  "mapper before publishing success");
            }
          }
        }
      }
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
  verifyCallableValues(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    verifyCallableValues(instance.fieldInitializers);
    verifyCallableValues(instance.staticFieldInitializers);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    verifyCallableValues(instance.body);
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyCallableValues(instance.body);
  }

  std::vector<std::size_t> threadTaskRoots(program.threadTaskAdapters().size(),
                                           0);
  std::vector<SemanticType> threadTaskResults(
      program.threadTaskAdapters().size(), SemanticType::Unknown);
  const auto verifyThreadTaskValues = [&](const HirBody &body) {
    for (const HirValue &value : body.values) {
      if (value.intrinsic == IntrinsicKind::ThreadScope) {
        if (value.threadTaskAdapter ||
            value.scopedThreadTasks.size() != value.parameterTypes.size() ||
            value.info.type != SemanticType::Int32 ||
            value.synchronization.kind !=
                SynchronizationOperationKind::ThreadScope ||
            !validSynchronizationOperation(value.synchronization) ||
            program.executionProfile() != ExecutionProfile::Concurrent)
          result.errors.emplace_back(
              "parallel scope has an invalid HIR group contract");
        for (std::size_t index = 0; index < value.scopedThreadTasks.size();
             ++index) {
          const auto *adapter =
              program.findThreadTaskAdapter(value.scopedThreadTasks[index]);
          if (adapter == nullptr || !adapter->scoped ||
              adapter->spawnValue != value.id ||
              adapter->scopeArgument != index ||
              index >= value.parameterTypes.size() ||
              adapter->taskType != value.parameterTypes[index]) {
            result.errors.emplace_back(
                "parallel scope has no exact HIR task operand");
            continue;
          }
          for (const auto &dependency : adapter->scopeDependencies)
            if (dependency.place.domain != body.placeDomain ||
                (dependency.place.root == 0 && !dependency.place.receiver) ||
                dependency.designatedFields.empty())
              result.errors.emplace_back(
                  "parallel task has no exact body-local dependency");
          ++threadTaskRoots[adapter->id - 1];
          threadTaskResults[adapter->id - 1] = SemanticType::Void;
        }
        continue;
      }
      if (!value.scopedThreadTasks.empty())
        result.errors.emplace_back(
            "non-scope HIR value carries scoped task operands");
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
      if (adapter == nullptr || adapter->spawnValue != value.id ||
          adapter->scoped || !adapter->scopeDependencies.empty()) {
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

  // Statements inside a compile-time branch or expansion that the symbolic
  // primary deferred (a dependent `if constexpr` condition or `constexpr for`
  // range) have no primary semantic record: concrete reanalysis analyzed them
  // first. Their HIR plan is verified on its own concrete facts.
  const auto deferredPrimaryStatements = [&](const HirBody &body) {
    std::unordered_set<HirStatementId> deferred;
    const auto visit = [&](auto &self, HirStatementId id) -> void {
      const HirStatement *statement = body.findStatement(id);
      if (statement == nullptr || !deferred.insert(id).second) {
        return;
      }
      if (statement->initializer) {
        self(self, *statement->initializer);
      }
      if (statement->body) {
        self(self, *statement->body);
      }
      if (statement->elseBranch) {
        self(self, *statement->elseBranch);
      }
      for (const HirStatementId nested : statement->statements) {
        self(self, nested);
      }
      for (const HirSwitchArm &arm : statement->switchArms) {
        for (const HirStatementId nested : arm.statements) {
          self(self, nested);
        }
      }
    };
    for (const HirStatement &statement : body.statements) {
      if (statement.kind != HirStatementKind::CompileTimeBranch) {
        continue;
      }
      const auto *ifStatement = stmtAs<IfStmt>(statement.source);
      const auto *loop = stmtAs<ConstexprForStmt>(statement.source);
      const bool deferredInPrimary =
          (ifStatement != nullptr && ifStatement->isConstexpr() &&
           !semantics.findConstexprBranch(*ifStatement).has_value()) ||
          (loop != nullptr &&
           semantics.findConstexprForExpansion(*loop) == nullptr);
      if (deferredInPrimary) {
        visit(visit, statement.id);
      }
    }
    return deferred;
  };

  const auto verifyRangeIterations = [&](const HirBody &body,
                                         bool concreteReanalysis = false) {
    const std::unordered_set<HirStatementId> deferred =
        concreteReanalysis ? deferredPrimaryStatements(body)
                           : std::unordered_set<HirStatementId>{};
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
      const bool primaryDeferred =
          semantic == nullptr && deferred.contains(statement.id);
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
               body.valueForSource(*expression) == value;
      };
      const HirLoan *rangeLoan =
          range == nullptr ? nullptr : body.findLoan(range->rangeLoan);
      const HirLoan *elementLoan =
          range == nullptr ? nullptr : body.findLoan(range->elementLoan);
      const HirLoan *elementOwnerLoan =
          range == nullptr ? nullptr : body.findLoan(range->elementOwnerLoan);
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
      const HirBinding *elementOwner =
          range == nullptr ? nullptr : bindingFor(range->elementOwner);
      const HirValue *elementAccessValue =
          range == nullptr ? nullptr
                           : body.findValue(range->elementAccessValue);
      const HirStatement *elementOwnerStatement =
          range == nullptr ? nullptr
                           : body.findStatement(range->elementOwnerStatement);
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
          (((genericSemantic || concreteReanalysis) &&
            (range->rangeLoan != 0) == (semantic->rangeLoan != 0) &&
            (range->elementLoan != 0) == (semantic->elementLoan != 0) &&
            (genericSemantic ||
             ((range->elementOwnerLoan != 0) ==
                  (semantic->elementOwnerLoan != 0) &&
              (range->rangeLoan == range->elementLoan) ==
                  (semantic->rangeLoan == semantic->elementLoan)))) ||
           (!genericSemantic && !concreteReanalysis &&
            range->rangeLoan == semantic->rangeLoan &&
            range->elementLoan == semantic->elementLoan &&
            range->elementOwnerLoan == semantic->elementOwnerLoan));
      const bool exactSemantic =
          (primaryDeferred && range != nullptr && exactConcreteTypes) ||
          (semantic != nullptr && range != nullptr && exactConcreteTypes &&
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
               semantic->expectedPayloadProjection &&
           (genericSemantic ||
            range->nonBorrowingEndpoint == semantic->nonBorrowingEndpoint) &&
           (genericSemantic || range->materializesElementValue ==
                                   semantic->materializesElementValue));
      const bool exactStructure =
          source != nullptr && range != nullptr && statement.body &&
          body.findStatement(*statement.body) != nullptr &&
          body.findStatement(range->coreLoop) != nullptr &&
          exactBinding(range->owner, source->ownerBinding()) &&
          exactBinding(range->rangeCarrier, source->rangeBinding()) &&
          exactBinding(range->iterator, source->iteratorBinding()) &&
          exactBinding(range->sentinel, source->sentinelBinding()) &&
          exactBinding(range->element, source->elementBinding()) &&
          (source->elementOwnerBinding() == nullptr
               ? range->elementOwner == 0
               : exactBinding(range->elementOwner,
                              source->elementOwnerBinding())) &&
          exactValue(range->source, source->ownerBinding()->initializer()) &&
          exactValue(range->begin, source->iteratorBinding()->initializer()) &&
          exactValue(range->end, source->sentinelBinding()->initializer()) &&
          exactValue(range->comparison, source->coreLoop()->condition()) &&
          exactValue(range->elementAccessValue,
                     source->elementOwnerBinding() == nullptr
                         ? source->elementBinding()->initializer()
                         : source->elementOwnerBinding()->initializer()) &&
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
      const HirValue *writtenElementSource =
          source == nullptr || source->elementBinding() == nullptr
              ? nullptr
              : body.valueForSource(*source->elementBinding()->initializer());
      const bool exactElementStaging =
          range != nullptr && source != nullptr &&
          elementAccessValue != nullptr &&
          (source->elementOwnerBinding() == nullptr
               ? elementOwner == nullptr && range->elementOwnerStatement == 0 &&
                     range->elementOwnerLoan == 0 &&
                     !range->materializesElementValue
               : source->elementOwnerBinding()->isRangeElementOwner() &&
                     elementOwner != nullptr &&
                     elementOwnerStatement != nullptr &&
                     elementOwnerStatement->source ==
                         source->elementOwnerBinding() &&
                     elementOwnerStatement->binding == range->elementOwner &&
                     elementOwnerStatement->value ==
                         range->elementAccessValue &&
                     elementOwner->info.access == AccessMode::ReadOnly &&
                     referentOrSelf(elementOwner->info.type) ==
                         elementAccessValue->info.type &&
                     writtenElementSource != nullptr &&
                     writtenElementSource->kind == HirValueKind::Variable &&
                     writtenElementSource->symbol ==
                         elementOwner->info.symbol &&
                     (range->materializesElementValue
                          ? elementAccessValue->info.category ==
                                    ValueCategory::Value &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::Reference &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::StringView &&
                                elementAccessValue->info.type.kind !=
                                    SemanticType::CString &&
                                elementAccessValue->info.traits.copyable &&
                                !elementAccessValue->info.traits
                                     .containsBorrowedState &&
                                elementAccessValue->borrowOrigin ==
                                    BorrowOriginKind::None &&
                                elementOwner->info.type ==
                                    elementAccessValue->info.type &&
                                range->elementOwnerLoan == 0 &&
                                range->elementAccess == AccessMode::ReadOnly &&
                                (!referenceElement ||
                                 (elementLoan != nullptr &&
                                  elementLoan->parent == 0 &&
                                  elementLoan->place.root ==
                                      elementOwner->info.symbol &&
                                  elementLoan->place.projections.empty() &&
                                  !elementLoan->place.receiver))
                          : elementAccessValue->info.category ==
                                    ValueCategory::Place &&
                                elementOwner->info.type.kind ==
                                    SemanticType::Reference &&
                                elementOwnerLoan != nullptr &&
                                elementOwnerLoan->access ==
                                    AccessMode::ReadOnly &&
                                std::find(elementOwnerLoan->carriers.begin(),
                                          elementOwnerLoan->carriers.end(),
                                          elementOwner->id) !=
                                    elementOwnerLoan->carriers.end()));
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
      const auto *endpoint = range ? body.findValue(range->end) : nullptr;
      const bool exactEndpointResult =
          range && (!range->nonBorrowingEndpoint ||
                    (endpoint && endpoint->info.type == range->sentinelType &&
                     endpoint->info.type.kind != SemanticType::Reference &&
                     !endpoint->info.traits.containsBorrowedState &&
                     endpoint->borrowOrigin == BorrowOriginKind::None));
      if (!exactSemantic || !exactStructure || !exactLoans || !exactStrategy ||
          !exactOwner || !exactExpectedPayloadProjection ||
          !exactEndpointResult || !exactElementStaging) {
        result.errors.emplace_back(
            "range HIR plan differs from semantic owner/loan/protocol facts");
      }
    }
  };
  verifyRangeIterations(program.module());
  for (const HirClassInstance &instance : program.classInstances()) {
    const bool reanalyzed =
        !instance.typeArguments.empty() || !instance.valueArguments.empty();
    verifyRangeIterations(instance.fieldInitializers, reanalyzed);
    verifyRangeIterations(instance.staticFieldInitializers, reanalyzed);
  }
  for (const HirFunctionInstance &instance : program.functionInstances()) {
    const FunctionInfo *function = semantics.findFunction(instance.declaration);
    const ClassTypeInfo *owner =
        function == nullptr || function->ownerClass == 0
            ? nullptr
            : semantics.findClassType(function->ownerClass);
    // Reanalysis starts a new body-local loan domain even when the range's
    // type happens not to mention any generic parameter. Compare the loan
    // relationship, not numeric identities allocated in the primary body.
    verifyRangeIterations(
        instance.body,
        function != nullptr &&
            (!function->genericParameters.empty() ||
             (owner != nullptr && !owner->genericParameters.empty())));
  }
  for (const HirConstructorInstance &instance :
       program.constructorInstances()) {
    verifyRangeIterations(instance.body, !instance.generatedDefault);
  }
  for (const HirDestructorInstance &instance : program.destructorInstances()) {
    const HirClassInstance *owner =
        instance.owner == 0 || instance.owner > program.classInstances().size()
            ? nullptr
            : &program.classInstances()[instance.owner - 1];
    verifyRangeIterations(instance.body,
                          owner != nullptr && (!owner->typeArguments.empty() ||
                                               !owner->valueArguments.empty()));
  }
  for (const HirLambda &instance : program.lambdaInstances()) {
    verifyRangeIterations(
        instance.body,
        !instance.type.lambdaEnclosingClassTypes.empty() ||
            !instance.type.lambdaEnclosingClassValues.empty() ||
            !instance.type.lambdaEnclosingFunctionTypes.empty() ||
            !instance.type.lambdaEnclosingFunctionValues.empty());
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
        semantic.declaration->initializer() == nullptr ||
        program.module().initializerValueForSource(
            *semantic.declaration->initializer()) != initializer) {
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
    pendingValues.insert(pendingValues.end(), value->conditionalInputs.begin(),
                         value->conditionalInputs.end());
    pendingValues.insert(pendingValues.end(), value->sequencedInputs.begin(),
                         value->sequencedInputs.end());
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
    for (const HirValueId input : parent.conditionalInputs) {
      valueParents[input].push_back(&parent);
    }
    for (const HirValueId input : parent.sequencedInputs) {
      valueParents[input].push_back(&parent);
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
        ((construction->generatedDefault && ([&] {
            const auto *lifecycle =
                owner != nullptr && owner->source != nullptr
                    ? semantics.findClassLifecycle(*owner->source)
                    : nullptr;
            return lifecycle != nullptr &&
                           lifecycle->generatedDefaultInitializers
                       ? target != nullptr && target->generatedDefault &&
                             targetOwner == owner
                       : !value.constructorTarget;
          }())) ||
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
               ((construction->constructor != 0 &&
                 !construction->generatedDefault) ||
                        (target != nullptr && target->generatedDefault)
                    ? FailurePropagationKind::Constructor
                    : FailurePropagationKind::None);
  };
  const auto exactSyntheticCommon = [](const HirValue &value) {
    return value.source == nullptr &&
           value.unsafeOperation == UnsafeOperationKind::None &&
           value.operands.empty() && value.conditionalInputs.empty() &&
           value.sequencedInputs.empty() && value.parameterTypes.empty() &&
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
        !value.parameterTypes.empty() || !value.conditionalInputs.empty() ||
        !value.sequencedInputs.empty() || value.operation || value.constant ||
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
  const auto listedContextualConstruction = [&](const HirValue &value) {
    const auto &entries = program.module().contextualConstructions;
    return value.source == nullptr &&
           value.kind == HirValueKind::DirectInitializer &&
           std::any_of(entries.begin(), entries.end(),
                       [&](const HirContextualConstruction &entry) {
                         return entry.value == value.id;
                       });
  };
  const auto exactSyntheticValue = [&](const HirValue &value) {
    if (exactDefaultArrayElement(value) || exactDefaultArrayLevel(value) ||
        listedContextualConstruction(value)) {
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
    // A contextual conversion of a source's value is a further call that
    // shares the source; its result is the conversion's, not the source's.
    if (contextualConversionCall(program.module(), value, semantics)) {
      const ResolvedOperatorInfo &conversion =
          *semantics.findContextualConversion(*value.source);
      if (value.info.type != conversion.returnType ||
          value.info.category != ValueCategory::Value || value.constant ||
          value.programConstantSubstitution) {
        result.errors.emplace_back(
            "module contextual conversion value differs from its semantic "
            "conversion");
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
      vectorConstructor->typeArguments.empty() &&
      vectorConstructor->valueArguments.empty() &&
      vectorConstructor->parameterTypes.empty();
  const bool exactStringConstructor =
      stringConstructor != nullptr &&
      stringConstructor->declaration == semanticHosted->stringConstructor &&
      exactOwner(stringConstructor->owner, stringType) &&
      stringConstructor->typeArguments.empty() &&
      stringConstructor->valueArguments.empty() &&
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
