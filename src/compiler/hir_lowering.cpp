#include "gti/hir.h"

namespace lang {

class HirLowerer::Impl {
public:
  explicit Impl(TargetInfo target) : target(std::move(target)) {}

  // Non-const: concrete instance reanalysis uses the analyzer's detach/
  // restore bracket (InstanceAnalysisScope) instead of copying it.
  [[nodiscard]] HirLoweringResult lower(const Program &source,
                                        SemanticVisitor &semantics) {
    analyzer = &semantics;
    baseModel = &semantics.model();
    output = {};
    output.program.executionProfile_ = target.executionProfile;
    output.program.semanticAnalysisSeal = baseModel->analysisSeal();
    if (!baseModel->analysisSeal().matchesProgram(source, target)) {
      output.program.valid_ = false;
      return std::move(output);
    }
    instanceIndex = HirInstanceIndex();
    nextValueId = 1;
    nextBindingId = 1;
    nextStatementId = 1;
    placeSnapshotId = baseModel->placeSnapshot();
    nextPlaceBodyId = 1;
    lifecycleValid = true;
    processedClasses = 0;
    processedFunctions = 0;
    processedConstructors = 0;
    processedDestructors = 0;
    lambdaTargets.clear();
    loweredProgramStorage.clear();
    functionAnalyses.clear();
    constructorAnalyses.clear();
    activeDefaultArguments.clear();

    seedDeclarations(source.declarations(), std::nullopt);
    lowerProgramInitializationPlan();
    lowerHostedProgramEntryPlan();
    finalizeLifetimes(output.program.moduleBody, *baseModel, false);
    lowerLoans(*baseModel, output.program.moduleBody);
    processPendingInstances();
    output.program.valid_ = output.diagnostics.empty() && lifecycleValid;
    if (output.program.valid_ &&
        !verifyHirProgramPlans(*baseModel, output.program).valid()) {
      output.program.valid_ = false;
    }
    return std::move(output);
  }

private:
  struct LoweredProgramStorage {
    HirBindingId binding = 0;
    std::optional<HirValueId> initializer;
    HirStatementId statement = 0;
  };

  [[nodiscard]] SemanticType
  substitute(const SemanticType &type,
             const GenericSubstitution &substitution) const {
    if (type.kind == SemanticType::TypeParameter) {
      const auto found = substitution.types.find(type.genericParameterId);
      if (found == substitution.types.end()) {
        return type;
      }
      return baseModel == nullptr
                 ? found->second
                 : baseModel->resolveExactClassSpecialization(found->second);
    }
    SemanticType result = type;
    for (SemanticType &argument : result.arguments) {
      argument = substitute(argument, substitution);
    }
    for (SemanticType &argument : result.lambdaEnclosingClassTypes) {
      argument = substitute(argument, substitution);
    }
    for (SemanticType &argument : result.lambdaEnclosingFunctionTypes) {
      argument = substitute(argument, substitution);
    }
    for (CompileTimeValue &argument : result.valueArguments) {
      if (argument.kind != CompileTimeValue::Parameter) {
        continue;
      }
      const auto found = substitution.values.find(argument.parameterId);
      if (found != substitution.values.end()) {
        argument = found->second;
      }
    }
    const auto substituteValues = [&](std::vector<CompileTimeValue> &values) {
      for (CompileTimeValue &argument : values) {
        if (argument.kind != CompileTimeValue::Parameter) {
          continue;
        }
        const auto found = substitution.values.find(argument.parameterId);
        if (found != substitution.values.end()) {
          argument = found->second;
        }
      }
    };
    substituteValues(result.lambdaEnclosingClassValues);
    substituteValues(result.lambdaEnclosingFunctionValues);
    if (result.arrayLengthParameterId != 0) {
      const auto found =
          substitution.values.find(result.arrayLengthParameterId);
      if (found != substitution.values.end() &&
          found->second.kind == CompileTimeValue::UInt64) {
        result.arrayLength = found->second.value;
        result.arrayLengthParameterId = 0;
      } else if (found != substitution.values.end() &&
                 found->second.kind == CompileTimeValue::Parameter) {
        result.arrayLengthParameterId = found->second.parameterId;
      }
    }
    return baseModel == nullptr
               ? result
               : baseModel->resolveExactClassSpecialization(std::move(result));
  }

  [[nodiscard]] static std::vector<const Expr *>
  omittedDefaultArguments(std::span<const Parameter> parameters,
                          std::size_t suppliedArguments) {
    std::vector<const Expr *> result;
    for (std::size_t index = suppliedArguments; index < parameters.size();
         ++index) {
      if (!parameters[index].defaultArgument ||
          !parameters[index].defaultArgument->expression) {
        return {};
      }
      result.push_back(parameters[index].defaultArgument->expression.get());
    }
    return result;
  }

  [[nodiscard]] GenericSubstitution
  classSubstitution(const ClassTypeInfo &declaration,
                    const std::vector<SemanticType> &typeArguments,
                    const std::vector<CompileTimeValue> &valueArguments) const {
    GenericSubstitution result;
    std::size_t typeIndex = 0;
    std::size_t valueIndex = 0;
    for (const GenericParameterInfo &parameter :
         declaration.genericParameters) {
      if (parameter.value) {
        if (valueIndex < valueArguments.size()) {
          result.values.emplace(parameter.id, valueArguments[valueIndex++]);
        }
      } else if (typeIndex < typeArguments.size()) {
        result.types.emplace(parameter.id, typeArguments[typeIndex++]);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<HirClassInstanceId>
  enqueueClass(const SemanticType &type,
               std::optional<SourceSpan> site = std::nullopt) {
    const SemanticType resolvedType =
        baseModel == nullptr ? type
                             : baseModel->resolveExactClassSpecialization(type);
    for (const SemanticType &argument : resolvedType.arguments) {
      (void)enqueueClass(argument);
    }
    if (resolvedType.kind != SemanticType::Class || resolvedType.classId == 0) {
      return std::nullopt;
    }
    if (const std::optional<std::size_t> existing = instanceIndex.findClass(
            resolvedType.classId, resolvedType.arguments,
            resolvedType.valueArguments)) {
      HirClassInstance &instance = output.program.classes[*existing - 1];
      if (!instance.instantiationSite && site) {
        instance.instantiationSite = std::move(site);
      }
      return *existing;
    }
    const ClassTypeInfo *declaration =
        baseModel->findClassType(resolvedType.classId);
    if (declaration == nullptr || declaration->declaration == nullptr) {
      return std::nullopt;
    }
    const ClassLifecycleInfo *lifecycle =
        baseModel->findClassLifecycle(*declaration->declaration);
    const HirClassInstanceId id = output.program.classes.size() + 1;
    output.program.classes.push_back(
        {.id = id,
         .sourceUnit = declaration->sourceUnit,
         .declaration = resolvedType.classId,
         .source = declaration->declaration,
         .typeArguments = resolvedType.arguments,
         .valueArguments = resolvedType.valueArguments,
         .instantiationSite = std::move(site),
         .type = resolvedType,
         .traits = analyzer->traitsFor(resolvedType),
         .transferPolicy = declaration->transferPolicy,
         .sharePolicy = declaration->sharePolicy,
         .kind = declaration->kind,
         .abstract = declaration->abstract,
         .polymorphic = declaration->polymorphic,
         .cAbiRecord = declaration->cAbiRecord,
         .cAbiLayout = declaration->cAbiLayout,
         .unionLayout = declaration->unionLayout,
         .defaultConstructor = lifecycle == nullptr
                                   ? SpecialMemberStatus::Deleted
                                   : lifecycle->defaultConstructor,
         .copyConstructor = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                 : lifecycle->copyConstructor,
         .moveConstructor = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                 : lifecycle->moveConstructor,
         .copyAssignment = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                : lifecycle->copyAssignment,
         .moveAssignment = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                : lifecycle->moveAssignment,
         .destructorStatus = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                  : lifecycle->destructor,
         .requiresActiveDropState =
             lifecycle != nullptr && lifecycle->requiresActiveDropState,
         .requiresActiveCleanup =
             analyzer->requiresActiveCleanupFor(resolvedType)});
    instanceIndex.recordClass(resolvedType.classId, resolvedType.arguments,
                              resolvedType.valueArguments, id);
    return id;
  }

  [[nodiscard]] HirFunctionInstanceId
  enqueueFunction(const FunctionInfo &declaration,
                  const std::vector<SemanticType> &classTypeArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  std::vector<SemanticType> functionTypeArguments,
                  std::vector<CompileTimeValue> functionValueArguments,
                  SemanticType returnType,
                  std::vector<SemanticType> parameterTypes,
                  std::optional<SourceSpan> site = std::nullopt) {
    // Free functions ignore class arguments, matching the previous scan.
    const bool freeFunction = declaration.ownerClass == 0;
    const std::vector<SemanticType> noTypeArguments;
    const std::vector<CompileTimeValue> noValueArguments;
    if (const std::optional<std::size_t> existing = instanceIndex.findFunction(
            declaration.id, freeFunction ? noTypeArguments : classTypeArguments,
            freeFunction ? noValueArguments : classValueArguments,
            functionTypeArguments, functionValueArguments)) {
      return *existing;
    }

    std::optional<HirClassInstanceId> owner;
    const Token &declarationToken =
        declaration.declaration != nullptr
            ? (declaration.declaration->operatorName()
                   ? declaration.declaration->operatorName()->symbol
                   : declaration.declaration->name())
            : Token{};
    const std::optional<SourceSpan> classUse =
        site ? site
             : (declarationToken.lexeme.empty()
                    ? std::optional<SourceSpan>{}
                    : std::optional<SourceSpan>{tokenSpan(declarationToken)});
    if (declaration.ownerClass != 0) {
      owner = enqueueClass(SemanticType::classType(declaration.ownerClass,
                                                   classTypeArguments,
                                                   classValueArguments),
                           classUse);
    }
    (void)enqueueClass(returnType, classUse);
    for (const SemanticType &parameter : parameterTypes) {
      (void)enqueueClass(parameter, classUse);
    }
    const HirFunctionInstanceId id = output.program.functions.size() + 1;
    if (freeFunction || owner.has_value()) {
      // An instance whose owner failed to resolve was never matchable by the
      // previous scan; leave it out of the index for exact equivalence.
      instanceIndex.recordFunction(
          declaration.id,
          freeFunction ? std::vector<SemanticType>{} : classTypeArguments,
          freeFunction ? std::vector<CompileTimeValue>{} : classValueArguments,
          functionTypeArguments, functionValueArguments, id);
    }
    output.program.functions.push_back(
        {.id = id,
         .sourceUnit = declaration.sourceUnit,
         .declaration = declaration.id,
         .source = declaration.declaration,
         .owner = owner,
         .typeArguments = std::move(functionTypeArguments),
         .valueArguments = std::move(functionValueArguments),
         .returnType = std::move(returnType),
         .parameterTypes = std::move(parameterTypes),
         .entryKind = declaration.entryKind,
         .returnBorrowOrigin = declaration.returnBorrowOrigin,
         .returnBorrowParameter = declaration.returnBorrowParameter,
         .returnBorrowAccess = declaration.returnBorrowAccess,
         .returnBorrowPlace = declaration.returnBorrowPlace,
         .instantiationSite = std::move(site),
         .staticMember = declaration.staticMember,
         .internalLinkage = declaration.internalLinkage,
         .constexprFunction = declaration.constexprFunction,
         .linkage = declaration.linkage,
         .externalSymbol = declaration.externalSymbol,
         .cArrayCountParameter = declaration.cArrayCountParameter,
         .virtualMethod = declaration.virtualMethod,
         .pureVirtual = declaration.pureVirtual,
         .overrideMethod = declaration.overrideMethod,
         .virtualRoots = declaration.virtualRoots});
    if (declaration.entryKind == ProgramEntryKind::OwnedArguments) {
      const FunctionInfo *append =
          baseModel->findFunction(declaration.entryArgumentAppendFunction);
      const HirFunctionInstance &entry = output.program.functions[id - 1];
      if (append != nullptr && entry.parameterTypes.size() == 2 &&
          entry.parameterTypes[1].kind == SemanticType::Class &&
          entry.parameterTypes[1].arguments.size() == 1) {
        const SemanticType argumentsType = entry.parameterTypes[1];
        const SemanticType argumentType = argumentsType.arguments.front();
        const HirFunctionInstanceId appendTarget = enqueueFunction(
            *append, argumentsType.arguments, argumentsType.valueArguments, {},
            {}, SemanticType::Void, {argumentType});
        output.program.functions[id - 1].entryArgumentAppendTarget =
            appendTarget;
      }
    }
    return id;
  }

  [[nodiscard]] HirNativeCallbackAdapterId
  enqueueNativeCallback(const NativeFunctionConversionInfo &conversion) {
    if (!conversion.type.hasNativeFunctionShape()) {
      lifecycleValid = false;
      return 0;
    }
    const FunctionInfo *target = baseModel->findFunction(conversion.function);
    const SemanticType *returnType = conversion.type.nativeFunctionReturnType();
    const std::span<const SemanticType> parameters =
        conversion.type.nativeFunctionParameterTypes();
    if (target == nullptr || target->declaration != conversion.declaration ||
        target->ownerClass != 0 || target->linkage != LanguageLinkage::Gti ||
        !target->genericParameters.empty() || target->parameterPack ||
        returnType == nullptr || target->returnType != *returnType ||
        target->parameterTypes.size() != parameters.size() ||
        !std::equal(target->parameterTypes.begin(),
                    target->parameterTypes.end(), parameters.begin())) {
      lifecycleValid = false;
      return 0;
    }
    const HirFunctionInstanceId function = enqueueFunction(
        *target, {}, {}, {}, {}, *returnType,
        std::vector<SemanticType>(parameters.begin(), parameters.end()));
    if (function == 0) {
      lifecycleValid = false;
      return 0;
    }
    const auto existing = std::find_if(
        output.program.nativeCallbacks.begin(),
        output.program.nativeCallbacks.end(),
        [&](const HirNativeCallbackAdapter &adapter) {
          return adapter.target == function && adapter.type == conversion.type;
        });
    if (existing != output.program.nativeCallbacks.end()) {
      return existing->id;
    }
    const HirNativeCallbackAdapterId id =
        output.program.nativeCallbacks.size() + 1;
    output.program.nativeCallbacks.push_back(
        {.id = id, .target = function, .type = conversion.type});
    return id;
  }

  [[nodiscard]] HirConstructorInstanceId
  enqueueConstructor(const ResolvedConstructionInfo &construction,
                     std::optional<SourceSpan> site = std::nullopt) {
    const std::optional<HirClassInstanceId> owner =
        enqueueClass(construction.constructedType, site);
    if (!owner || construction.kind != ConstructorKind::Ordinary ||
        construction.declaration == nullptr || construction.constructor == 0) {
      return 0;
    }
    if (const std::optional<std::size_t> existing =
            instanceIndex.findConstructor(construction.constructor, *owner,
                                          construction.typeArguments,
                                          construction.valueArguments)) {
      HirConstructorInstance &instance =
          output.program.constructors[*existing - 1];
      if (construction.borrowOrigin != BorrowOriginKind::None) {
        instance.borrowOrigin = construction.borrowOrigin;
        instance.borrowParameter = construction.borrowArgument;
        instance.borrowAccess = construction.borrowAccess;
      }
      return instance.id;
    }
    const HirConstructorInstanceId id = output.program.constructors.size() + 1;
    output.program.constructors.push_back(
        {.id = id,
         .sourceUnit = output.program.classes[*owner - 1].sourceUnit,
         .declaration = construction.constructor,
         .source = construction.declaration,
         .owner = *owner,
         .typeArguments = construction.typeArguments,
         .valueArguments = construction.valueArguments,
         .parameterTypes = construction.parameterTypes,
         .borrowOrigin = construction.borrowOrigin,
         .borrowParameter = construction.borrowArgument,
         .borrowAccess = construction.borrowAccess,
         .instantiationSite = std::move(site)});
    instanceIndex.recordConstructor(construction.constructor, *owner,
                                    construction.typeArguments,
                                    construction.valueArguments, id);
    return id;
  }

  [[nodiscard]] std::optional<HirDestructorInstanceId>
  enqueueDestructor(HirClassInstanceId owner) {
    if (owner == 0 || owner > output.program.classes.size()) {
      return std::nullopt;
    }
    const HirClassInstance &classInstance = output.program.classes[owner - 1];
    const ClassTypeInfo *classType =
        baseModel->findClassType(classInstance.declaration);
    const ClassLifecycleInfo *lifecycle =
        classType == nullptr || classType->declaration == nullptr
            ? nullptr
            : baseModel->findClassLifecycle(*classType->declaration);
    if (lifecycle == nullptr || !lifecycle->declaredDestructor ||
        lifecycle->declaredDestructor->declaration == nullptr) {
      return std::nullopt;
    }
    if (const std::optional<std::size_t> existing =
            instanceIndex.findDestructor(owner)) {
      return *existing;
    }
    const HirDestructorInstanceId id = output.program.destructors.size() + 1;
    output.program.destructors.push_back(
        {.id = id,
         .sourceUnit = classInstance.sourceUnit,
         .source = lifecycle->declaredDestructor->declaration,
         .owner = owner});
    instanceIndex.recordDestructor(owner, id);
    return id;
  }

  void seedDeclarations(const StmtList &declarations,
                        std::optional<ClassId> enclosingClass) {
    for (const StmtPtr &statement : declarations) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          seedDeclarations(*branch, enclosingClass);
        }
        continue;
      }
      if (const auto *namespaceDeclaration =
              dynamic_cast<const NamespaceDecl *>(statement.get())) {
        seedDeclarations(namespaceDeclaration->declarations(), enclosingClass);
        continue;
      }
      if (const auto *externC =
              dynamic_cast<const ExternCDecl *>(statement.get())) {
        seedDeclarations(externC->declarations(), enclosingClass);
        continue;
      }
      if (const auto *classDeclaration =
              dynamic_cast<const ClassDecl *>(statement.get())) {
        const ClassTypeInfo *classInfo =
            baseModel->findClassType(*classDeclaration);
        if (classInfo != nullptr && classInfo->genericParameters.empty()) {
          (void)enqueueClass(SemanticType::classType(classInfo->id));
        }
        seedDeclarations(classDeclaration->members(),
                         classInfo == nullptr
                             ? std::optional<ClassId>{}
                             : std::optional<ClassId>{classInfo->id});
        continue;
      }
      if (const auto *enumDeclaration =
              dynamic_cast<const EnumDecl *>(statement.get())) {
        const EnumTypeInfo *info = baseModel->findEnumType(*enumDeclaration);
        if (info != nullptr) {
          HirEnum lowered{.declaration = info->id,
                          .sourceUnit = info->sourceUnit,
                          .source = enumDeclaration,
                          .qualifiedName = info->qualifiedName,
                          .underlyingType = info->underlyingType,
                          .payload = info->payload};
          lowered.enumerators.reserve(info->enumerators.size());
          for (const EnumeratorInfo &enumerator : info->enumerators) {
            lowered.enumerators.push_back(
                {.source = enumerator.declaration,
                 .value = enumerator.value,
                 .variantIndex = enumerator.variantIndex,
                 .payloadTypes = enumerator.payloadTypes});
          }
          output.program.enums.push_back(std::move(lowered));
        }
        continue;
      }
      if (const auto *function =
              dynamic_cast<const FunctionDecl *>(statement.get())) {
        const FunctionInfo *info = baseModel->findFunction(*function);
        if (info == nullptr || !info->genericParameters.empty()) {
          continue;
        }
        std::vector<SemanticType> classArguments;
        if (info->ownerClass != 0) {
          const ClassTypeInfo *owner =
              baseModel->findClassType(info->ownerClass);
          if (owner == nullptr || !owner->genericParameters.empty()) {
            continue;
          }
        }
        (void)enqueueFunction(*info, classArguments, {}, {}, {},
                              info->returnType, info->parameterTypes);
        continue;
      }
      // Program-wide storage is lowered once from the semantic initialization
      // plan after declaration seeding. Its order is not the combined AST
      // visitation order, and class statics share the module body with globals.
    }
  }

  void lowerProgramInitializationPlan() {
    const ProgramInitializationPlan &semanticPlan =
        baseModel->programInitializationPlan();
    output.program.programInitialization.unitOrder = semanticPlan.unitOrder;
    output.program.programInitialization.steps.reserve(
        semanticPlan.steps.size());
    for (const ProgramInitializationStep &step : semanticPlan.steps) {
      const VariableDecl *declaration = step.declaration;
      const BindingInfo *info = declaration == nullptr
                                    ? nullptr
                                    : baseModel->findBinding(*declaration);
      HirClassInstanceId ownerClass = 0;
      if (step.kind == ProgramStorageKind::StaticField) {
        ownerClass =
            enqueueClass(SemanticType::classType(step.ownerClass)).value_or(0);
      }
      if (step.id == 0 || declaration == nullptr || info == nullptr ||
          step.symbol == 0 ||
          (step.kind == ProgramStorageKind::StaticField && ownerClass == 0)) {
        lifecycleValid = false;
        continue;
      }
      const HirBindingId binding =
          lowerBinding(*declaration, *info, output.program.moduleBody);
      std::optional<HirValueId> initializer;
      HirStatementId statement = 0;
      if (step.role == ProgramInitializationStepRole::Initializer) {
        initializer = lowerExpression(declaration->initializer(), *baseModel,
                                      {}, {}, output.program.moduleBody);
        statement = appendStatement({.kind = HirStatementKind::Variable,
                                     .source = declaration,
                                     .binding = binding,
                                     .value = initializer},
                                    output.program.moduleBody);
        output.program.moduleBody.roots.push_back(statement);
      }
      const LoweredProgramStorage lowered{.binding = binding,
                                          .initializer = initializer,
                                          .statement = statement};
      loweredProgramStorage.insert_or_assign(declaration, lowered);
      output.program.programInitialization.steps.push_back(
          {.id = step.id,
           .sourceUnit = step.sourceUnit,
           .kind = step.kind,
           .role = step.role,
           .source = declaration,
           .symbol = step.symbol,
           .ownerClass = ownerClass,
           .requiresActiveCleanup = step.requiresActiveCleanup,
           .binding = binding,
           .initializer = initializer,
           .statement = statement});
    }
    if (output.program.programInitialization.steps.size() !=
        semanticPlan.steps.size()) {
      lifecycleValid = false;
    }
  }

  [[nodiscard]] const ConstructorInfo *
  findHostedConstructor(const SemanticType &type, ConstructorId id) const {
    const ClassTypeInfo *classType = baseModel->findClassType(type.classId);
    const ClassLifecycleInfo *lifecycle =
        classType == nullptr || classType->declaration == nullptr
            ? nullptr
            : baseModel->findClassLifecycle(*classType->declaration);
    if (lifecycle == nullptr) {
      return nullptr;
    }
    const auto found = std::find_if(
        lifecycle->constructors.begin(), lifecycle->constructors.end(),
        [id](const ConstructorInfo &constructor) {
          return constructor.id == id &&
                 constructor.kind == ConstructorKind::Ordinary &&
                 constructor.access == AccessModifier::Public &&
                 constructor.declaration != nullptr;
        });
    return found == lifecycle->constructors.end() ? nullptr : &*found;
  }

  [[nodiscard]] HirConstructorInstanceId
  enqueueHostedConstructor(const SemanticType &type, ConstructorId id,
                           const SourceSpan &site) {
    const ConstructorInfo *constructor = findHostedConstructor(type, id);
    if (constructor == nullptr) {
      return 0;
    }
    return enqueueConstructor({.constructor = constructor->id,
                               .declaration = constructor->declaration,
                               .constructedType = type,
                               .typeArguments = type.arguments,
                               .valueArguments = type.valueArguments,
                               .parameterTypes = constructor->parameterTypes},
                              site);
  }

  void lowerHostedProgramEntryPlan() {
    const std::optional<HostedProgramEntryPlan> &semanticPlan =
        baseModel->hostedProgramEntryPlan();
    if (!semanticPlan) {
      output.program.hostedProgramEntry.reset();
      return;
    }
    const FunctionInfo *entryInfo =
        baseModel->findFunction(semanticPlan->entry);
    const std::optional<std::size_t> entryInstance =
        entryInfo == nullptr
            ? std::nullopt
            : instanceIndex.findFunction(entryInfo->id, {}, {}, {}, {});
    HirHostedProgramEntryPlan lowered{
        .semanticEntry = semanticPlan->entry,
        .semanticAppendFunction = semanticPlan->appendFunction,
        .semanticVectorConstructor = semanticPlan->vectorConstructor,
        .semanticStringConstructor = semanticPlan->stringConstructor,
        .entry = entryInstance.value_or(0),
        .kind = semanticPlan->kind,
        .sourceUnit = semanticPlan->sourceUnit,
        .mainAnchor = semanticPlan->mainAnchor,
        .validateCount = semanticPlan->validateCount,
        .convertCount = semanticPlan->convertCount};
    if (semanticPlan->kind == ProgramEntryKind::NoArguments) {
      if (lowered.entry == 0 || semanticPlan->appendFunction != 0 ||
          semanticPlan->vectorConstructor != 0 ||
          semanticPlan->stringConstructor != 0 ||
          !semanticPlan->validateCount.empty() ||
          !semanticPlan->convertCount.empty()) {
        lifecycleValid = false;
      }
      output.program.hostedProgramEntry = std::move(lowered);
      return;
    }
    if (semanticPlan->kind != ProgramEntryKind::OwnedArguments ||
        entryInfo == nullptr || entryInfo->parameterTypes.size() != 2 ||
        entryInfo->parameterTypes[1].kind != SemanticType::Class ||
        entryInfo->parameterTypes[1].arguments.size() != 1 ||
        lowered.entry == 0) {
      lifecycleValid = false;
      output.program.hostedProgramEntry = std::move(lowered);
      return;
    }
    const SemanticType &vectorType = entryInfo->parameterTypes[1];
    const SemanticType &stringType = vectorType.arguments.front();
    const HirFunctionInstance *entry =
        output.program.findFunctionInstance(lowered.entry);
    lowered.appendFunction =
        entry != nullptr && entry->entryArgumentAppendTarget
            ? *entry->entryArgumentAppendTarget
            : 0;
    lowered.vectorConstructor = enqueueHostedConstructor(
        vectorType, semanticPlan->vectorConstructor, semanticPlan->mainAnchor);
    lowered.stringConstructor = enqueueHostedConstructor(
        stringType, semanticPlan->stringConstructor, semanticPlan->mainAnchor);
    const HirFunctionInstance *append =
        output.program.findFunctionInstance(lowered.appendFunction);
    if (semanticPlan->appendFunction == 0 ||
        semanticPlan->vectorConstructor == 0 ||
        semanticPlan->stringConstructor == 0 || lowered.appendFunction == 0 ||
        lowered.vectorConstructor == 0 || lowered.stringConstructor == 0 ||
        append == nullptr ||
        append->declaration != semanticPlan->appendFunction ||
        semanticPlan->validateCount.empty() ||
        semanticPlan->convertCount.empty()) {
      lifecycleValid = false;
    }
    output.program.hostedProgramEntry = std::move(lowered);
  }

  void processPendingInstances() {
    while (processedClasses < output.program.classes.size() ||
           processedFunctions < output.program.functions.size() ||
           processedConstructors < output.program.constructors.size() ||
           processedDestructors < output.program.destructors.size()) {
      while (processedClasses < output.program.classes.size()) {
        processClass(processedClasses++);
      }
      enqueueVirtualContractRoots();
      while (processedFunctions < output.program.functions.size()) {
        processFunction(processedFunctions++);
      }
      while (processedConstructors < output.program.constructors.size()) {
        processConstructor(processedConstructors++);
      }
      while (processedDestructors < output.program.destructors.size()) {
        processDestructor(processedDestructors++);
      }
    }
  }

  [[nodiscard]] const HirClassInstance *
  findClassInHierarchy(HirClassInstanceId instance, ClassId declaration,
                       std::unordered_set<HirClassInstanceId> &seen) const {
    if (instance == 0 || instance > output.program.classes.size() ||
        !seen.insert(instance).second) {
      return nullptr;
    }
    const HirClassInstance &candidate = output.program.classes[instance - 1];
    if (candidate.declaration == declaration) {
      return &candidate;
    }
    const HirClassInstance *found = nullptr;
    for (const HirBaseInstance &base : candidate.bases) {
      const HirClassInstance *match =
          findClassInHierarchy(base.instance, declaration, seen);
      if (match == nullptr) {
        continue;
      }
      if (found != nullptr && found->id != match->id) {
        return nullptr;
      }
      found = match;
    }
    return found;
  }

  // A concrete override carries its pure virtual roots even when source only
  // calls the override through a concrete value. Materialize those exact root
  // instances now so MIR has one complete virtual failure contract instead of
  // relying on a separate base-typed call to discover the interface member.
  void enqueueVirtualContractRoots() {
    const std::size_t functionCount = output.program.functions.size();
    for (std::size_t index = 0; index < functionCount; ++index) {
      const HirFunctionInstance override = output.program.functions[index];
      if (!override.owner || !override.virtualMethod || override.pureVirtual ||
          !override.overrideMethod || override.virtualRoots.empty()) {
        continue;
      }
      for (const FunctionId rootId : override.virtualRoots) {
        const FunctionInfo *root = baseModel->findFunction(rootId);
        const ClassTypeInfo *rootOwner =
            root == nullptr ? nullptr
                            : baseModel->findClassType(root->ownerClass);
        if (root == nullptr || rootOwner == nullptr || !root->virtualMethod ||
            !root->pureVirtual || root->overrideMethod ||
            !root->genericParameters.empty()) {
          continue;
        }
        std::unordered_set<HirClassInstanceId> seen;
        const HirClassInstance *owner =
            findClassInHierarchy(*override.owner, root->ownerClass, seen);
        if (owner == nullptr) {
          continue;
        }
        const std::vector<SemanticType> ownerTypes = owner->typeArguments;
        const std::vector<CompileTimeValue> ownerValues = owner->valueArguments;
        const GenericSubstitution substitution =
            classSubstitution(*rootOwner, ownerTypes, ownerValues);
        std::vector<SemanticType> parameterTypes;
        parameterTypes.reserve(root->parameterTypes.size());
        for (const SemanticType &parameter : root->parameterTypes) {
          parameterTypes.push_back(substitute(parameter, substitution));
        }
        (void)enqueueFunction(*root, ownerTypes, ownerValues, {}, {},
                              substitute(root->returnType, substitution),
                              std::move(parameterTypes),
                              override.instantiationSite);
      }
    }
  }

  [[nodiscard]] static bool lambdaMatchesType(const HirLambda &lambda,
                                              const SemanticType &type) {
    return type.kind == SemanticType::Lambda && lambda.type == type;
  }

  void seedLambdaTarget(const SemanticType &type) {
    if (type.kind == SemanticType::Lambda) {
      const auto existing = std::find_if(
          output.program.lambdas.begin(), output.program.lambdas.end(),
          [&](const HirLambda &candidate) {
            return lambdaMatchesType(candidate, type);
          });
      if (existing != output.program.lambdas.end()) {
        lambdaTargets.insert_or_assign(type.lambdaId, existing->id);
      }
    }
    for (const SemanticType &argument : type.arguments) {
      seedLambdaTarget(argument);
    }
  }

  void seedLambdaTargets(const std::vector<SemanticType> &types) {
    for (const SemanticType &type : types) {
      seedLambdaTarget(type);
    }
  }

  [[nodiscard]] static bool containsLambdaType(const SemanticType &type) {
    return type.kind == SemanticType::Lambda ||
           std::any_of(type.arguments.begin(), type.arguments.end(),
                       containsLambdaType);
  }

  void enqueueVirtualMembers(
      const StmtList &members, const GenericSubstitution &substitution,
      const std::vector<SemanticType> &classTypeArguments,
      const std::vector<CompileTimeValue> &classValueArguments) {
    for (const StmtPtr &member : members) {
      if (const auto *conditional =
              dynamic_cast<const ConditionalStmt *>(member.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          enqueueVirtualMembers(*branch, substitution, classTypeArguments,
                                classValueArguments);
        }
        continue;
      }
      const auto *function = dynamic_cast<const FunctionDecl *>(member.get());
      const FunctionInfo *info =
          function == nullptr ? nullptr : baseModel->findFunction(*function);
      if (info == nullptr || !info->virtualMethod || info->ownerClass == 0 ||
          !info->genericParameters.empty()) {
        continue;
      }
      std::vector<SemanticType> parameterTypes;
      parameterTypes.reserve(info->parameterTypes.size());
      for (const SemanticType &parameter : info->parameterTypes) {
        parameterTypes.push_back(substitute(parameter, substitution));
      }
      (void)enqueueFunction(*info, classTypeArguments, classValueArguments, {},
                            {}, substitute(info->returnType, substitution),
                            std::move(parameterTypes));
    }
  }

  void processClass(std::size_t index) {
    lambdaTargets.clear();
    const HirClassInstance snapshot = output.program.classes[index];
    seedLambdaTargets(snapshot.typeArguments);
    const ClassTypeInfo *declaration =
        baseModel->findClassType(snapshot.declaration);
    if (declaration == nullptr) {
      return;
    }
    const GenericSubstitution substitution = classSubstitution(
        *declaration, snapshot.typeArguments, snapshot.valueArguments);
    std::vector<HirBaseInstance> bases;
    bases.reserve(declaration->bases.size());
    for (const ClassBaseTypeInfo &base : declaration->bases) {
      const SemanticType type = substitute(base.type, substitution);
      const std::optional<HirClassInstanceId> instance = enqueueClass(type);
      bases.push_back({.instance = instance.value_or(0),
                       .type = type,
                       .interface = base.interface});
    }
    output.program.classes[index].bases = std::move(bases);
    enqueueVirtualMembers(declaration->declaration->members(), substitution,
                          snapshot.typeArguments, snapshot.valueArguments);
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!snapshot.typeArguments.empty() || !snapshot.valueArguments.empty()) {
      analysis = analyzer->analyzeClassFieldInitializers(
          snapshot.declaration, snapshot.typeArguments,
          snapshot.valueArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics),
                                snapshot.instantiationSite);
      model = &analysis.model;
    }
    std::vector<HirClassField> fields;
    HirBody fieldInitializers;
    fields.reserve(declaration->fields.size());
    for (const ClassFieldTypeInfo &field : declaration->fields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      BindingInfo info{.type = type,
                       .access = field.declaration != nullptr &&
                                         field.declaration->isMutable()
                                     ? AccessMode::Mutable
                                     : AccessMode::ReadOnly,
                       .traits = analyzer->traitsFor(type)};
      if (field.declaration != nullptr) {
        if (const BindingInfo *recorded =
                model->findBinding(*field.declaration)) {
          info.symbol = recorded->symbol;
          info.constant = recorded->constant;
        }
      }
      const HirBindingId binding =
          field.declaration == nullptr
              ? 0
              : lowerBinding(*field.declaration, info, fieldInitializers);
      const std::optional<HirValueId> initializer =
          field.declaration == nullptr
              ? std::nullopt
              : lowerExpression(field.declaration->initializer(), *model,
                                snapshot.typeArguments, snapshot.valueArguments,
                                fieldInitializers);
      if (field.declaration != nullptr) {
        HirStatement statement{.kind = HirStatementKind::Variable,
                               .source = field.declaration,
                               .binding = binding,
                               .value = initializer};
        fieldInitializers.roots.push_back(
            appendStatement(std::move(statement), fieldInitializers));
      }
      fields.push_back(
          {.declaration = field.declaration,
           .binding = binding,
           .initializer = initializer,
           .info = info,
           .requiresActiveCleanup = analyzer->requiresActiveCleanupFor(type)});
    }
    output.program.classes[index].fields = std::move(fields);
    finalizeLifetimes(fieldInitializers, *model, false);
    lowerLoans(*model, fieldInitializers);
    output.program.classes[index].fieldInitializers =
        std::move(fieldInitializers);
    std::vector<HirClassField> staticFields;
    HirBody staticFieldInitializers;
    staticFields.reserve(declaration->staticFields.size());
    for (const ClassFieldTypeInfo &field : declaration->staticFields) {
      const SemanticType type = substitute(field.type, substitution);
      (void)enqueueClass(type);
      BindingInfo info{.type = type,
                       .access = field.declaration != nullptr &&
                                         field.declaration->isMutable()
                                     ? AccessMode::Mutable
                                     : AccessMode::ReadOnly,
                       .traits = analyzer->traitsFor(type),
                       .staticStorage = true};
      if (field.declaration != nullptr) {
        if (const BindingInfo *recorded =
                model->findBinding(*field.declaration)) {
          info.symbol = recorded->symbol;
          info.constant = recorded->constant;
        }
      }
      LoweredProgramStorage lowered;
      if (field.declaration != nullptr) {
        const auto found = loweredProgramStorage.find(field.declaration);
        if (found == loweredProgramStorage.end()) {
          // Generic class statics are intentionally outside the whole-program
          // semantic plan until a concrete qualified-static identity exists.
          // Preserve their previous per-instance body representation.
          lowered.binding =
              lowerBinding(*field.declaration, info, staticFieldInitializers);
          lowered.initializer = lowerExpression(
              field.declaration->initializer(), *model, snapshot.typeArguments,
              snapshot.valueArguments, staticFieldInitializers);
          lowered.statement =
              appendStatement({.kind = HirStatementKind::Variable,
                               .source = field.declaration,
                               .binding = lowered.binding,
                               .value = lowered.initializer},
                              staticFieldInitializers);
          staticFieldInitializers.roots.push_back(lowered.statement);
          if (declaration->genericParameters.empty()) {
            lifecycleValid = false;
          }
        } else {
          lowered = found->second;
        }
      }
      staticFields.push_back(
          {.declaration = field.declaration,
           .binding = lowered.binding,
           .initializer = lowered.initializer,
           .info = info,
           .requiresActiveCleanup = analyzer->requiresActiveCleanupFor(type)});
    }
    output.program.classes[index].staticFields = std::move(staticFields);
    finalizeLifetimes(staticFieldInitializers, *model, false);
    lowerLoans(*model, staticFieldInitializers);
    output.program.classes[index].staticFieldInitializers =
        std::move(staticFieldInitializers);
    output.program.classes[index].destructor = enqueueDestructor(snapshot.id);
  }

  void appendInstanceDiagnostics(std::vector<Diagnostic> diagnostics,
                                 const std::optional<SourceSpan> &site) {
    for (Diagnostic &diagnostic : diagnostics) {
      if (site) {
        diagnostic.related.push_back(
            {*site, "Concrete generic instance requested here."});
      }
      output.diagnostics.push_back(std::move(diagnostic));
    }
  }

  [[nodiscard]] SemanticInstanceAnalysis *
  ensureFunctionAnalysis(HirFunctionInstanceId id) {
    if (id == 0 || id > output.program.functions.size()) {
      return nullptr;
    }
    if (const auto found = functionAnalyses.find(id);
        found != functionAnalyses.end()) {
      return found->second.get();
    }
    const HirFunctionInstance &snapshot = output.program.functions[id - 1];
    const FunctionInfo *declaration =
        baseModel->findFunction(snapshot.declaration);
    if (declaration == nullptr) {
      return nullptr;
    }
    std::vector<SemanticType> classArguments;
    std::vector<CompileTimeValue> classValueArguments;
    if (snapshot.owner && *snapshot.owner <= output.program.classes.size()) {
      const HirClassInstance &owner =
          output.program.classes[*snapshot.owner - 1];
      classArguments = owner.typeArguments;
      classValueArguments = owner.valueArguments;
    }
    const bool concreteInstance =
        !declaration->genericParameters.empty() || !classArguments.empty() ||
        !classValueArguments.empty() || !snapshot.typeArguments.empty() ||
        !snapshot.valueArguments.empty();
    if (!concreteInstance) {
      return nullptr;
    }
    auto analysis = std::make_unique<SemanticInstanceAnalysis>(
        analyzer->analyzeFunctionInstance(
            declaration->id, classArguments, classValueArguments,
            snapshot.typeArguments, snapshot.valueArguments));
    appendInstanceDiagnostics(std::move(analysis->diagnostics),
                              snapshot.instantiationSite);
    SemanticInstanceAnalysis *result = analysis.get();
    functionAnalyses.emplace(id, std::move(analysis));
    return result;
  }

  [[nodiscard]] SemanticInstanceAnalysis *
  ensureConstructorAnalysis(HirConstructorInstanceId id) {
    if (id == 0 || id > output.program.constructors.size()) {
      return nullptr;
    }
    if (const auto found = constructorAnalyses.find(id);
        found != constructorAnalyses.end()) {
      return found->second.get();
    }
    const HirConstructorInstance &snapshot =
        output.program.constructors[id - 1];
    if (snapshot.owner == 0 || snapshot.owner > output.program.classes.size()) {
      return nullptr;
    }
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    auto analysis = std::make_unique<SemanticInstanceAnalysis>(
        analyzer->analyzeConstructorInstance(
            snapshot.declaration, owner.typeArguments, owner.valueArguments,
            snapshot.typeArguments, snapshot.valueArguments));
    appendInstanceDiagnostics(std::move(analysis->diagnostics),
                              snapshot.instantiationSite);
    SemanticInstanceAnalysis *result = analysis.get();
    constructorAnalyses.emplace(id, std::move(analysis));
    return result;
  }

  void processFunction(std::size_t index) {
    const HirFunctionInstance snapshot = output.program.functions[index];
    const FunctionInfo *declaration =
        baseModel->findFunction(snapshot.declaration);
    if (declaration == nullptr || declaration->declaration == nullptr) {
      return;
    }
    std::vector<SemanticType> classArguments;
    std::vector<CompileTimeValue> classValueArguments;
    if (snapshot.owner) {
      const HirClassInstance &owner =
          output.program.classes[*snapshot.owner - 1];
      classArguments = owner.typeArguments;
      classValueArguments = owner.valueArguments;
    }
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    if (snapshot.owner) {
      currentReceiverType = output.program.classes[*snapshot.owner - 1].type;
      currentReceiverAccess =
          receiverAllowsMutation(declaration->declaration->receiverMutability())
              ? AccessMode::Mutable
              : AccessMode::ReadOnly;
    }

    // Generic declarations are only enqueued after selection. An empty type
    // argument vector can therefore mean a concrete zero-element pack, not an
    // unresolved declaration; reanalyze it so the pack binding retains that
    // exact concrete-empty identity.
    const bool concreteInstance =
        !declaration->genericParameters.empty() || !classArguments.empty() ||
        !classValueArguments.empty() || !snapshot.typeArguments.empty() ||
        !snapshot.valueArguments.empty();
    const SemanticModel *model = baseModel;
    if (concreteInstance) {
      SemanticInstanceAnalysis *analysis = ensureFunctionAnalysis(snapshot.id);
      if (analysis == nullptr) {
        lifecycleValid = false;
        currentReceiverType = enclosingReceiverType;
        currentReceiverAccess = enclosingReceiverAccess;
        return;
      }
      model = &analysis->model;
    }

    lambdaTargets.clear();
    seedLambdaTargets(snapshot.typeArguments);
    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(declaration->declaration->parameters().size());
    for (const Parameter &parameter : declaration->declaration->parameters()) {
      parameterBindings.push_back(lowerBinding(parameter, *model, body));
    }
    if (declaration->declaration->body()) {
      body.roots =
          lowerStatements(declaration->declaration->body()->statements(),
                          *model, classArguments, classValueArguments, body);
    }
    finalizeLifetimes(body, *model, true);
    lowerLoans(*model, body);
    std::vector<HirCallableParameter> callableParameters;
    callableParameters.reserve(declaration->callableParameters.size());
    for (const CallableParameterContract &parameter :
         declaration->callableParameters) {
      if (parameter.parameterIndex >= snapshot.parameterTypes.size() ||
          (parameter.boundary == CallableBoundary::Owned &&
           !containsLambdaType(
               snapshot.parameterTypes[parameter.parameterIndex]))) {
        continue;
      }
      HirCallableParameter lowered{
          .parameterIndex = parameter.parameterIndex,
          .callableType = snapshot.parameterTypes[parameter.parameterIndex],
          .access = parameter.access,
          .boundary = parameter.boundary,
          .ownedTransport = parameter.ownedTransport};
      if (lowered.ownedTransport) {
        lowered.ownedTransport->destinationType = snapshot.returnType;
      }
      lowered.signatures.reserve(parameter.signatures.size());
      for (const CallableSignatureRequirement &signature :
           parameter.signatures) {
        HirCallableSignature concrete{
            .source = signature.source,
            .returnType = signature.returnType,
            .parameterTypes = signature.parameterTypes,
            .requiredCapability = signature.capability};
        if (signature.source != nullptr) {
          if (const ResolvedLambdaCallInfo *resolved =
                  model->findLambdaCall(*signature.source)) {
            concrete.returnType = resolved->returnType;
            concrete.parameterTypes = resolved->parameterTypes;
            concrete.selectedCapability = resolved->capability;
          } else if (const ResolvedOperatorInfo *resolved =
                         model->findOperator(*signature.source)) {
            concrete.returnType = resolved->returnType;
            concrete.parameterTypes = resolved->parameterTypes;
            concrete.selectedCapability = resolved->capability;
          }
          const auto value =
              std::find_if(body.values.begin(), body.values.end(),
                           [&](const HirValue &candidate) {
                             return candidate.source == signature.source;
                           });
          if (value != body.values.end()) {
            concrete.functionTarget = value->functionTarget;
            concrete.lambdaTarget = value->lambdaTarget;
          }
        }
        lowered.signatures.emplace_back(std::move(concrete));
      }
      lowered.forwardings.reserve(parameter.forwardings.size());
      for (const CallableForwardingRequirement &forwarding :
           parameter.forwardings) {
        HirCallableForwarding concrete{.source = forwarding.source,
                                       .parameterIndex =
                                           forwarding.parameterIndex};
        if (forwarding.source != nullptr) {
          const auto value =
              std::find_if(body.values.begin(), body.values.end(),
                           [&](const HirValue &candidate) {
                             return candidate.source == forwarding.source;
                           });
          if (value != body.values.end()) {
            concrete.functionTarget = value->functionTarget;
          }
        }
        const bool duplicate = std::any_of(
            lowered.forwardings.begin(), lowered.forwardings.end(),
            [&](const HirCallableForwarding &existing) {
              return existing.parameterIndex == concrete.parameterIndex &&
                     existing.functionTarget == concrete.functionTarget;
            });
        if (!duplicate) {
          lowered.forwardings.emplace_back(std::move(concrete));
        }
      }
      callableParameters.emplace_back(std::move(lowered));
    }
    std::sort(callableParameters.begin(), callableParameters.end(),
              [](const HirCallableParameter &left,
                 const HirCallableParameter &right) {
                return left.parameterIndex < right.parameterIndex;
              });
    output.program.functions[index].body = std::move(body);
    output.program.functions[index].parameterBindings =
        std::move(parameterBindings);
    output.program.functions[index].callableParameters =
        std::move(callableParameters);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  void processConstructor(std::size_t index) {
    const HirConstructorInstance snapshot = output.program.constructors[index];
    const HirClassInstance owner = output.program.classes[snapshot.owner - 1];
    const std::vector<SemanticType> &classArguments = owner.typeArguments;
    const std::vector<CompileTimeValue> &classValueArguments =
        owner.valueArguments;
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis *analysis = ensureConstructorAnalysis(snapshot.id);
    if (analysis == nullptr) {
      lifecycleValid = false;
      currentReceiverType = enclosingReceiverType;
      currentReceiverAccess = enclosingReceiverAccess;
      return;
    }

    lambdaTargets.clear();
    seedLambdaTargets(classArguments);
    seedLambdaTargets(snapshot.typeArguments);
    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(snapshot.source->parameters().size());
    for (const Parameter &parameter : snapshot.source->parameters()) {
      parameterBindings.push_back(
          lowerBinding(parameter, analysis->model, body));
    }
    std::vector<HirConstructorInitializer> initializers;
    std::vector<HirValueId> initializerValues;
    bool hasExplicitBase = false;
    for (const ConstructorInitializer &initializer :
         snapshot.source->initializers()) {
      HirConstructorInitializer lowered{.source = &initializer,
                                        .explicitArgumentCount =
                                            initializer.arguments.size()};
      const ResolvedConstructorInitializerInfo *resolved =
          analysis->model.findConstructorInitializer(initializer);
      if (resolved != nullptr) {
        lowered.kind = resolved->kind;
        lowered.targetType = resolved->targetType;
        lowered.field = resolved->field;
        lowered.storesReference = resolved->storesReference;
        lowered.borrowAccess = resolved->borrowAccess;
        lowered.generatedDefault = resolved->generatedDefault;
        lowered.ownedParameter = resolved->ownedParameter;
        if (resolved->kind == ConstructorInitializerTargetKind::Base) {
          hasExplicitBase = true;
          lowered.base = enqueueClass(resolved->targetType);
          const HirConstructorInstanceId target =
              enqueueConstructor(ResolvedConstructionInfo{
                  .constructor = resolved->constructor,
                  .declaration = resolved->declaration,
                  .constructedType = resolved->targetType,
                  .parameterTypes = resolved->parameterTypes,
                  .defaultArguments = resolved->defaultArguments,
                  .generatedDefault = resolved->generatedDefault});
          if (target != 0) {
            lowered.constructorTarget = target;
          }
        }
      }
      for (const ExprPtr &argument : initializer.arguments) {
        if (const std::optional<HirValueId> value =
                lowerExpression(argument, analysis->model, classArguments,
                                classValueArguments, body)) {
          lowered.arguments.push_back(*value);
          initializerValues.push_back(*value);
        }
      }
      if (resolved != nullptr &&
          resolved->kind == ConstructorInitializerTargetKind::Base &&
          !resolved->defaultArguments.empty() && lowered.constructorTarget) {
        const HirConstructorInstanceId target = *lowered.constructorTarget;
        const HirConstructorInstance &targetInstance =
            output.program.constructors[target - 1];
        std::vector<SemanticType> targetClassArguments;
        std::vector<CompileTimeValue> targetClassValueArguments;
        if (targetInstance.owner != 0 &&
            targetInstance.owner <= output.program.classes.size()) {
          const HirClassInstance &targetOwner =
              output.program.classes[targetInstance.owner - 1];
          targetClassArguments = targetOwner.typeArguments;
          targetClassValueArguments = targetOwner.valueArguments;
        }
        const SemanticInstanceAnalysis *targetAnalysis =
            ensureConstructorAnalysis(target);
        const SemanticModel &defaultModel =
            targetAnalysis == nullptr ? *baseModel : targetAnalysis->model;
        for (const Expr *defaultArgument : resolved->defaultArguments) {
          if (const std::optional<HirValueId> value = lowerDefaultArgument(
                  defaultArgument, defaultModel, targetClassArguments,
                  targetClassValueArguments, body)) {
            lowered.arguments.push_back(*value);
            initializerValues.push_back(*value);
          }
        }
      }
      initializers.push_back(std::move(lowered));
    }
    if (!hasExplicitBase) {
      const auto base = std::find_if(owner.bases.begin(), owner.bases.end(),
                                     [](const HirBaseInstance &candidate) {
                                       return !candidate.interface;
                                     });
      if (base != owner.bases.end()) {
        std::vector<HirValueId> implicitBaseValues;
        HirConstructorInitializer lowered{
            .kind = ConstructorInitializerTargetKind::Base,
            .targetType = base->type,
            .base = base->instance == 0
                        ? std::nullopt
                        : std::optional<HirClassInstanceId>(base->instance)};
        const ClassTypeInfo *baseType =
            baseModel->findClassType(base->type.classId);
        const ClassLifecycleInfo *lifecycle =
            baseType == nullptr || baseType->declaration == nullptr
                ? nullptr
                : baseModel->findClassLifecycle(*baseType->declaration);
        if (lifecycle != nullptr) {
          const auto declared = std::find_if(
              lifecycle->constructors.begin(), lifecycle->constructors.end(),
              [](const ConstructorInfo &candidate) {
                return candidate.requiredParameterCount == 0;
              });
          const bool generated = declared == lifecycle->constructors.end();
          lowered.generatedDefault = generated;
          std::vector<SemanticType> parameterTypes;
          std::vector<const Expr *> defaultArguments;
          if (!generated && declared->declaration != nullptr) {
            const GenericSubstitution substitution = classSubstitution(
                *baseType, base->type.arguments, base->type.valueArguments);
            parameterTypes.reserve(declared->parameterTypes.size());
            for (const SemanticType &parameter : declared->parameterTypes) {
              parameterTypes.push_back(substitute(parameter, substitution));
            }
            defaultArguments =
                omittedDefaultArguments(declared->declaration->parameters(), 0);
          }
          const HirConstructorInstanceId target =
              enqueueConstructor(ResolvedConstructionInfo{
                  .constructor = generated ? 0 : declared->id,
                  .declaration = generated ? nullptr : declared->declaration,
                  .constructedType = base->type,
                  .parameterTypes = parameterTypes,
                  .defaultArguments = defaultArguments,
                  .generatedDefault = generated});
          if (target != 0) {
            lowered.constructorTarget = target;
            const SemanticInstanceAnalysis *targetAnalysis =
                ensureConstructorAnalysis(target);
            const SemanticModel &defaultModel =
                targetAnalysis == nullptr ? *baseModel : targetAnalysis->model;
            for (const Expr *defaultArgument : defaultArguments) {
              if (const std::optional<HirValueId> value = lowerDefaultArgument(
                      defaultArgument, defaultModel, base->type.arguments,
                      base->type.valueArguments, body)) {
                lowered.arguments.push_back(*value);
                implicitBaseValues.push_back(*value);
              }
            }
          }
        }
        initializers.insert(initializers.begin(), std::move(lowered));
        initializerValues.insert(initializerValues.begin(),
                                 implicitBaseValues.begin(),
                                 implicitBaseValues.end());
      }
    }
    body.roots =
        lowerStatements(snapshot.source->body()->statements(), analysis->model,
                        classArguments, classValueArguments, body);
    finalizeLifetimes(body, analysis->model, true, initializers);
    lowerLoans(analysis->model, body);
    output.program.constructors[index].initializerValues =
        std::move(initializerValues);
    output.program.constructors[index].parameterBindings =
        std::move(parameterBindings);
    output.program.constructors[index].initializers = std::move(initializers);
    output.program.constructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  void processDestructor(std::size_t index) {
    const HirDestructorInstance snapshot = output.program.destructors[index];
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!owner.typeArguments.empty() || !owner.valueArguments.empty()) {
      analysis = analyzer->analyzeDestructorInstance(
          owner.declaration, owner.typeArguments, owner.valueArguments);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt);
      model = &analysis.model;
    }

    lambdaTargets.clear();
    seedLambdaTargets(owner.typeArguments);
    HirBody body;
    body.roots =
        lowerStatements(snapshot.source->body()->statements(), *model,
                        owner.typeArguments, owner.valueArguments, body);
    finalizeLifetimes(body, *model, true);
    lowerLoans(*model, body);
    output.program.destructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
  }

  [[nodiscard]] HirValueId lowerImplicitReceiver(HirBody &body) {
    const HirValueId id = nextValueId++;
    body.values.push_back(
        {.id = id,
         .kind = HirValueKind::This,
         .info = {.type = currentReceiverType,
                  .category = ValueCategory::Place,
                  .access = currentReceiverAccess,
                  .traits = analyzer->traitsFor(currentReceiverType)}});
    return id;
  }

  [[nodiscard]] HirBindingId lowerBinding(const VariableDecl &declaration,
                                          const SemanticModel &model,
                                          HirBody &body) {
    const BindingInfo *info = model.findBinding(declaration);
    return lowerBinding(
        declaration,
        info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info, body);
  }

  [[nodiscard]] HirBindingId lowerBinding(const VariableDecl &declaration,
                                          const BindingInfo &info,
                                          HirBody &body) {
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back({.id = id, .variable = &declaration, .info = info});
    (void)enqueueClass(info.type, tokenSpan(declaration.name()));
    return id;
  }

  [[nodiscard]] HirBindingId
  lowerStructuredSource(const StructuredBindingDecl &declaration,
                        const BindingInfo &info, HirBody &body) {
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back(
        {.id = id, .structuredSource = &declaration, .info = info});
    (void)enqueueClass(info.type);
    return id;
  }

  [[nodiscard]] HirValueId lowerStructuredIndex(std::uint64_t index,
                                                HirBody &body) {
    const HirValueId id = nextValueId++;
    body.values.push_back({.id = id,
                           .kind = HirValueKind::Literal,
                           .info = makeExpressionInfo(SemanticType::UInt64),
                           .literal = Literal{index}});
    return id;
  }

  [[nodiscard]] HirBindingId lowerBinding(const Parameter &parameter,
                                          const SemanticModel &model,
                                          HirBody &body) {
    const BindingInfo *info = model.findBinding(parameter);
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back(
        {.id = id,
         .parameter = &parameter,
         .info =
             info == nullptr ? makeBindingInfo(SemanticType::Unknown) : *info});
    if (info != nullptr) {
      (void)enqueueClass(info->type, tokenSpan(parameter.name));
    }
    return id;
  }

  [[nodiscard]] HirBindingId lowerPayloadBinding(const Token &name,
                                                 const BindingInfo &info,
                                                 HirBody &body) {
    const HirBindingId id = nextBindingId++;
    body.bindings.push_back({.id = id, .payloadPattern = &name, .info = info});
    (void)enqueueClass(info.type);
    return id;
  }

  void ensurePlaceDomain(HirBody &body) {
    if (body.placeDomain == PlaceDomain{}) {
      body.placeDomain = {.snapshot = placeSnapshotId,
                          .body = nextPlaceBodyId++};
    }
  }

  [[nodiscard]] static PlaceKey qualifyPlace(PlaceKey place,
                                             PlaceDomain domain) {
    place.domain = domain;
    return place;
  }

  [[nodiscard]] static PlaceKey
  qualifyBorrowPlace(const BorrowOriginPlace &place, PlaceDomain domain) {
    return {
        .domain = domain, .root = place.root, .projections = place.projections};
  }

  [[nodiscard]] static OwnershipEvent
  qualifyOwnershipEvent(OwnershipEvent event, PlaceDomain domain) {
    event.place.domain = domain;
    return event;
  }

  void lowerLoans(const SemanticModel &model, HirBody &body) {
    ensurePlaceDomain(body);
    body.loans.clear();
    std::unordered_map<SymbolId, HirBindingId> bindings;
    for (const HirBinding &binding : body.bindings) {
      if (binding.info.symbol != 0) {
        bindings.insert_or_assign(binding.info.symbol, binding.id);
      }
    }
    for (const SemanticLoanInfo &loan : model.loans()) {
      HirLoan lowered{.semanticLoan = loan.id,
                      .parent = loan.parent,
                      .place = qualifyPlace(loan.place, body.placeDomain),
                      .access = loan.access,
                      .entry = loan.entry};
      for (const SymbolId carrier : loan.carriers) {
        const auto found = bindings.find(carrier);
        if (found != bindings.end() &&
            std::find(lowered.carriers.begin(), lowered.carriers.end(),
                      found->second) == lowered.carriers.end()) {
          lowered.carriers.push_back(found->second);
        }
      }
      if (!lowered.carriers.empty()) {
        body.loans.push_back(std::move(lowered));
      }
    }
  }

  [[nodiscard]] HirDropType dropTypeFor(const SemanticType &type) {
    HirDropType result{.type = type,
                       .requiresActiveCleanup =
                           analyzer->requiresActiveCleanupFor(type)};
    if (type.kind != SemanticType::Class) {
      if (type.kind == SemanticType::Lambda) {
        const auto lambda = lambdaTargets.find(type.lambdaId);
        if (lambda != lambdaTargets.end()) {
          result.lambdaInstance = lambda->second;
        }
      }
      return result;
    }
    result.classInstance = enqueueClass(type);
    if (result.classInstance) {
      result.destructor = enqueueDestructor(*result.classInstance);
    }
    return result;
  }

  [[nodiscard]] bool materializesDropValue(const HirValue &value) const {
    if (value.info.category != ValueCategory::Value ||
        value.info.type == SemanticType::Unknown ||
        value.info.type.kind == SemanticType::Reference ||
        value.info.traits.drop != DropKind::Lexical ||
        !analyzer->requiresActiveCleanupFor(value.info.type)) {
      return false;
    }
    switch (value.kind) {
    case HirValueKind::ArrayInitializer:
    case HirValueKind::Binary:
    case HirValueKind::Call:
    case HirValueKind::Conditional:
    case HirValueKind::Move:
    case HirValueKind::NativeCallback:
    case HirValueKind::Conversion:
    case HirValueKind::DirectInitializer:
    case HirValueKind::Grouping:
    case HirValueKind::Lambda:
    case HirValueKind::Literal:
    case HirValueKind::Logical:
    case HirValueKind::PackFold:
    case HirValueKind::PackExpansion:
    case HirValueKind::PayloadConstruction:
    case HirValueKind::Postfix:
    case HirValueKind::Unary:
    case HirValueKind::Unexpected:
      return true;
    case HirValueKind::MemberAccess:
    case HirValueKind::Index:
      return value.functionTarget.has_value();
    case HirValueKind::Assignment:
    case HirValueKind::DereferenceSet:
    case HirValueKind::IndexSet:
    case HirValueKind::LayoutQuery:
    case HirValueKind::PayloadExtraction:
    case HirValueKind::QualifiedName:
    case HirValueKind::This:
    case HirValueKind::MemberSet:
    case HirValueKind::Variable:
      return false;
    }
    return false;
  }

  void assignFullExpression(HirBody &body, HirValueId valueId,
                            HirFullExpressionId fullExpression,
                            std::unordered_set<HirValueId> &visited) {
    if (valueId == 0 || !visited.insert(valueId).second) {
      return;
    }
    const auto found = std::find_if(
        body.values.begin(), body.values.end(),
        [valueId](const HirValue &value) { return value.id == valueId; });
    if (found == body.values.end()) {
      return;
    }
    if (found->fullExpression != 0 && found->fullExpression != fullExpression) {
      lifecycleValid = false;
      return;
    }
    found->fullExpression = fullExpression;
    for (const HirValueId operand : found->operands) {
      assignFullExpression(body, operand, fullExpression, visited);
    }
    if (found->receiver) {
      assignFullExpression(body, *found->receiver, fullExpression, visited);
    }
  }

  void appendFullExpression(HirBody &body, std::vector<HirValueId> roots,
                            HirStatementId statement = 0,
                            std::size_t constructorInitializer = 0) {
    std::erase(roots, HirValueId{0});
    if (roots.empty()) {
      return;
    }
    const HirFullExpressionId id = body.fullExpressions.size() + 1;
    body.fullExpressions.push_back(
        {.id = id,
         .statement = statement,
         .constructorInitializer = constructorInitializer,
         .roots = roots});
    std::unordered_set<HirValueId> visited;
    for (const HirValueId root : roots) {
      assignFullExpression(body, root, id, visited);
    }
  }

  [[nodiscard]] std::vector<HirValueId>
  mapFullExpressionRoots(HirBody &body,
                         const SemanticFullExpression &expression) {
    std::vector<HirValueId> roots;
    roots.reserve(expression.roots.size());
    for (const Expr *source : expression.roots) {
      const auto found = std::find_if(
          body.values.rbegin(), body.values.rend(),
          [source](const HirValue &value) { return value.source == source; });
      if (found == body.values.rend()) {
        lifecycleValid = false;
        continue;
      }
      roots.push_back(found->id);
    }
    return roots;
  }

  void finalizeLifetimes(
      HirBody &body, const SemanticModel &model, bool lexicalBindings,
      const std::vector<HirConstructorInitializer> &initializers = {}) {
    body.fullExpressions.clear();
    body.dropObligations.clear();
    for (HirBinding &binding : body.bindings) {
      binding.dropObligation.reset();
    }
    for (HirValue &value : body.values) {
      value.fullExpression = 0;
      value.dropObligation.reset();
    }

    struct PendingFullExpression {
      const SemanticFullExpression *expression = nullptr;
      HirStatementId statement = 0;
      std::size_t constructorInitializer = 0;
    };
    std::vector<PendingFullExpression> pendingFullExpressions;
    for (std::size_t index = 0; index < initializers.size(); ++index) {
      const HirConstructorInitializer &initializer = initializers[index];
      if (initializer.source == nullptr) {
        if (!initializer.arguments.empty()) {
          appendFullExpression(body, initializer.arguments, 0, index + 1);
        }
        continue;
      }
      for (const SemanticFullExpression &expression :
           model.fullExpressionsFor(*initializer.source)) {
        pendingFullExpressions.push_back(
            {.expression = &expression, .constructorInitializer = index + 1});
      }
    }
    for (const HirStatement &statement : body.statements) {
      if (statement.source == nullptr) {
        continue;
      }
      for (const SemanticFullExpression &expression :
           model.fullExpressionsFor(*statement.source)) {
        pendingFullExpressions.push_back(
            {.expression = &expression, .statement = statement.id});
      }
    }
    std::stable_sort(pendingFullExpressions.begin(),
                     pendingFullExpressions.end(),
                     [](const PendingFullExpression &left,
                        const PendingFullExpression &right) {
                       return left.expression->order < right.expression->order;
                     });
    for (const PendingFullExpression &pending : pendingFullExpressions) {
      const std::vector<HirValueId> roots =
          pending.constructorInitializer != 0 &&
                  pending.constructorInitializer <= initializers.size()
              ? initializers[pending.constructorInitializer - 1].arguments
              : mapFullExpressionRoots(body, *pending.expression);
      appendFullExpression(body, roots, pending.statement,
                           pending.constructorInitializer);
    }

    if (lexicalBindings) {
      for (HirBinding &binding : body.bindings) {
        if (binding.info.type.kind == SemanticType::Reference ||
            binding.info.staticStorage ||
            binding.info.traits.drop != DropKind::Lexical) {
          continue;
        }
        const HirDropObligationId id = body.dropObligations.size() + 1;
        body.dropObligations.push_back(
            {.id = id,
             .constructionOrder = id,
             .kind = HirDropObligationKind::Binding,
             .binding = binding.id,
             .dropType = dropTypeFor(binding.info.type),
             .initiallyActive = binding.parameter != nullptr});
        binding.dropObligation = id;
      }
    }

    std::unordered_set<HirValueId> temporaryReceiverValues;
    for (const HirValue &call : body.values) {
      if (!call.callPlan || !call.callPlan->receiver) {
        continue;
      }
      const HirCallReceiver &receiver = *call.callPlan->receiver;
      if (receiver.kind == HirCallInputKind::ReadTemporaryBorrow ||
          receiver.kind == HirCallInputKind::MutableTemporaryBorrow) {
        temporaryReceiverValues.insert(receiver.value);
      }
    }
    for (HirValue &value : body.values) {
      if (value.fullExpression == 0 ||
          (!materializesDropValue(value) &&
           !temporaryReceiverValues.contains(value.id))) {
        continue;
      }
      const HirDropObligationId id = body.dropObligations.size() + 1;
      body.dropObligations.push_back(
          {.id = id,
           .constructionOrder = id,
           .kind = HirDropObligationKind::Value,
           .value = value.id,
           .fullExpression = value.fullExpression,
           .dropType = dropTypeFor(value.info.type)});
      value.dropObligation = id;
    }
  }

  [[nodiscard]] static std::vector<SemanticLoanId>
  orderedLoanEnds(std::vector<SemanticLoanId> loans,
                  const SemanticModel &model) {
    const auto depth = [&](SemanticLoanId id) {
      std::size_t result = 0;
      std::vector<SemanticLoanId> visited;
      while (id != 0 &&
             std::find(visited.begin(), visited.end(), id) == visited.end()) {
        visited.push_back(id);
        const SemanticLoanInfo *loan = model.findLoan(id);
        id = loan == nullptr ? 0 : loan->parent;
        if (id != 0) {
          ++result;
        }
      }
      return result;
    };
    std::stable_sort(loans.begin(), loans.end(),
                     [&](SemanticLoanId left, SemanticLoanId right) {
                       return depth(left) > depth(right);
                     });
    return loans;
  }

  [[nodiscard]] HirStatementId appendStatement(HirStatement statement,
                                               HirBody &body) {
    statement.id = nextStatementId++;
    const HirStatementId id = statement.id;
    body.statements.push_back(std::move(statement));
    return id;
  }

  [[nodiscard]] std::vector<HirStatementId>
  lowerStatements(const StmtList &statements, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    std::vector<HirStatementId> result;
    for (const StmtPtr &statement : statements) {
      if (const std::optional<HirStatementId> lowered =
              lowerStatement(statement.get(), model, classArguments,
                             classValueArguments, body)) {
        result.push_back(*lowered);
      }
    }
    return result;
  }

  [[nodiscard]] std::optional<HirStatementId>
  lowerStatement(const Stmt *statement, const SemanticModel &model,
                 const std::vector<SemanticType> &classArguments,
                 const std::vector<CompileTimeValue> &classValueArguments,
                 HirBody &body) {
    const std::optional<HirStatementId> lowered = lowerStatementImpl(
        statement, model, classArguments, classValueArguments, body);
    if (lowered && statement != nullptr && !body.statements.empty() &&
        body.statements.back().id == *lowered) {
      body.statements.back().endedLoans =
          orderedLoanEnds(model.loansEndingAfter(*statement), model);
    }
    return lowered;
  }

  [[nodiscard]] std::optional<HirStatementId>
  lowerStatementImpl(const Stmt *statement, const SemanticModel &model,
                     const std::vector<SemanticType> &classArguments,
                     const std::vector<CompileTimeValue> &classValueArguments,
                     HirBody &body) {
    if (statement == nullptr) {
      return std::nullopt;
    }
    if (const auto *block = dynamic_cast<const BlockStmt *>(statement)) {
      return appendStatement({.kind = HirStatementKind::Block,
                              .source = statement,
                              .unsafeBlock = block->isUnsafe(),
                              .statements = lowerStatements(
                                  block->statements(), model, classArguments,
                                  classValueArguments, body)},
                             body);
    }
    if (const auto *conditional =
            dynamic_cast<const ConditionalStmt *>(statement)) {
      std::vector<HirStatementId> statements;
      if (const StmtList *branch = conditional->activeBranch(target)) {
        statements = lowerStatements(*branch, model, classArguments,
                                     classValueArguments, body);
      }
      return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                              .source = statement,
                              .statements = std::move(statements)},
                             body);
    }
    if (dynamic_cast<const CompileErrorDirective *>(statement) != nullptr) {
      return std::nullopt;
    }
    if (dynamic_cast<const StaticAssertDecl *>(statement) != nullptr) {
      return std::nullopt;
    }
    if (const auto *expression =
            dynamic_cast<const ExpressionStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Expression,
           .source = statement,
           .value = lowerExpression(expression->expression(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *forStatement = dynamic_cast<const ForStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::For,
           .source = statement,
           .condition =
               lowerExpression(forStatement->condition(), model, classArguments,
                               classValueArguments, body),
           .increment =
               lowerExpression(forStatement->increment(), model, classArguments,
                               classValueArguments, body),
           .initializer =
               lowerStatement(forStatement->initializer().get(), model,
                              classArguments, classValueArguments, body),
           .body = lowerStatement(forStatement->body().get(), model,
                                  classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *rangeFor = dynamic_cast<const RangeForStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::RangeFor,
           .source = statement,
           .body = lowerStatement(rangeFor->lowered().get(), model,
                                  classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *ifStatement = dynamic_cast<const IfStmt *>(statement)) {
      if (ifStatement->isConstexpr()) {
        std::vector<HirStatementId> statements;
        if (const std::optional<bool> selected =
                model.findConstexprBranch(*ifStatement)) {
          const StmtPtr &branch =
              *selected ? ifStatement->thenBranch() : ifStatement->elseBranch();
          if (const std::optional<HirStatementId> lowered =
                  lowerStatement(branch.get(), model, classArguments,
                                 classValueArguments, body)) {
            statements.push_back(*lowered);
          }
        }
        return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                                .source = statement,
                                .statements = std::move(statements)},
                               body);
      }
      return appendStatement(
          {.kind = HirStatementKind::If,
           .source = statement,
           .condition =
               lowerExpression(ifStatement->condition(), model, classArguments,
                               classValueArguments, body),
           .body = lowerStatement(ifStatement->thenBranch().get(), model,
                                  classArguments, classValueArguments, body),
           .elseBranch =
               lowerStatement(ifStatement->elseBranch().get(), model,
                              classArguments, classValueArguments, body),
           .thenEntryEndedLoans = orderedLoanEnds(
               model.loansEndingAtConditionalEntry(*ifStatement, true), model),
           .elseEntryEndedLoans = orderedLoanEnds(
               model.loansEndingAtConditionalEntry(*ifStatement, false),
               model)},
          body);
    }
    if (const auto *loopControl =
            dynamic_cast<const LoopControlStmt *>(statement)) {
      return appendStatement(
          {.kind = loopControl->keyword().kind == TokenKind::BREAK
                       ? HirStatementKind::Break
                       : HirStatementKind::Continue,
           .source = statement},
          body);
    }
    if (const auto *returnStatement =
            dynamic_cast<const ReturnStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Return,
           .source = statement,
           .value = lowerExpression(returnStatement->value(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *switchStatement =
            dynamic_cast<const SwitchStmt *>(statement)) {
      const std::optional<HirValueId> subject =
          lowerExpression(switchStatement->expression(), model, classArguments,
                          classValueArguments, body);
      std::vector<HirSwitchArm> arms;
      arms.reserve(switchStatement->arms().size());
      for (std::size_t armIndex = 0; armIndex < switchStatement->arms().size();
           ++armIndex) {
        const SwitchArm &arm = switchStatement->arms()[armIndex];
        HirSwitchArm loweredArm;
        loweredArm.labels.reserve(arm.labels.size());
        for (const SwitchLabel &label : arm.labels) {
          const ResolvedPayloadPatternInfo *payloadPattern =
              label.value ? model.findPayloadPattern(*label.value) : nullptr;
          std::optional<HirValueId> value;
          if (payloadPattern == nullptr) {
            value = lowerExpression(label.value, model, classArguments,
                                    classValueArguments, body);
          }
          const SwitchCaseValue *constant =
              label.value ? model.findSwitchCase(*label.value) : nullptr;
          loweredArm.labels.push_back(
              {.source = &label,
               .isDefault = label.isDefault(),
               .value = value,
               .constant = constant == nullptr
                               ? std::nullopt
                               : std::optional<SwitchCaseValue>{*constant}});
          if (payloadPattern != nullptr && subject) {
            for (const PayloadBindingInfo &payload : payloadPattern->bindings) {
              if (payload.name == nullptr || payload.source == nullptr) {
                continue;
              }
              const HirBindingId binding =
                  lowerPayloadBinding(*payload.name, payload.binding, body);
              const HirValueId extraction = nextValueId++;
              body.values.push_back(
                  {.id = extraction,
                   .kind = HirValueKind::PayloadExtraction,
                   .source = payload.source,
                   .info = {.type = payload.binding.type,
                            .category = ValueCategory::Value,
                            .access = AccessMode::ReadOnly,
                            .traits =
                                analyzer->traitsFor(payload.binding.type)},
                   .operands = {*subject},
                   .enumOwner = payloadPattern->owner,
                   .enumVariant = payloadPattern->variantIndex,
                   .payloadIndex = payload.payloadIndex});
              output.program.sourceValueIds[payload.source].push_back(
                  extraction);
              loweredArm.payloadBindings.push_back(
                  {.binding = binding, .value = extraction});
            }
          }
        }
        loweredArm.statements = lowerStatements(
            arm.statements, model, classArguments, classValueArguments, body);
        loweredArm.entryEndedLoans = orderedLoanEnds(
            model.loansEndingAtSwitchArmEntry(*switchStatement, armIndex),
            model);
        arms.push_back(std::move(loweredArm));
      }
      return appendStatement(
          {.kind = HirStatementKind::Switch,
           .source = statement,
           .value = subject,
           .switchArms = std::move(arms),
           .exhaustiveSwitch = model.isExhaustiveSwitch(*switchStatement)},
          body);
    }
    if (const auto *structured =
            dynamic_cast<const StructuredBindingDecl *>(statement)) {
      const StructuredBindingInfo *info =
          model.findStructuredBinding(*structured);
      const BindingInfo sourceInfo =
          info == nullptr ? makeBindingInfo(SemanticType::Unknown)
                          : info->source;
      const HirBindingId source =
          lowerStructuredSource(*structured, sourceInfo, body);
      const std::optional<HirValueId> value =
          lowerExpression(structured->initializer(), model, classArguments,
                          classValueArguments, body);
      std::vector<HirStructuredBindingElement> bindings;
      if (info != nullptr) {
        bindings.reserve(info->elements.size());
        for (const StructuredBindingElementInfo &element : info->elements) {
          if (element.declaration == nullptr) {
            continue;
          }
          HirStructuredBindingElement lowered{
              .binding =
                  lowerBinding(*element.declaration, element.binding, body),
              .projection =
                  element.projection ==
                          StructuredBindingProjectionKind::ArrayElement
                      ? HirStructuredBindingProjectionKind::ArrayElement
                      : HirStructuredBindingProjectionKind::Field,
              .field = element.field};
          if (element.projection ==
              StructuredBindingProjectionKind::ArrayElement) {
            lowered.index = lowerStructuredIndex(element.index, body);
          }
          bindings.emplace_back(std::move(lowered));
        }
      }
      return appendStatement({.kind = HirStatementKind::StructuredBinding,
                              .source = statement,
                              .binding = source,
                              .value = value,
                              .structuredBindings = std::move(bindings)},
                             body);
    }
    if (const auto *variable = dynamic_cast<const VariableDecl *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Variable,
           .source = statement,
           .binding = lowerBinding(*variable, model, body),
           .value = lowerExpression(variable->initializer(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *doWhile = dynamic_cast<const DoWhileStmt *>(statement)) {
      const std::optional<HirStatementId> loweredBody =
          lowerStatement(doWhile->body().get(), model, classArguments,
                         classValueArguments, body);
      const std::optional<HirValueId> loweredCondition =
          lowerExpression(doWhile->condition(), model, classArguments,
                          classValueArguments, body);
      return appendStatement({.kind = HirStatementKind::DoWhile,
                              .source = statement,
                              .condition = loweredCondition,
                              .body = loweredBody},
                             body);
    }
    if (const auto *whileStatement =
            dynamic_cast<const WhileStmt *>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::While,
           .source = statement,
           .condition =
               lowerExpression(whileStatement->condition(), model,
                               classArguments, classValueArguments, body),
           .body = lowerStatement(whileStatement->body().get(), model,
                                  classArguments, classValueArguments, body)},
          body);
    }
    if (dynamic_cast<const EmptyStmt *>(statement) != nullptr) {
      return appendStatement(
          {.kind = HirStatementKind::Empty, .source = statement}, body);
    }
    return std::nullopt;
  }

  [[nodiscard]] std::vector<SemanticType> receiverClassArguments(
      const ExprPtr &callee, const FunctionInfo &target,
      const SemanticType &dispatchOwner, const SemanticModel &model,
      const std::vector<SemanticType> &currentClassArguments) const {
    if (target.ownerClass == 0) {
      return {};
    }
    if (dispatchOwner.kind == SemanticType::Class &&
        dispatchOwner.classId == target.ownerClass) {
      return dispatchOwner.arguments;
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      SemanticType receiver = model.typeOf(*member->object());
      if (receiver.kind == SemanticType::RawPointer &&
          receiver.arguments.size() == 1) {
        // Copy before assignment: the pointee lives inside the vector the
        // assignment replaces, so a direct self-assign reads freed storage.
        SemanticType pointee = receiver.arguments.front();
        receiver = std::move(pointee);
      }
      if (receiver.kind == SemanticType::Class &&
          receiver.classId == target.ownerClass) {
        return receiver.arguments;
      }
      if (const ResolvedOperatorInfo *arrow = model.findOperator(*member);
          arrow != nullptr &&
          arrow->returnType.kind == SemanticType::Reference &&
          !arrow->returnType.arguments.empty()) {
        receiver = arrow->returnType.arguments.front();
        if (receiver.kind == SemanticType::Class &&
            receiver.classId == target.ownerClass) {
          return receiver.arguments;
        }
      }
    }
    return currentClassArguments;
  }

  [[nodiscard]] std::vector<CompileTimeValue> receiverClassValueArguments(
      const ExprPtr &callee, const FunctionInfo &target,
      const SemanticType &dispatchOwner, const SemanticModel &model,
      const std::vector<CompileTimeValue> &currentClassArguments) const {
    if (target.ownerClass == 0) {
      return {};
    }
    if (dispatchOwner.kind == SemanticType::Class &&
        dispatchOwner.classId == target.ownerClass) {
      return dispatchOwner.valueArguments;
    }
    if (const auto *member = dynamic_cast<const Get *>(callee.get())) {
      SemanticType receiver = model.typeOf(*member->object());
      if (receiver.kind == SemanticType::RawPointer &&
          receiver.arguments.size() == 1) {
        SemanticType pointee = receiver.arguments.front();
        receiver = std::move(pointee);
      }
      if (receiver.kind == SemanticType::Class &&
          receiver.classId == target.ownerClass) {
        return receiver.valueArguments;
      }
      if (const ResolvedOperatorInfo *arrow = model.findOperator(*member);
          arrow != nullptr &&
          arrow->returnType.kind == SemanticType::Reference &&
          !arrow->returnType.arguments.empty()) {
        receiver = arrow->returnType.arguments.front();
        if (receiver.kind == SemanticType::Class &&
            receiver.classId == target.ownerClass) {
          return receiver.valueArguments;
        }
      }
    }
    return currentClassArguments;
  }

  [[nodiscard]] HirLambdaId
  lowerLambda(const Lambda &lambda, const SemanticModel &model,
              const std::vector<SemanticType> &classArguments,
              const std::vector<CompileTimeValue> &classValueArguments) {
    const LambdaInfo *info = model.findLambda(lambda);
    if (info == nullptr) {
      return 0;
    }
    if (const auto found = lambdaTargets.find(info->id);
        found != lambdaTargets.end()) {
      return found->second;
    }

    const HirLambdaId id = output.program.lambdas.size() + 1;
    lambdaTargets.emplace(info->id, id);
    output.program.lambdas.push_back({.id = id});

    (void)enqueueClass(info->returnType);
    for (const SemanticType &parameterType : info->parameterTypes) {
      (void)enqueueClass(parameterType);
    }
    for (const LambdaCaptureInfo &capture : info->captures) {
      (void)enqueueClass(capture.type);
    }

    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(lambda.parameters().size());
    for (const Parameter &parameter : lambda.parameters()) {
      parameterBindings.push_back(lowerBinding(parameter, model, body));
    }
    body.roots = lowerStatements(lambda.body(), model, classArguments,
                                 classValueArguments, body);
    finalizeLifetimes(body, model, true);
    lowerLoans(model, body);
    std::vector<bool> captureRequiresActiveCleanup;
    captureRequiresActiveCleanup.reserve(info->captures.size());
    for (const LambdaCaptureInfo &capture : info->captures) {
      captureRequiresActiveCleanup.push_back(
          analyzer->requiresActiveCleanupFor(capture.type));
    }
    output.program.lambdas[id - 1] = {
        .id = id,
        .declaration = info->id,
        .source = &lambda,
        .type = model.typeOf(lambda),
        .returnType = info->returnType,
        .parameterTypes = info->parameterTypes,
        .parameterBindings = std::move(parameterBindings),
        .captures = info->captures,
        .captureRequiresActiveCleanup = std::move(captureRequiresActiveCleanup),
        .traits = info->traits,
        .body = std::move(body),
    };
    return id;
  }

  [[nodiscard]] static bool
  orderedValueParameterType(const SemanticType &type) {
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64:
    case SemanticType::Float:
    case SemanticType::Double:
    case SemanticType::Bool:
    case SemanticType::Char:
    case SemanticType::StringView:
    case SemanticType::CString:
    case SemanticType::NullPtr:
    case SemanticType::RawPointer:
    case SemanticType::Enum:
      return true;
    default:
      return false;
    }
  }

  [[nodiscard]] std::optional<std::vector<HirCallArgument>>
  orderedArguments(const std::vector<HirValueId> &arguments,
                   const std::vector<SemanticType> &parameterTypes,
                   const HirBody &body,
                   std::size_t explicitArgumentCount =
                       std::numeric_limits<std::size_t>::max()) const {
    if (arguments.size() != parameterTypes.size()) {
      return std::nullopt;
    }

    std::vector<HirCallArgument> result;
    result.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const HirValue *input = body.findValue(arguments[index]);
      const SemanticType &parameter = parameterTypes[index];
      if (input == nullptr || input->kind == HirValueKind::PackExpansion) {
        return std::nullopt;
      }

      HirCallInputKind kind = HirCallInputKind::Value;
      if (parameter.kind == SemanticType::Reference) {
        kind = parameter.referenceAccess == AccessMode::Mutable
                   ? HirCallInputKind::MutableBorrow
                   : HirCallInputKind::ReadBorrow;
      } else if (orderedValueParameterType(parameter)) {
        kind = HirCallInputKind::Value;
      } else if (parameter.kind != SemanticType::Class ||
                 input->info.type != parameter ||
                 input->info.traits.containsBorrowedState) {
        return std::nullopt;
      } else if (input->info.category == ValueCategory::Place &&
                 input->info.traits.copyable) {
        kind = HirCallInputKind::CopyValue;
      } else if (input->info.category == ValueCategory::Value &&
                 input->info.traits.movable) {
        kind = HirCallInputKind::MoveValue;
      } else {
        return std::nullopt;
      }

      result.push_back({.parameterIndex = index,
                        .value = arguments[index],
                        .parameterType = parameter,
                        .kind = kind,
                        .defaultArgument = index >= explicitArgumentCount});
    }
    return result;
  }

  [[nodiscard]] std::optional<HirCallReceiver>
  orderedReceiver(HirValueId receiver, ReceiverMutability mutability,
                  CallReceiverMode receiverMode, const HirBody &body) const {
    HirValueId source = receiver;
    const HirValue *input = body.findValue(source);
    if (input == nullptr) {
      return std::nullopt;
    }

    HirCallInputKind kind = HirCallInputKind::Value;
    const bool temporaryReceiver =
        receiverMode == CallReceiverMode::FreshTemporaryRead ||
        receiverMode == CallReceiverMode::FreshTemporaryMutable;
    if (temporaryReceiver) {
      while (input != nullptr && input->kind == HirValueKind::Grouping &&
             input->operands.size() == 1) {
        source = input->operands.front();
        input = body.findValue(source);
      }
      if (input == nullptr || input->info.type.kind != SemanticType::Class ||
          input->info.category != ValueCategory::Value ||
          input->info.traits.containsBorrowedState) {
        return std::nullopt;
      }
      kind = receiverMode == CallReceiverMode::FreshTemporaryMutable
                 ? HirCallInputKind::MutableTemporaryBorrow
                 : HirCallInputKind::ReadTemporaryBorrow;
    }
    const bool consumesReceiver = receiverMode == CallReceiverMode::Consuming ||
                                  mutability == ReceiverMutability::Consuming;
    if (consumesReceiver) {
      if (input->info.type.kind != SemanticType::Class ||
          input->info.category != ValueCategory::Value ||
          !input->info.traits.movable ||
          input->info.traits.containsBorrowedState) {
        return std::nullopt;
      }
      kind = HirCallInputKind::MoveValue;
    } else if (!temporaryReceiver &&
               input->info.category == ValueCategory::Place) {
      kind = receiverMode == CallReceiverMode::MutablePlace
                 ? HirCallInputKind::MutableBorrow
                 : HirCallInputKind::ReadBorrow;
    } else if (!temporaryReceiver && receiverMode != CallReceiverMode::Value) {
      return std::nullopt;
    }

    return HirCallReceiver{
        .value = source,
        .kind = kind,
        .type = input->info.type,
    };
  }

  [[nodiscard]] ExpressionInfo
  operatorResultInfo(const ResolvedOperatorInfo &resolved) const {
    SemanticType type = resolved.returnType;
    ValueCategory category = ValueCategory::Value;
    AccessMode access = AccessMode::ReadOnly;
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
      access = type.referenceAccess;
      type = type.arguments.front();
      category = ValueCategory::Place;
    }
    return {.type = type,
            .category = category,
            .access = access,
            .traits = analyzer->traitsFor(type)};
  }

  [[nodiscard]] HirValueId splitOverloadedArrowProjection(
      HirValue value, const ResolvedOperatorInfo &resolved, HirBody &body) {
    const HirValueId receiver = value.operands.front();
    HirValue arrowCall{
        .id = value.id,
        .kind = HirValueKind::Call,
        .source = value.source,
        .info = operatorResultInfo(resolved),
        .operands = {receiver},
        .parameterTypes = value.parameterTypes,
        .definedFailure = std::move(value.definedFailure),
        .borrowOrigin = value.borrowOrigin,
        .borrowArgument = value.borrowArgument,
        .borrowAccess = value.borrowAccess,
        .borrowPlace = value.borrowPlace,
        .dispatch = value.dispatch,
        .dispatchOwner = value.dispatchOwner,
        .receiver = receiver,
        .functionTarget = value.functionTarget,
        .callableBoundary = value.callableBoundary,
    };
    if (const std::optional<HirCallReceiver> planned =
            orderedReceiver(receiver, resolved.receiverMutability,
                            resolved.receiverMode, body)) {
      arrowCall.callPlan = HirCallPlan{.receiver = *planned, .arguments = {}};
    } else {
      lifecycleValid = false;
    }
    (void)enqueueClass(arrowCall.info.type);

    const HirValueId arrowId = arrowCall.id;
    value.id = nextValueId++;
    value.operands.front() = arrowId;
    value.operation = TokenKind::DOT;
    value.parameterTypes.clear();
    value.intrinsic = IntrinsicKind::None;
    value.synchronization = {};
    value.definedFailure = {};
    value.borrowOrigin = BorrowOriginKind::None;
    value.borrowArgument = 0;
    value.borrowAccess = AccessMode::ReadOnly;
    value.borrowPlace.reset();
    value.dispatch = CallDispatch::Static;
    value.dispatchOwner = SemanticType::Unknown;
    value.receiver.reset();
    value.callPlan.reset();
    value.functionTarget.reset();
    value.callableArguments.clear();
    value.callableBoundary.reset();
    value.callableInvocation.reset();

    const HirValueId projectionId = value.id;
    body.values.push_back(std::move(arrowCall));
    body.values.push_back(std::move(value));
    output.program.sourceValueIds[body.values[body.values.size() - 2].source]
        .push_back(arrowId);
    output.program.sourceValueIds[body.values.back().source].push_back(
        projectionId);
    return projectionId;
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerDefaultArgument(const Expr *expression, const SemanticModel &model,
                       const std::vector<SemanticType> &classArguments,
                       const std::vector<CompileTimeValue> &classValueArguments,
                       HirBody &body) {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (!activeDefaultArguments.insert(expression).second) {
      lifecycleValid = false;
      return std::nullopt;
    }
    const std::optional<HirValueId> result = lowerExpression(
        *expression, model, classArguments, classValueArguments, body);
    activeDefaultArguments.erase(expression);
    return result;
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerExpression(const ExprPtr &expression, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    if (!expression) {
      return std::nullopt;
    }
    return lowerExpression(*expression, model, classArguments,
                           classValueArguments, body);
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerExpression(const Expr &expression, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    ensurePlaceDomain(body);
    const Expr *raw = &expression;
    HirValueKind kind = HirValueKind::Literal;
    std::vector<HirValueId> operands;
    std::optional<TokenKind> operation;
    std::optional<Literal> literal;
    std::optional<HirValueId> receiver;
    std::optional<HirLambdaId> lambdaTarget;
    std::optional<HirNativeCallbackAdapterId> nativeCallbackAdapter;
    std::optional<HirFunctionInstanceId> contextualBoolTarget;
    std::optional<EnumId> enumOwner;
    std::optional<EnumConstant> enumValue;
    std::optional<std::size_t> enumVariant;
    std::optional<std::size_t> payloadIndex;
    const auto lowerOperand = [&](const ExprPtr &operand) {
      if (const std::optional<HirValueId> id = lowerExpression(
              operand, model, classArguments, classValueArguments, body)) {
        operands.push_back(*id);
      }
    };

    if (const auto *assign = dynamic_cast<const Assign *>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*assign);
      if (resolved != nullptr &&
          resolved->kind == OverloadedOperator::Assignment) {
        kind = HirValueKind::Call;
        const SymbolId symbol = model.findResolvedSymbol(*assign);
        const SymbolRecord *record = model.database().findSymbol(symbol);
        HirValue target;
        target.id = nextValueId++;
        target.kind = assign->path().segments.size() == 1
                          ? HirValueKind::Variable
                          : HirValueKind::QualifiedName;
        target.symbol = symbol;
        target.info = record == nullptr
                          ? ExpressionInfo{.type = resolved->dispatchOwner,
                                           .category = ValueCategory::Place,
                                           .access = AccessMode::Mutable,
                                           .traits = semanticTraits(
                                               resolved->dispatchOwner)}
                          : ExpressionInfo{.type = record->type,
                                           .category = ValueCategory::Place,
                                           .access = record->mutableBinding
                                                         ? AccessMode::Mutable
                                                         : AccessMode::ReadOnly,
                                           .traits = record->traits};
        if (const PlaceKey *place = model.findPlace(*assign)) {
          target.place = qualifyPlace(*place, body.placeDomain);
        }
        receiver = target.id;
        operands.push_back(target.id);
        body.values.push_back(std::move(target));
      } else {
        kind = HirValueKind::Assignment;
        operation = assign->oper().kind;
      }
      lowerOperand(assign->value());
    } else if (const auto *initializer =
                   dynamic_cast<const ArrayInitializer *>(raw)) {
      kind = HirValueKind::ArrayInitializer;
      for (const ExprPtr &element : initializer->elements()) {
        lowerOperand(element);
      }
    } else if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
      kind = HirValueKind::Binary;
      operation = binary->oper().kind;
      lowerOperand(binary->left());
      lowerOperand(binary->right());
    } else if (const auto *call = dynamic_cast<const Call *>(raw)) {
      const ResolvedCallInfo *resolved = model.findCall(*call);
      const ResolvedPayloadConstructionInfo *payload =
          model.findPayloadConstruction(*call);
      const ResolvedConstructionInfo *construction =
          model.findConstruction(*call);
      const ClassTypeInfo *constructedClass =
          construction == nullptr ||
                  construction->constructedType.kind != SemanticType::Class
              ? nullptr
              : baseModel->findClassType(construction->constructedType.classId);
      kind = payload != nullptr ? HirValueKind::PayloadConstruction
             : construction != nullptr && constructedClass != nullptr &&
                     constructedClass->kind == ClassKind::Union &&
                     construction->constructor == 0
                 ? HirValueKind::DirectInitializer
             : resolved != nullptr && resolved->intrinsic == IntrinsicKind::Move
                 ? HirValueKind::Move
                 : HirValueKind::Call;
      if (payload != nullptr) {
        enumOwner = payload->owner;
        enumVariant = payload->variantIndex;
      } else if (kind == HirValueKind::Call) {
        // A resolved type construction has no runtime callee value. Retaining
        // its type-name expression would create an untyped, semantically
        // unrecorded HIR operand beside the exact constructor target.
        if (model.findExpression(*call->callee()) != nullptr) {
          lowerOperand(call->callee());
        }
        // An implicit receiver exists only for an unqualified call that
        // resolves to a non-static member of the enclosing class. A free
        // function is not static, so testing staticness alone attached a
        // spurious receiver to every unqualified free-function call made
        // inside a member body.
        const FunctionInfo *resolvedInfo =
            resolved != nullptr && resolved->declaration != nullptr
                ? model.findFunction(*resolved->declaration)
                : nullptr;
        if (resolvedInfo != nullptr && resolvedInfo->ownerClass != 0 &&
            !resolved->declaration->isStatic() &&
            dynamic_cast<const Variable *>(call->callee().get()) != nullptr &&
            currentReceiverType.kind == SemanticType::Class) {
          receiver = lowerImplicitReceiver(body);
        }
      }
      for (const ExprPtr &argument : call->arguments()) {
        lowerOperand(argument);
      }
    } else if (const auto *conditional =
                   dynamic_cast<const ConditionalExpr *>(raw)) {
      kind = HirValueKind::Conditional;
      lowerOperand(conditional->condition());
      lowerOperand(conditional->thenExpression());
      lowerOperand(conditional->elseExpression());
    } else if (const auto *conversion = dynamic_cast<const Conversion *>(raw)) {
      kind = HirValueKind::Conversion;
      lowerOperand(conversion->value());
    } else if (const auto *initializer =
                   dynamic_cast<const DirectInitializer *>(raw)) {
      kind = HirValueKind::DirectInitializer;
      for (const ExprPtr &argument : initializer->arguments()) {
        lowerOperand(argument);
      }
    } else if (const auto *set = dynamic_cast<const DereferenceSet *>(raw)) {
      kind = HirValueKind::DereferenceSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->value());
    } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
      kind = HirValueKind::MemberAccess;
      operation = get->access().kind;
      lowerOperand(get->object());
    } else if (const auto *grouping = dynamic_cast<const Grouping *>(raw)) {
      kind = HirValueKind::Grouping;
      lowerOperand(grouping->expression());
    } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
      kind = HirValueKind::Index;
      lowerOperand(index->object());
      lowerOperand(index->index());
    } else if (const auto *set = dynamic_cast<const IndexSet *>(raw)) {
      kind = HirValueKind::IndexSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->index());
      lowerOperand(set->value());
    } else if (const auto *lambda = dynamic_cast<const Lambda *>(raw)) {
      kind = HirValueKind::Lambda;
      const HirLambdaId target =
          lowerLambda(*lambda, model, classArguments, classValueArguments);
      if (target != 0) {
        lambdaTarget = target;
      }
      for (const LambdaCapture &capture : lambda->captures()) {
        lowerOperand(capture.initializer);
      }
    } else if (dynamic_cast<const LayoutQuery *>(raw) != nullptr) {
      kind = HirValueKind::LayoutQuery;
      if (const std::optional<ConstantValue> constant =
              model.findConstant(*raw)) {
        if (const auto *integer = std::get_if<ConstantInteger>(&*constant);
            integer != nullptr && !integer->negative) {
          literal = Literal{integer->magnitude};
        }
      }
    } else if (const auto *literalExpression =
                   dynamic_cast<const LiteralExpr *>(raw)) {
      const ResolvedConstructionInfo *construction =
          model.findConstruction(*literalExpression);
      if (construction != nullptr &&
          construction->constructedType.kind == SemanticType::Class &&
          construction->parameterTypes.size() == 1 &&
          construction->parameterTypes.front() == SemanticType::NullPtr &&
          std::holds_alternative<std::nullptr_t>(literalExpression->value())) {
        HirValue argument;
        argument.id = nextValueId++;
        argument.kind = HirValueKind::Literal;
        argument.info =
            ExpressionInfo{.type = SemanticType::NullPtr,
                           .category = ValueCategory::Value,
                           .access = AccessMode::ReadOnly,
                           .traits = semanticTraits(SemanticType::NullPtr)};
        argument.literal = literalExpression->value();
        operands.push_back(argument.id);
        body.values.push_back(std::move(argument));
        kind = HirValueKind::DirectInitializer;
      } else {
        kind = HirValueKind::Literal;
        literal = literalExpression->value();
      }
    } else if (const auto *logical = dynamic_cast<const Logical *>(raw)) {
      kind = HirValueKind::Logical;
      operation = logical->oper().kind;
      lowerOperand(logical->left());
      lowerOperand(logical->right());
    } else if (const auto *fold = dynamic_cast<const PackFold *>(raw)) {
      kind = HirValueKind::PackFold;
      if (const auto *pattern =
              dynamic_cast<const Call *>(fold->pattern().get())) {
        for (const ExprPtr &argument : pattern->arguments()) {
          lowerOperand(argument);
        }
      }
    } else if (dynamic_cast<const PackExpansion *>(raw) != nullptr) {
      kind = HirValueKind::PackExpansion;
    } else if (const auto *postfix = dynamic_cast<const Postfix *>(raw)) {
      kind = HirValueKind::Postfix;
      operation = postfix->oper().kind;
      lowerOperand(postfix->expression());
    } else if (const auto *qualified =
                   dynamic_cast<const QualifiedName *>(raw)) {
      kind = HirValueKind::QualifiedName;
      if (const ResolvedEnumeratorInfo *resolved =
              model.findEnumerator(*qualified)) {
        enumOwner = resolved->owner;
        enumValue = resolved->value;
        enumVariant = resolved->variantIndex;
        const EnumTypeInfo *enumeration =
            baseModel->findEnumType(resolved->owner);
        if (enumeration != nullptr && enumeration->payload) {
          kind = HirValueKind::PayloadConstruction;
        }
      }
    } else if (dynamic_cast<const This *>(raw) != nullptr) {
      kind = HirValueKind::This;
    } else if (const auto *set = dynamic_cast<const Set *>(raw)) {
      kind = HirValueKind::MemberSet;
      operation = set->oper().kind;
      lowerOperand(set->object());
      lowerOperand(set->value());
    } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
      kind = HirValueKind::Unary;
      operation = unary->oper().kind;
      lowerOperand(unary->right());
    } else if (const auto *unexpected = dynamic_cast<const Unexpected *>(raw)) {
      kind = HirValueKind::Unexpected;
      lowerOperand(unexpected->error());
    } else if (dynamic_cast<const Variable *>(raw) != nullptr) {
      const ExpressionInfo *info = model.findExpression(*raw);
      if (const std::optional<ConstantValue> constant =
              model.findConstant(*raw);
          info != nullptr && info->category == ValueCategory::Value &&
          constant) {
        if (const auto *integer = std::get_if<ConstantInteger>(&*constant);
            integer != nullptr && !integer->negative) {
          kind = HirValueKind::Literal;
          literal = Literal{integer->magnitude};
        } else {
          kind = HirValueKind::Variable;
        }
      } else {
        kind = HirValueKind::Variable;
      }
    }

    if (const NativeFunctionConversionInfo *conversion =
            model.findNativeFunctionConversion(*raw)) {
      kind = HirValueKind::NativeCallback;
      const HirNativeCallbackAdapterId adapter =
          enqueueNativeCallback(*conversion);
      if (adapter != 0) {
        nativeCallbackAdapter = adapter;
      }
    }

    HirValue value{.id = nextValueId++,
                   .kind = kind,
                   .source = raw,
                   .unsafeOperation = model.unsafeOperation(*raw),
                   .symbol = model.findResolvedSymbol(*raw),
                   .operands = std::move(operands),
                   .operation = operation,
                   .literal = std::move(literal),
                   .constant = model.findConstant(*raw),
                   .programConstantSubstitution =
                       model.isProgramConstantSubstitution(*raw),
                   .receiver = receiver,
                   .nativeCallbackAdapter = nativeCallbackAdapter,
                   .lambdaTarget = lambdaTarget,
                   .enumOwner = enumOwner,
                   .enumValue = enumValue,
                   .enumVariant = enumVariant,
                   .payloadIndex = payloadIndex};
    if (const auto *call = dynamic_cast<const Call *>(raw);
        call != nullptr &&
        dynamic_cast<const Get *>(call->callee().get()) != nullptr &&
        !value.operands.empty()) {
      const HirValue *callee = body.findValue(value.operands.front());
      if (callee != nullptr && callee->kind == HirValueKind::MemberAccess &&
          !callee->operands.empty()) {
        value.receiver =
            callee->unsafeOperation == UnsafeOperationKind::RawMember
                ? std::optional<HirValueId>{callee->id}
                : std::optional<HirValueId>{callee->operands.front()};
      }
    }
    if (const DefinedFailureOperation *failure =
            model.findDefinedFailure(*raw)) {
      value.definedFailure = *failure;
    }
    if (const ExpressionInfo *info = model.findExpression(*raw)) {
      value.info = *info;
      (void)enqueueClass(info->type);
    }
    if (kind == HirValueKind::PackExpansion) {
      if (value.info.type.kind != SemanticType::TypePack ||
          !value.info.type.concretePack || value.symbol == 0) {
        lifecycleValid = false;
      } else {
        value.packExpansionElements.reserve(value.info.type.arguments.size());
        for (const SemanticType &element : value.info.type.arguments) {
          const SemanticTypeTraits traits = analyzer->traitsFor(element);
          HirCallInputKind inputKind = HirCallInputKind::Value;
          if (orderedValueParameterType(element)) {
            inputKind = HirCallInputKind::Value;
          } else if (element.kind == SemanticType::Class &&
                     !traits.containsBorrowedState && traits.copyable) {
            inputKind = HirCallInputKind::CopyValue;
          } else if (element.kind == SemanticType::Class &&
                     !traits.containsBorrowedState && traits.movable) {
            inputKind = HirCallInputKind::MoveValue;
          } else {
            lifecycleValid = false;
          }
          value.packExpansionElements.push_back(
              {.type = element, .traits = traits, .kind = inputKind});
        }
      }
    }
    if (const PlaceKey *place = model.findPlace(*raw)) {
      value.place = qualifyPlace(*place, body.placeDomain);
    }
    if (const OwnershipEvent *event = model.findOwnershipEvent(*raw)) {
      value.ownership = qualifyOwnershipEvent(*event, body.placeDomain);
      if (!value.place) {
        value.place = value.ownership->place;
      }
    }
    if (value.kind == HirValueKind::Variable && value.place &&
        value.place->receiver && value.symbol != 0) {
      // Preserve the resolved meaning of an unqualified instance field in
      // HIR. It is the same receiver projection as `this.field`, even though
      // the source omitted the explicit receiver.
      kind = HirValueKind::MemberAccess;
      value.kind = kind;
      value.operands.insert(value.operands.begin(),
                            lowerImplicitReceiver(body));
    }
    if (const auto *call = dynamic_cast<const Call *>(raw)) {
      if (const ResolvedCallInfo *resolved = model.findCall(*call)) {
        value.intrinsic = resolved->intrinsic;
        value.dispatch = resolved->dispatch;
        value.dispatchOwner = resolved->dispatchOwner;
        value.parameterTypes = resolved->parameterTypes;
        value.borrowOrigin = resolved->borrowOrigin;
        value.borrowArgument = resolved->borrowArgument;
        value.borrowAccess = resolved->borrowAccess;
        value.borrowPlace = resolved->borrowPlace;
        if (value.borrowOrigin == BorrowOriginKind::Global &&
            value.borrowPlace) {
          ensurePlaceDomain(body);
          value.place =
              qualifyBorrowPlace(*value.borrowPlace, body.placeDomain);
        }
        value.callableArguments = resolved->callableArguments;
        if (resolved->intrinsic == IntrinsicKind::None &&
            resolved->function != 0 && resolved->declaration != nullptr) {
          if (const FunctionInfo *target =
                  baseModel->findFunction(resolved->function)) {
            value.functionTarget = enqueueFunction(
                *target,
                receiverClassArguments(call->callee(), *target,
                                       resolved->dispatchOwner, model,
                                       classArguments),
                receiverClassValueArguments(call->callee(), *target,
                                            resolved->dispatchOwner, model,
                                            classValueArguments),
                resolved->typeArguments, resolved->valueArguments,
                resolved->returnType, resolved->parameterTypes,
                tokenSpan(call->paren()));
          }
        }
        if (!resolved->defaultArguments.empty() && value.functionTarget &&
            *value.functionTarget <= output.program.functions.size()) {
          const HirFunctionInstance &target =
              output.program.functions[*value.functionTarget - 1];
          std::vector<SemanticType> targetClassArguments;
          std::vector<CompileTimeValue> targetClassValueArguments;
          if (target.owner && *target.owner <= output.program.classes.size()) {
            const HirClassInstance &owner =
                output.program.classes[*target.owner - 1];
            targetClassArguments = owner.typeArguments;
            targetClassValueArguments = owner.valueArguments;
          }
          const SemanticInstanceAnalysis *targetAnalysis =
              ensureFunctionAnalysis(*value.functionTarget);
          const SemanticModel &defaultModel =
              targetAnalysis == nullptr ? *baseModel : targetAnalysis->model;
          for (const Expr *defaultArgument : resolved->defaultArguments) {
            if (const std::optional<HirValueId> lowered = lowerDefaultArgument(
                    defaultArgument, defaultModel, targetClassArguments,
                    targetClassValueArguments, body)) {
              value.operands.push_back(*lowered);
            }
          }
        }
      }
      if (const ResolvedLambdaCallInfo *resolved =
              model.findLambdaCall(*call)) {
        value.parameterTypes = resolved->parameterTypes;
        value.callableBoundary = resolved->boundary;
        value.callableInvocation = resolved->capability;
        if (const auto target = lambdaTargets.find(resolved->lambda);
            target != lambdaTargets.end()) {
          value.lambdaTarget = target->second;
        } else {
          const auto existing = std::find_if(
              output.program.lambdas.begin(), output.program.lambdas.end(),
              [&](const HirLambda &candidate) {
                const SemanticType calleeType = model.typeOf(*call->callee());
                return candidate.returnType == resolved->returnType &&
                       candidate.parameterTypes == resolved->parameterTypes &&
                       lambdaMatchesType(candidate, calleeType);
              });
          if (existing != output.program.lambdas.end()) {
            value.lambdaTarget = existing->id;
          }
        }
      }
      if (model.findLambdaCall(*call) == nullptr &&
          model.findOperator(*call) == nullptr) {
        if (const DeferredCallableCallInfo *deferred =
                model.findDeferredCallableCall(*call)) {
          value.parameterTypes = deferred->parameterTypes;
          value.callableBoundary = deferred->boundary;
          value.callableInvocation = deferred->capability;
        }
      }
    }
    if (const auto *fold = dynamic_cast<const PackFold *>(raw)) {
      if (const ResolvedPackFoldInfo *resolved = model.findPackFold(*fold)) {
        value.packFoldSymbol = resolved->packSymbol;
        value.packFoldParameter = resolved->packParameter;
        value.packFoldFunction = resolved->function;
        value.packFoldArgument = resolved->packArgument;
        value.packFoldElements.reserve(resolved->elements.size());
        for (const ResolvedPackFoldElement &element : resolved->elements) {
          const FunctionInfo *target =
              baseModel->findFunction(element.call.function);
          const HirFunctionInstanceId functionTarget =
              target == nullptr
                  ? 0
                  : enqueueFunction(*target, {}, {}, element.call.typeArguments,
                                    element.call.valueArguments,
                                    element.call.returnType,
                                    element.call.parameterTypes,
                                    tokenSpan(fold->ellipsis()));
          if (functionTarget == 0) {
            lifecycleValid = false;
          }
          value.packFoldElements.push_back(
              {.elementType = element.elementType,
               .functionTarget = functionTarget,
               .parameterTypes = element.call.parameterTypes});
        }
      }
    }
    if (const ResolvedConstructionInfo *construction =
            model.findConstruction(*raw)) {
      const bool inPlaceStorageConstruction =
          value.intrinsic == IntrinsicKind::StorageConstruct ||
          value.intrinsic == IntrinsicKind::PrefixStorageAppend ||
          value.intrinsic == IntrinsicKind::PrefixStorageInsert;
      if (!inPlaceStorageConstruction) {
        value.parameterTypes = construction->parameterTypes;
      }
      value.borrowOrigin = construction->borrowOrigin;
      value.borrowArgument = construction->borrowArgument;
      value.borrowAccess = construction->borrowAccess;
      value.constructorKind = construction->kind;
      std::optional<SourceSpan> site;
      if (const auto *call = dynamic_cast<const Call *>(raw)) {
        site = tokenSpan(call->paren());
      } else if (const auto *initializer =
                     dynamic_cast<const DirectInitializer *>(raw)) {
        site = tokenSpan(initializer->brace());
      }
      const HirConstructorInstanceId target =
          enqueueConstructor(*construction, std::move(site));
      if (target != 0) {
        value.constructorTarget = target;
        if (!construction->defaultArguments.empty() &&
            target <= output.program.constructors.size()) {
          const HirConstructorInstance &targetInstance =
              output.program.constructors[target - 1];
          std::vector<SemanticType> targetClassArguments;
          std::vector<CompileTimeValue> targetClassValueArguments;
          if (targetInstance.owner != 0 &&
              targetInstance.owner <= output.program.classes.size()) {
            const HirClassInstance &owner =
                output.program.classes[targetInstance.owner - 1];
            targetClassArguments = owner.typeArguments;
            targetClassValueArguments = owner.valueArguments;
          }
          const SemanticInstanceAnalysis *targetAnalysis =
              ensureConstructorAnalysis(target);
          const SemanticModel &defaultModel =
              targetAnalysis == nullptr ? *baseModel : targetAnalysis->model;
          for (const Expr *defaultArgument : construction->defaultArguments) {
            if (const std::optional<HirValueId> lowered = lowerDefaultArgument(
                    defaultArgument, defaultModel, targetClassArguments,
                    targetClassValueArguments, body)) {
              value.operands.push_back(*lowered);
            }
          }
        }
      }
    }
    if (kind == HirValueKind::MemberAccess && value.symbol != 0) {
      const SymbolRecord *member = model.database().findSymbol(value.symbol);
      value.storedReferenceAccess =
          member != nullptr && member->type.kind == SemanticType::Reference;
    }
    if (const ResolvedOperatorInfo *resolved = model.findOperator(*raw);
        resolved != nullptr && resolved->function != 0) {
      value.parameterTypes = resolved->parameterTypes;
      value.dispatch = resolved->dispatch;
      value.dispatchOwner = resolved->dispatchOwner;
      value.callableBoundary = resolved->boundary;
      if (resolved->kind == OverloadedOperator::Call) {
        value.callableInvocation = resolved->capability;
      }
      value.borrowOrigin = resolved->borrowOrigin;
      value.borrowArgument = resolved->borrowArgument;
      value.borrowAccess = resolved->borrowAccess;
      if ((resolved->kind == OverloadedOperator::Call ||
           resolved->kind == OverloadedOperator::Assignment) &&
          !value.operands.empty()) {
        value.receiver = value.operands.front();
      }
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = resolved->dispatchOwner;
        if (const auto *binary = dynamic_cast<const Binary *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*binary->left());
          }
        } else if (const auto *get = dynamic_cast<const Get *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*get->object());
          }
        } else if (const auto *index = dynamic_cast<const Index *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*index->object());
          }
        } else if (const auto *call = dynamic_cast<const Call *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*call->callee());
          }
        } else if (const auto *unary = dynamic_cast<const Unary *>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*unary->right());
          }
        } else if (dynamic_cast<const Assign *>(raw) != nullptr) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            const SymbolRecord *receiverSymbol =
                model.database().findSymbol(value.symbol);
            receiverType = receiverSymbol == nullptr ? SemanticType::Unknown
                                                     : receiverSymbol->type;
          }
        }
        const std::vector<SemanticType> ownerArguments =
            receiverType.kind == SemanticType::Class ? receiverType.arguments
                                                     : classArguments;
        const std::vector<CompileTimeValue> ownerValueArguments =
            receiverType.kind == SemanticType::Class
                ? receiverType.valueArguments
                : classValueArguments;
        value.functionTarget =
            enqueueFunction(*target, ownerArguments, ownerValueArguments, {},
                            {}, resolved->returnType, resolved->parameterTypes);
      }
    }
    if (const ResolvedOperatorInfo *resolved =
            model.findContextualConversion(*raw);
        resolved != nullptr && resolved->function != 0) {
      value.dispatch = resolved->dispatch;
      value.dispatchOwner = resolved->dispatchOwner;
      value.borrowOrigin = resolved->borrowOrigin;
      value.borrowArgument = resolved->borrowArgument;
      value.borrowAccess = resolved->borrowAccess;
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = resolved->dispatchOwner;
        if (receiverType.kind != SemanticType::Class ||
            receiverType.classId != target->ownerClass) {
          receiverType = model.typeOf(*raw);
        }
        const std::vector<SemanticType> ownerArguments =
            receiverType.kind == SemanticType::Class ? receiverType.arguments
                                                     : classArguments;
        const std::vector<CompileTimeValue> ownerValueArguments =
            receiverType.kind == SemanticType::Class
                ? receiverType.valueArguments
                : classValueArguments;
        contextualBoolTarget =
            enqueueFunction(*target, ownerArguments, ownerValueArguments, {},
                            {}, resolved->returnType, resolved->parameterTypes);
      }
    }
    if (const auto *call = dynamic_cast<const Call *>(raw);
        call != nullptr && kind == HirValueKind::Call &&
        value.intrinsic == IntrinsicKind::None && !value.constructorTarget &&
        value.functionTarget &&
        (model.findOperator(*call) == nullptr ||
         model.findOperator(*call)->kind == OverloadedOperator::Call) &&
        model.findLambdaCall(*call) == nullptr &&
        (model.findDeferredCallableCall(*call) == nullptr ||
         model.findOperator(*call) != nullptr) &&
        value.parameterTypes.size() >= call->arguments().size() &&
        value.parameterTypes.size() <= value.operands.size()) {
      const std::size_t argumentCount = value.parameterTypes.size();
      const bool exactOperands = argumentCount <= value.operands.size();
      std::vector<HirValueId> arguments;
      if (exactOperands) {
        arguments.assign(value.operands.end() -
                             static_cast<std::ptrdiff_t>(argumentCount),
                         value.operands.end());
      }
      const std::optional<std::vector<HirCallArgument>> plannedArguments =
          exactOperands ? orderedArguments(arguments, value.parameterTypes,
                                           body, call->arguments().size())
                        : std::nullopt;
      const ResolvedCallInfo *resolved = model.findCall(*call);
      const ResolvedOperatorInfo *resolvedOperator = model.findOperator(*call);
      const FunctionInfo *resolvedTarget =
          resolved == nullptr || resolved->function == 0
              ? nullptr
              : baseModel->findFunction(resolved->function);
      std::optional<HirValueId> callReceiver;
      ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
      CallReceiverMode receiverMode = CallReceiverMode::None;
      bool supportedReceiver = true;
      if (resolvedOperator != nullptr) {
        callReceiver = value.receiver;
        receiverMutability = resolvedOperator->receiverMutability;
        receiverMode = resolvedOperator->receiverMode;
        supportedReceiver = callReceiver.has_value() &&
                            body.findValue(*callReceiver) != nullptr;
      } else if (resolved != nullptr && resolved->declaration != nullptr &&
                 resolvedTarget != nullptr && resolvedTarget->ownerClass != 0 &&
                 !resolved->declaration->isStatic()) {
        const HirValue *callee = value.operands.empty()
                                     ? nullptr
                                     : body.findValue(value.operands.front());
        if (callee != nullptr && callee->kind == HirValueKind::MemberAccess &&
            !callee->operands.empty()) {
          supportedReceiver =
              callee->unsafeOperation != UnsafeOperationKind::RawMember;
          callReceiver = callee->operands.front();
        } else {
          callReceiver = value.receiver;
        }
        supportedReceiver = supportedReceiver && callReceiver.has_value() &&
                            body.findValue(*callReceiver) != nullptr;
        receiverMutability = resolved->declaration->receiverMutability();
        receiverMode = resolved->receiverMode;
      }
      if (plannedArguments && supportedReceiver) {
        HirCallPlan plan;
        if (callReceiver) {
          plan.receiver = orderedReceiver(*callReceiver, receiverMutability,
                                          receiverMode, body);
          if (!plan.receiver) {
            supportedReceiver = false;
          }
        }
        if (supportedReceiver) {
          plan.arguments = *plannedArguments;
          value.callPlan = std::move(plan);
        }
      }
    }
    if (model.findConstruction(*raw) != nullptr && value.constructorTarget &&
        value.constructorKind == ConstructorKind::Ordinary &&
        value.intrinsic == IntrinsicKind::None &&
        !value.parameterTypes.empty() &&
        value.parameterTypes.size() <= value.operands.size()) {
      std::vector<HirValueId> arguments(
          value.operands.end() -
              static_cast<std::ptrdiff_t>(value.parameterTypes.size()),
          value.operands.end());
      const std::size_t explicitArgumentCount =
          dynamic_cast<const Call *>(raw) != nullptr
              ? dynamic_cast<const Call *>(raw)->arguments().size()
          : dynamic_cast<const DirectInitializer *>(raw) != nullptr
              ? dynamic_cast<const DirectInitializer *>(raw)->arguments().size()
              : value.parameterTypes.size();
      if (std::optional<std::vector<HirCallArgument>> plannedArguments =
              orderedArguments(arguments, value.parameterTypes, body,
                               explicitArgumentCount)) {
        value.callPlan = HirCallPlan{.receiver = std::nullopt,
                                     .arguments = std::move(*plannedArguments)};
      }
    }
    if (!value.callPlan && value.functionTarget && !value.operands.empty()) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*raw);
      const bool freshReceiver =
          resolved != nullptr &&
          (resolved->receiverMode == CallReceiverMode::FreshTemporaryRead ||
           resolved->receiverMode == CallReceiverMode::FreshTemporaryMutable);
      if (freshReceiver &&
          value.operands.size() >= value.parameterTypes.size() + 1) {
        const HirValueId receiverValue = value.operands.front();
        std::vector<HirValueId> arguments(
            value.operands.end() -
                static_cast<std::ptrdiff_t>(value.parameterTypes.size()),
            value.operands.end());
        const std::optional<HirCallReceiver> plannedReceiver =
            orderedReceiver(receiverValue, resolved->receiverMutability,
                            resolved->receiverMode, body);
        const std::optional<std::vector<HirCallArgument>> plannedArguments =
            orderedArguments(arguments, value.parameterTypes, body);
        if (plannedReceiver && plannedArguments) {
          value.receiver = receiverValue;
          value.callPlan = HirCallPlan{.receiver = *plannedReceiver,
                                       .arguments = *plannedArguments};
        } else {
          lifecycleValid = false;
        }
      }
    }
    if ((value.kind == HirValueKind::MemberAccess ||
         value.kind == HirValueKind::MemberSet) &&
        value.operation == TokenKind::ARROW && value.functionTarget &&
        !value.operands.empty()) {
      if (const ResolvedOperatorInfo *resolved = model.findOperator(*raw);
          resolved != nullptr && resolved->kind == OverloadedOperator::Arrow) {
        return splitOverloadedArrowProjection(std::move(value), *resolved,
                                              body);
      }
    }

    if (const ResolvedOperatorInfo *resolved =
            model.findContextualConversion(*raw);
        resolved != nullptr && contextualBoolTarget) {
      const HirValueId receiverValue = value.id;
      const HirFunctionInstanceId target = *contextualBoolTarget;
      body.values.push_back(std::move(value));
      output.program.sourceValueIds[raw].push_back(receiverValue);

      HirValue conversion{
          .id = nextValueId++,
          .kind = HirValueKind::Call,
          .source = raw,
          .info = {.type = SemanticType::Bool,
                   .category = ValueCategory::Value,
                   .access = AccessMode::ReadOnly,
                   .traits = semanticTraits(SemanticType::Bool)},
          .operands = {receiverValue},
          .definedFailure = {.propagation =
                                 resolved->dispatch == CallDispatch::Virtual
                                     ? FailurePropagationKind::VirtualCall
                                     : FailurePropagationKind::DirectCall},
          .dispatch = resolved->dispatch,
          .dispatchOwner = resolved->dispatchOwner,
          .receiver = receiverValue,
          .functionTarget = target,
          .callableBoundary = resolved->boundary,
      };
      if (const std::optional<HirCallReceiver> planned =
              orderedReceiver(receiverValue, resolved->receiverMutability,
                              resolved->receiverMode, body)) {
        conversion.callPlan =
            HirCallPlan{.receiver = *planned, .arguments = {}};
      } else {
        lifecycleValid = false;
      }

      const HirValueId conversionId = conversion.id;
      body.values.push_back(std::move(conversion));
      output.program.sourceValueIds[raw].push_back(conversionId);
      return conversionId;
    }

    const HirValueId id = value.id;
    body.values.push_back(std::move(value));
    output.program.sourceValueIds[raw].push_back(id);
    return id;
  }

  TargetInfo target;
  SemanticVisitor *analyzer = nullptr;
  const SemanticModel *baseModel = nullptr;
  HirLoweringResult output;
  HirValueId nextValueId = 1;
  HirBindingId nextBindingId = 1;
  HirStatementId nextStatementId = 1;
  std::size_t placeSnapshotId = 0;
  std::size_t nextPlaceBodyId = 1;
  bool lifecycleValid = true;
  std::size_t processedClasses = 0;
  HirInstanceIndex instanceIndex;
  std::size_t processedFunctions = 0;
  std::size_t processedConstructors = 0;
  std::size_t processedDestructors = 0;
  SemanticType currentReceiverType = SemanticType::Unknown;
  AccessMode currentReceiverAccess = AccessMode::ReadOnly;
  std::unordered_map<LambdaId, HirLambdaId> lambdaTargets;
  std::unordered_map<const VariableDecl *, LoweredProgramStorage>
      loweredProgramStorage;
  std::unordered_map<HirFunctionInstanceId,
                     std::unique_ptr<SemanticInstanceAnalysis>>
      functionAnalyses;
  std::unordered_map<HirConstructorInstanceId,
                     std::unique_ptr<SemanticInstanceAnalysis>>
      constructorAnalyses;
  std::unordered_set<const Expr *> activeDefaultArguments;
};

HirLowerer::HirLowerer(TargetInfo target)
    : impl(std::make_unique<Impl>(std::move(target))) {}

HirLowerer::~HirLowerer() = default;
HirLowerer::HirLowerer(HirLowerer &&) noexcept = default;
HirLowerer &HirLowerer::operator=(HirLowerer &&) noexcept = default;

HirLoweringResult HirLowerer::lower(const Program &source,
                                    SemanticVisitor &semantics) {
  return impl->lower(source, semantics);
}

} // namespace lang
