#include "gti/hir.h"

#include "diagnostic_registry.h"
#include "gti/semantic_analyzer.h"

#include <unordered_set>

namespace lang {

class HirLowerer::Impl {
public:
  explicit Impl(TargetInfo target, HirLoweringScope scope)
      : target(std::move(target)),
        reachablePrefixRequested(scope.reachablePrefix),
        entryDeclarations(scope.entryDeclarations),
        completeDeclarations(std::move(scope.completeDeclarations)),
        omitLibraryOnlyBodies(scope.omitLibraryOnlyBodies),
        trustedUnits(scope.trustedUnits.begin(), scope.trustedUnits.end()) {}

  [[nodiscard]] HirLoweringResult lowerConstantFunction(
      const SemanticModel &semantics, SemanticInstanceProvider &provider,
      FunctionId root, const std::vector<FunctionId> &completedFunctions,
      const GenericSubstitution &substitution,
      const std::vector<ConstructorId> &completedConstructors) {
    initializeConstant(semantics, provider, completedFunctions,
                       completedConstructors);
    const FunctionInfo *function = semantics.findFunction(root);
    if (function == nullptr) {
      output.program.valid_ = false;
      return std::move(output);
    }
    std::vector<SemanticType> types;
    std::vector<CompileTimeValue> values;
    std::vector<SemanticType> ownerTypes;
    std::vector<CompileTimeValue> ownerValues;
    if (function->ownerClass != 0) {
      const auto *owner = semantics.findClassType(function->ownerClass);
      if (owner == nullptr) {
        output.program.valid_ = false;
        return std::move(output);
      }
      for (const auto &parameter : owner->genericParameters) {
        if (parameter.pack ||
            (parameter.value ? !substitution.values.contains(parameter.id)
                             : !substitution.types.contains(parameter.id))) {
          output.program.valid_ = false;
          return std::move(output);
        }
        if (parameter.value)
          ownerValues.push_back(substitution.values.at(parameter.id));
        else
          ownerTypes.push_back(substitution.types.at(parameter.id));
      }
    }
    for (const auto &parameter : function->genericParameters) {
      if (parameter.pack ||
          (parameter.value ? !substitution.values.contains(parameter.id)
                           : !substitution.types.contains(parameter.id))) {
        output.program.valid_ = false;
        return std::move(output);
      }
      if (parameter.value)
        values.push_back(substitution.values.at(parameter.id));
      else
        types.push_back(substitution.types.at(parameter.id));
    }
    auto parameters = function->parameterTypes;
    for (auto &type : parameters)
      type = substitute(type, substitution);
    if (enqueueFunction(*function, ownerTypes, ownerValues, std::move(types),
                        std::move(values),
                        substitute(function->returnType, substitution),
                        std::move(parameters)) == 0) {
      output.program.valid_ = false;
      return std::move(output);
    }
    return finishConstant();
  }

  [[nodiscard]] HirLoweringResult lowerConstantConstructor(
      const SemanticModel &semantics, SemanticInstanceProvider &provider,
      const ResolvedConstructionInfo &root,
      const std::vector<FunctionId> &completedFunctions,
      const std::vector<ConstructorId> &completedConstructors) {
    initializeConstant(semantics, provider, completedFunctions,
                       completedConstructors);
    if (enqueueConstructor(root) == 0) {
      output.program.valid_ = false;
      return std::move(output);
    }
    return finishConstant();
  }

  [[nodiscard]] HirLoweringResult lower(const Program &source,
                                        const SemanticModel &semantics,
                                        SemanticInstanceProvider &provider) {
    initialize(semantics, provider);
    if (!baseModel->analysisSeal().matchesProgram(source, target)) {
      output.program.valid_ = false;
      return std::move(output);
    }
    // For a reachable prefix, discover everything reachable from the program
    // roots first. Every instance is appended to its table when first
    // enqueued and is complete once processed, so the tables at this point
    // are a closed prefix of the final program: nothing discovered later is
    // referenced from it. An entry-unit scope seeds the roots and the unit's
    // own declarations and never seeds the rest. Otherwise every declaration
    // is seeded first, which keeps the complete program's established
    // instance order.
    if (entryDeclarations) {
      seedProgramRoots(source.declarations());
      const StmtList &declarations = source.declarations();
      const std::size_t end =
          std::min(declarations.size(),
                   entryDeclarations->first + entryDeclarations->second);
      for (std::size_t index = entryDeclarations->first; index < end; ++index) {
        seedDeclaration(declarations[index], std::nullopt);
      }
    } else if (reachablePrefixRequested) {
      seedProgramRoots(source.declarations());
    } else {
      seedDeclarations(source.declarations(), std::nullopt);
    }
    lowerEmbeddingPlan();
    lowerProgramInitializationPlan();
    lowerHostedProgramEntryPlan();
    finalizeLifetimes(output.program.moduleBody, *baseModel, false);
    lowerLoans(*baseModel, output.program.moduleBody);
    processPendingInstances();
    const InstanceCounts reachable = instanceCounts();
    // Every other declaration in scope is still lowered: concrete semantic
    // analysis of its instances is what reports their diagnostics.
    if (reachablePrefixRequested && !entryDeclarations) {
      if (completeDeclarations) {
        const StmtList &declarations = source.declarations();
        for (const auto &[first, count] : *completeDeclarations) {
          const std::size_t end = std::min(declarations.size(), first + count);
          for (std::size_t index = first; index < end; ++index) {
            seedDeclaration(declarations[index], std::nullopt);
          }
        }
      } else {
        seedDeclarations(source.declarations(), std::nullopt);
      }
      processPendingInstances();
    }
    output.program.valid_ = output.requiredSemanticBodies.empty() &&
                            !hasErrorDiagnostics(output.diagnostics) &&
                            lifecycleValid;
    if (output.program.valid_ &&
        !verifyHirProgramPlans(*baseModel, output.program).valid()) {
      output.program.valid_ = false;
    }
    if (output.program.valid_ && reachable != instanceCounts()) {
      HirProgram prefix = reachablePrefix(reachable);
      if (verifyHirProgramPlans(*baseModel, prefix).valid()) {
        output.reachable = std::move(prefix);
      }
    }
    return std::move(output);
  }

private:
  void
  initializeConstant(const SemanticModel &semantics,
                     SemanticInstanceProvider &provider,
                     const std::vector<FunctionId> &completedFunctions,
                     const std::vector<ConstructorId> &completedConstructors) {
    initialize(semantics, provider);
    constantFunctions = completedFunctions;
    constantConstructors = completedConstructors;
    constantClosure = true;
    // Preserve source-unit/target identity for failure origins without
    // claiming completed whole-program semantic analysis.
    output.program.semanticAnalysisSeal = {
        .target = target, .sourceGraph = semantics.analysisSeal().sourceGraph};
  }

  HirLoweringResult finishConstant() {
    ensurePlaceDomain(output.program.moduleBody);
    processPendingInstances();
    output.program.valid_ =
        output.requiredSemanticBodies.empty() &&
        !hasErrorDiagnostics(output.diagnostics) && lifecycleValid &&
        output.program.destructors.empty() && output.program.lambdas.empty();
    return std::move(output);
  }

  void initialize(const SemanticModel &semantics,
                  SemanticInstanceProvider &provider) {
    instanceProvider = &provider;
    baseModel = &semantics;
    output = {};
    output.program.executionProfile_ = target.executionProfile;
    output.program.programKind_ = baseModel->programKind();
    output.program.semanticAnalysisSeal = baseModel->analysisSeal();
    constantClosure = false;
    constantFunctions.clear();
    constantConstructors.clear();
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
    discoveryDepth = 0;
    discoverySite.reset();
    discoveryOrigin.reset();
    discoveryLimitReached = false;
    discoveredGenericInstances = 0;
    classDiscoveryDepths.clear();
    functionDiscoveryDepths.clear();
    constructorDiscoveryDepths.clear();
    destructorDiscoveryDepths.clear();
    lambdaTargets.clear();
    loweredProgramStorage.clear();
    functionAnalyses.clear();
    constructorAnalyses.clear();
    activeDefaultArguments.clear();
    activeDefaultSourceLocation.reset();
    currentSourceFunctionName.clear();
    collectionResults.clear();
  }
  struct LoweredProgramStorage {
    HirBindingId binding = 0;
    std::optional<HirValueId> initializer;
    HirStatementId statement = 0;
  };

  // These are compilation resource bounds, not a termination proof or a
  // semantic restriction on exact recursive calls. Existing canonical
  // instances never consume another step in the discovery chain.
  static constexpr std::size_t maximumDiscoveryDepth = 64;
  static constexpr std::size_t maximumDiscoveredInstances = 16384;

  struct InstanceDiscovery {
    std::size_t depth;
    std::optional<SourceSpan> origin;
  };

  [[nodiscard]] InstanceDiscovery
  nextDiscovery(const std::optional<SourceSpan> &site) const {
    return {discoveryDepth + 1, discoveryOrigin ? discoveryOrigin : site};
  }

  struct DiscoveryScope {
    DiscoveryScope(std::size_t &depth, std::optional<SourceSpan> &site,
                   std::optional<SourceSpan> &origin,
                   const InstanceDiscovery &next,
                   std::optional<SourceSpan> nextSite)
        : depth(depth), site(site), origin(origin), previous(depth),
          previousSite(site), previousOrigin(origin) {
      depth = next.depth;
      site = std::move(nextSite);
      origin = next.origin;
    }
    ~DiscoveryScope() {
      depth = previous;
      site = std::move(previousSite);
      origin = std::move(previousOrigin);
    }
    std::size_t &depth;
    std::optional<SourceSpan> &site;
    std::optional<SourceSpan> &origin;
    std::size_t previous;
    std::optional<SourceSpan> previousSite;
    std::optional<SourceSpan> previousOrigin;
  };

  [[nodiscard]] bool admitInstance(const Token &declaration,
                                   const std::optional<SourceSpan> &site,
                                   bool generic) {
    if (discoveryLimitReached) {
      return false;
    }
    if (discoveryDepth < maximumDiscoveryDepth &&
        (!generic || discoveredGenericInstances < maximumDiscoveredInstances)) {
      return true;
    }
    const bool depthLimit = discoveryDepth >= maximumDiscoveryDepth;
    const std::string limit = std::to_string(
        depthLimit ? maximumDiscoveryDepth : maximumDiscoveredInstances);
    auto diagnostic = makeDiagnostic(
        DiagnosticId::GenericInstantiationLimit,
        site.value_or(discoverySite.value_or(tokenSpan(declaration))),
        {depthLimit ? "discovery-depth" : "distinct-instance", limit});
    diagnostic.related.push_back(
        {tokenSpan(declaration),
         "Declaration requiring this concrete instance."});
    diagnostic.hints.emplace_back(
        "Ensure type/value-changing generic recursion terminates through a "
        "selected compile-time branch or exact specialization; runtime "
        "conditions do not limit instantiation.");
    output.diagnostics.push_back(std::move(diagnostic));
    discoveryLimitReached = true;
    lifecycleValid = false;
    return false;
  }

  [[nodiscard]] SemanticType
  substitute(const SemanticType &type,
             const GenericSubstitution &substitution) const {
    return substituteSemanticType(type, substitution, baseModel);
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
      if (!parameter.typeArgumentPath.empty()) {
        if (auto type = projectGenericTypeArgument(
                typeArguments, parameter.typeArgumentPath, parameter.id,
                parameter.pack)) {
          result.types.emplace(parameter.id, std::move(*type));
        }
      } else if (parameter.pack) {
        std::vector<SemanticType> elements(
            typeArguments.begin() + static_cast<std::ptrdiff_t>(typeIndex),
            typeArguments.end());
        result.types.emplace(
            parameter.id,
            SemanticType::concreteTypePack(parameter.id, std::move(elements)));
        typeIndex = typeArguments.size();
      } else if (parameter.value) {
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
    // An existing instance discovered its arguments when it was created, and
    // admission is monotone, so revisiting them can only propagate a first
    // instantiation site. Re-walking them on every hit made each lookup of a
    // nested generic proportional to its whole argument tree.
    if (resolvedType.kind == SemanticType::Class && resolvedType.classId != 0) {
      if (const std::optional<std::size_t> existing = instanceIndex.findClass(
              resolvedType.classId, resolvedType.arguments,
              resolvedType.valueArguments);
          existing &&
          (!site || output.program.classes[*existing - 1].instantiationSite)) {
        return *existing;
      }
    }
    for (const SemanticType &argument : resolvedType.arguments) {
      (void)enqueueClass(argument, site);
    }
    if (resolvedType.kind == SemanticType::Enum) {
      enqueueEnum(resolvedType);
      return std::nullopt;
    }
    if (resolvedType.kind != SemanticType::Class || resolvedType.classId == 0) {
      return std::nullopt;
    }
    if (const std::optional<std::size_t> existing = instanceIndex.findClass(
            resolvedType.classId, resolvedType.arguments,
            resolvedType.valueArguments)) {
      HirClassInstance &instance = output.program.classes[*existing - 1];
      if (!instance.instantiationSite && site) {
        classDiscoveryDepths[*existing - 1].origin =
            discoveryOrigin ? discoveryOrigin : site;
        instance.instantiationSite = std::move(site);
      }
      return *existing;
    }
    const ClassTypeInfo *declaration =
        baseModel->findClassType(resolvedType.classId);
    if (declaration == nullptr || declaration->declaration == nullptr) {
      return std::nullopt;
    }
    // These trusted declarations establish primitive identities in semantics;
    // they have no nominal class representation or executable lifecycle.
    if (declaration->compilerCapability ==
        CompilerCapabilityTypeKind::NativeCScalar) {
      return std::nullopt;
    }
    const bool generic = !declaration->genericParameters.empty();
    if (!admitInstance(declaration->declaration->name(), site, generic)) {
      return std::nullopt;
    }
    const ClassLifecycleInfo *lifecycle =
        baseModel->findClassLifecycle(*declaration->declaration);
    const bool lifecycleBoundBorrowCarrier =
        instanceProvider->lifecycleBoundBorrowCarrierFor(resolvedType);
    // This staged family admits passive structural values, not arbitrary
    // class capabilities or executable lifecycle bodies. The ordinary class
    // catalog still owns exact substituted fields and initializer metadata.
    if (constantClosure &&
        (declaration->kind == ClassKind::Union || declaration->polymorphic ||
         !declaration->bases.empty() || lifecycleBoundBorrowCarrier ||
         declaration->compilerCapability != CompilerCapabilityTypeKind::None ||
         instanceProvider->requiresActiveCleanupFor(resolvedType))) {
      lifecycleValid = false;
      return std::nullopt;
    }
    const HirClassInstanceId id = output.program.classes.size() + 1;
    const GenericSubstitution capabilitySubstitution = classSubstitution(
        *declaration, resolvedType.arguments, resolvedType.valueArguments);
    const auto concreteCondition =
        [&](const std::optional<SemanticType> &condition)
        -> std::optional<SemanticType> {
      return condition
                 ? std::optional(substitute(*condition, capabilitySubstitution))
                 : std::nullopt;
    };
    SemanticTypeTraits instanceTraits =
        instanceProvider->traitsFor(resolvedType);
    if (declaration->compilerCapability ==
        CompilerCapabilityTypeKind::OptionalStorage) {
      instanceTraits.copyable = false;
      instanceTraits.copyAssignable = false;
      instanceTraits.moveAssignable = false;
    }
    const auto concreteSpecialMember = [](SpecialMemberStatus status,
                                          bool available) {
      return status == SpecialMemberStatus::Generated && !available
                 ? SpecialMemberStatus::Deleted
                 : status;
    };
    output.program.classes.push_back(
        {.id = id,
         .sourceUnit = declaration->sourceUnit,
         .declaration = resolvedType.classId,
         .source = declaration->declaration,
         .typeArguments = resolvedType.arguments,
         .valueArguments = resolvedType.valueArguments,
         .instantiationSite = std::move(site),
         .type = resolvedType,
         .traits = instanceTraits,
         .compilerCapability = declaration->compilerCapability,
         .lifecycleBoundBorrowCarrier = lifecycleBoundBorrowCarrier,
         .mutexGuardBorrowCarrier = declaration->mutexGuardBorrowCarrier,
         .transferPolicy = declaration->transferPolicy,
         .sharePolicy = declaration->sharePolicy,
         .transferCondition = concreteCondition(declaration->transferCondition),
         .shareCondition = concreteCondition(declaration->shareCondition),
         .kind = declaration->kind,
         .abstract = declaration->abstract,
         .polymorphic = declaration->polymorphic,
         .cAbiRecord = declaration->cAbiRecord,
         .cOpaqueHandle = declaration->cOpaqueHandle,
         .cAbiLayout = declaration->cAbiLayout,
         .unionLayout = declaration->unionLayout,
         .defaultConstructor = lifecycle == nullptr
                                   ? SpecialMemberStatus::Deleted
                                   : lifecycle->defaultConstructor,
         .generatedDefaultRequiresSchedule =
             lifecycle != nullptr &&
             (lifecycle->generatedDefaultFieldInitializers ||
              lifecycle->generatedDefaultBaseConstructor != 0),
         .copyConstructor =
             lifecycle == nullptr
                 ? SpecialMemberStatus::Deleted
                 : concreteSpecialMember(lifecycle->copyConstructor,
                                         instanceTraits.copyable),
         .moveConstructor =
             lifecycle == nullptr
                 ? SpecialMemberStatus::Deleted
                 : concreteSpecialMember(lifecycle->moveConstructor,
                                         instanceTraits.movable),
         .copyAssignment =
             lifecycle == nullptr
                 ? SpecialMemberStatus::Deleted
                 : concreteSpecialMember(lifecycle->copyAssignment,
                                         instanceTraits.copyAssignable),
         .moveAssignment =
             lifecycle == nullptr
                 ? SpecialMemberStatus::Deleted
                 : concreteSpecialMember(lifecycle->moveAssignment,
                                         instanceTraits.moveAssignable),
         .destructorStatus = lifecycle == nullptr ? SpecialMemberStatus::Deleted
                                                  : lifecycle->destructor,
         .requiresActiveDropState =
             lifecycle != nullptr && lifecycle->requiresActiveDropState,
         .requiresActiveCleanup =
             instanceProvider->requiresActiveCleanupFor(resolvedType)});
    instanceIndex.recordClass(resolvedType.classId, resolvedType.arguments,
                              resolvedType.valueArguments, id);
    classDiscoveryDepths.push_back(
        nextDiscovery(output.program.classes.back().instantiationSite));
    discoveredGenericInstances += generic;
    if (declaration->enclosingClass != 0) {
      // Keep the lexical declaration shell even when only its child is used.
      // No outer object is constructed or captured. Register this instance
      // before following the parent, whose fields can refer back to the child.
      (void)enqueueClass(SemanticType::classType(declaration->enclosingClass));
    }
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
    if (constantClosure &&
        (declaration.declaration == nullptr ||
         !declaration.declaration->isConstexpr() ||
         std::find(constantFunctions.begin(), constantFunctions.end(),
                   declaration.id) == constantFunctions.end())) {
      lifecycleValid = false;
      return 0;
    }
    // Calls can discover an instance through individual arguments or through
    // one forwarded pack. Virtual members are also seeded from declarations.
    // These paths must publish the same concrete signature; source pack
    // bindings remain grouped in the body for forwarding and cleanup.
    std::vector<SemanticType> concreteParameters;
    for (SemanticType &parameter : parameterTypes) {
      SemanticType::appendExpandedArgument(concreteParameters,
                                           std::move(parameter));
    }
    parameterTypes = std::move(concreteParameters);
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
    const ClassTypeInfo *genericOwner =
        declaration.ownerClass == 0
            ? nullptr
            : baseModel->findClassType(declaration.ownerClass);
    const bool generic =
        !declaration.genericParameters.empty() ||
        (genericOwner != nullptr && !genericOwner->genericParameters.empty());
    if (!admitInstance(declarationToken, site, generic)) {
      return 0;
    }
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
    // Discovering signature classes may consume the final inventory entry.
    // Recheck immediately before publication, as well as before expansion.
    if (!admitInstance(declarationToken, site, generic)) {
      return 0;
    }
    const HirFunctionInstanceId id = output.program.functions.size() + 1;
    functionDiscoveryDepths.push_back(nextDiscovery(site));
    discoveredGenericInstances += generic;
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
         .source = declaration.bodyDeclaration(),
         .owner = owner,
         .typeArguments = std::move(functionTypeArguments),
         .valueArguments = std::move(functionValueArguments),
         .returnType = std::move(returnType),
         .parameterTypes = std::move(parameterTypes),
         .entryKind = declaration.entryKind,
         .returnBorrowOrigin = declaration.returnBorrowOrigin,
         .returnBorrowParameter = declaration.returnBorrowParameter,
         .returnBorrowAccess = declaration.returnBorrowAccess,
         .returnBorrowParameters = declaration.returnBorrowParameters,
         .returnBorrowFields = declaration.returnBorrowFields,
         .returnBorrowPlace = declaration.returnBorrowPlace,
         .ownedPointeeProjection = declaration.ownedPointeeProjection,
         .instantiationSite = std::move(site),
         .staticMember = declaration.staticMember,
         .internalLinkage = declaration.internalLinkage,
         .constexprFunction = declaration.constexprFunction,
         .requiresUnsafe = declaration.requiresUnsafe,
         .neverReturns = declaration.neverReturns,
         .tracksCaller = declaration.tracksCaller,
         .cThreadOperation = declaration.cThreadOperation,
         .processMemoryOperation = declaration.processMemoryOperation,
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
    seedInstanceSignatureClasses(
        output.program.functions[id - 1].typeArguments,
        output.program.functions[id - 1].returnType,
        output.program.functions[id - 1].parameterTypes);
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
        {.id = id,
         .target = function,
         .type = conversion.type,
         .cThreadCallback = conversion.cThreadCallback});
    return id;
  }

  [[nodiscard]] HirConstructorInstanceId
  enqueueConstructor(const ResolvedConstructionInfo &construction,
                     std::optional<SourceSpan> site = std::nullopt) {
    const auto *declaration =
        construction.declaration == nullptr
            ? nullptr
            : baseModel->findConstructor(*construction.declaration);
    const bool constant =
        declaration != nullptr && declaration->constexprConstructor;
    if (constantClosure && !construction.generatedDefault &&
        (!constant ||
         std::find(constantConstructors.begin(), constantConstructors.end(),
                   construction.constructor) == constantConstructors.end())) {
      lifecycleValid = false;
      return 0;
    }
    const std::optional<HirClassInstanceId> owner =
        enqueueClass(construction.constructedType, site);
    const ClassTypeInfo *ownerType =
        owner ? baseModel->findClassType(construction.constructedType.classId)
              : nullptr;
    const ClassLifecycleInfo *ownerLifecycle =
        ownerType != nullptr && ownerType->declaration != nullptr
            ? baseModel->findClassLifecycle(*ownerType->declaration)
            : nullptr;
    const bool generatedBody =
        construction.generatedDefault && ownerLifecycle != nullptr &&
        (ownerLifecycle->generatedDefaultBaseConstructor != 0 ||
         ownerLifecycle->generatedDefaultFieldInitializers);
    if (!owner || construction.kind != ConstructorKind::Ordinary ||
        (!generatedBody && (construction.generatedDefault ||
                            construction.declaration == nullptr ||
                            construction.constructor == 0))) {
      return 0;
    }
    const ConstructorId identity = generatedBody ? 0 : construction.constructor;
    if (const std::optional<std::size_t> existing =
            instanceIndex.findConstructor(identity, *owner,
                                          construction.typeArguments,
                                          construction.valueArguments)) {
      HirConstructorInstance &instance =
          output.program.constructors[*existing - 1];
      if (construction.borrowOrigin != BorrowOriginKind::None) {
        instance.borrowOrigin = construction.borrowOrigin;
        instance.borrowParameter = construction.borrowArgument;
        instance.borrowParameters = construction.borrowArguments;
        instance.borrowAccess = construction.borrowAccess;
      }
      return instance.id;
    }
    const HirConstructorInstanceId id = output.program.constructors.size() + 1;
    const Token &declarationToken = construction.declaration == nullptr
                                        ? ownerType->declaration->name()
                                        : construction.declaration->name();
    const bool generic =
        !ownerType->genericParameters.empty() ||
        (declaration != nullptr && !declaration->genericParameters.empty());
    // A generated default is the already-admitted owner's field schedule,
    // not another source instantiation edge. Retain that owner's depth while
    // still counting a distinct generic constructor toward the request budget.
    // Ordinary constructor bodies retain their independent discovery edge.
    const InstanceDiscovery constructorDiscovery =
        generatedBody ? classDiscoveryDepths[*owner - 1] : nextDiscovery(site);
    const bool admitted = [&] {
      DiscoveryScope scope(discoveryDepth, discoverySite, discoveryOrigin,
                           {.depth = constructorDiscovery.depth - 1,
                            .origin = constructorDiscovery.origin},
                           site ? site : discoverySite);
      return admitInstance(declarationToken, site, generic);
    }();
    if (!admitted) {
      return 0;
    }
    constructorDiscoveryDepths.push_back(constructorDiscovery);
    discoveredGenericInstances += generic;
    output.program.constructors.push_back(
        {.id = id,
         .sourceUnit = output.program.classes[*owner - 1].sourceUnit,
         .declaration = identity,
         .source = generatedBody ? nullptr : construction.declaration,
         .generatedDefault = generatedBody,
         .constexprConstructor = constant,
         .requiresUnsafe =
             declaration != nullptr && declaration->requiresUnsafe,
         .owner = *owner,
         .typeArguments = construction.typeArguments,
         .valueArguments = construction.valueArguments,
         .parameterTypes = construction.parameterTypes,
         .borrowOrigin = construction.borrowOrigin,
         .borrowParameter = construction.borrowArgument,
         .borrowAccess = construction.borrowAccess,
         .borrowParameters = construction.borrowArguments,
         .instantiationSite = std::move(site)});
    instanceIndex.recordConstructor(identity, *owner,
                                    construction.typeArguments,
                                    construction.valueArguments, id);
    seedInstanceSignatureClasses(
        output.program.constructors[id - 1].typeArguments, SemanticType::Void,
        output.program.constructors[id - 1].parameterTypes);
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
        lifecycle->declaredDestructor->declaration == nullptr ||
        lifecycle->destructor != SpecialMemberStatus::Declared ||
        lifecycle->declaredDestructor->declaration->body() == nullptr) {
      return std::nullopt;
    }
    if (const std::optional<std::size_t> existing =
            instanceIndex.findDestructor(owner)) {
      return *existing;
    }
    const HirDestructorInstanceId id = output.program.destructors.size() + 1;
    const bool generic = !classType->genericParameters.empty();
    if (!admitInstance(lifecycle->declaredDestructor->declaration->name(),
                       classInstance.instantiationSite, generic)) {
      return std::nullopt;
    }
    destructorDiscoveryDepths.push_back(
        nextDiscovery(classInstance.instantiationSite));
    discoveredGenericInstances += generic;
    output.program.destructors.push_back(
        {.id = id,
         .sourceUnit = classInstance.sourceUnit,
         .source = lifecycle->declaredDestructor->declaration,
         .owner = owner});
    instanceIndex.recordDestructor(owner, id);
    return id;
  }

  // Declarations are emitted even when their generic owner has no executable
  // instance. Their nondependent signature components still require concrete
  // representation identities; discovering them must not instantiate a
  // dependent owner or any of its function bodies.
  bool seedConcreteDeclarationType(const SemanticType &type) {
    bool concrete =
        type != SemanticType::Unknown &&
        type.kind != SemanticType::TypeParameter &&
        (type.kind != SemanticType::TypePack || type.concretePack) &&
        type.arrayLengthParameterId == 0;
    for (const CompileTimeValue &value : type.valueArguments) {
      concrete = concrete && value.kind == CompileTimeValue::UInt64;
    }
    for (const SemanticType &argument : type.arguments) {
      // Do not short-circuit discovery of a concrete sibling or nested type.
      const bool argumentConcrete = seedConcreteDeclarationType(argument);
      concrete = concrete && argumentConcrete;
    }
    if (concrete) {
      (void)enqueueClass(type);
    }
    return concrete;
  }

  // An alias or an identity-only type argument needs an exact enum identity,
  // but is not itself a request for a concrete class layout or executable
  // lifecycle. Walk its argument graph without using the representation-bearing
  // class discovery path. An enum's actual alternative fields still discover
  // their required class layouts through enqueueEnum.
  bool seedEnumIdentities(const SemanticType &type) {
    bool concrete =
        type != SemanticType::Unknown &&
        type.kind != SemanticType::TypeParameter &&
        (type.kind != SemanticType::TypePack || type.concretePack) &&
        type.arrayLengthParameterId == 0;
    for (const CompileTimeValue &value : type.valueArguments)
      concrete = concrete && value.kind != CompileTimeValue::Parameter;
    for (const SemanticType &argument : type.arguments) {
      const bool argumentConcrete = seedEnumIdentities(argument);
      concrete = concrete && argumentConcrete;
    }
    if (concrete && type.kind == SemanticType::Enum)
      enqueueEnum(type);
    return concrete;
  }

  void enqueueEnum(const SemanticType &type) {
    if (std::any_of(output.program.enums.begin(), output.program.enums.end(),
                    [&](const HirEnum &item) { return item.type == type; })) {
      return;
    }
    std::optional<EnumTypeInfo> info = instanceProvider->enumTypeFor(type);
    // Nongeneric declarations are already concrete semantic descriptors.
    // Lightweight provider clients need no instantiation service for them.
    if (!info) {
      const EnumTypeInfo *declaration = baseModel->findEnumType(type.enumId);
      if (declaration != nullptr && declaration->genericParameters.empty() &&
          type == SemanticType::enumType(declaration->id))
        info = *declaration;
    }
    if (!info) {
      return;
    }
    HirEnum lowered{.declaration = info->id,
                    .type = type,
                    .sourceUnit = info->sourceUnit,
                    .source = info->declaration,
                    .qualifiedName = info->qualifiedName,
                    .underlyingType = info->underlyingType,
                    .traits = instanceProvider->traitsFor(type),
                    .requiresActiveCleanup =
                        instanceProvider->requiresActiveCleanupFor(type),
                    .payload = info->payload};
    lowered.enumerators.reserve(info->enumerators.size());
    for (const EnumeratorInfo &enumerator : info->enumerators) {
      HirEnumerator variant{.source = enumerator.declaration,
                            .value = enumerator.value,
                            .variantIndex = enumerator.variantIndex,
                            .payloadTypes = enumerator.payloadTypes};
      for (const SemanticType &payload : enumerator.payloadTypes) {
        variant.payloadRequiresActiveCleanup.push_back(
            instanceProvider->requiresActiveCleanupFor(payload));
      }
      lowered.enumerators.push_back(std::move(variant));
    }
    // Publish the identity before discovering contained types: finite owning
    // recursion may revisit this enum through a concrete class's fields.
    output.program.enums.push_back(std::move(lowered));
    for (const EnumeratorInfo &enumerator : info->enumerators) {
      for (const SemanticType &payload : enumerator.payloadTypes) {
        (void)enqueueClass(payload);
      }
    }
  }

  struct InstanceCounts {
    std::size_t enums = 0;
    std::size_t symbols = 0;
    std::size_t classes = 0;
    std::size_t functions = 0;
    std::size_t nativeCallbacks = 0;
    std::size_t threadTasks = 0;
    std::size_t constructors = 0;
    std::size_t destructors = 0;
    std::size_t lambdas = 0;

    friend bool operator==(const InstanceCounts &,
                           const InstanceCounts &) = default;
  };

  [[nodiscard]] InstanceCounts instanceCounts() const {
    const HirProgram &program = output.program;
    return {.enums = program.enums.size(),
            .symbols = program.concreteSymbols.size(),
            .classes = program.classes.size(),
            .functions = program.functions.size(),
            .nativeCallbacks = program.nativeCallbacks.size(),
            .threadTasks = program.threadTasks.size(),
            .constructors = program.constructors.size(),
            .destructors = program.destructors.size(),
            .lambdas = program.lambdas.size()};
  }

  // Instance identities are table positions, so truncating every table keeps
  // all identities in the prefix unchanged.
  [[nodiscard]] HirProgram reachablePrefix(const InstanceCounts &counts) const {
    HirProgram prefix = output.program;
    prefix.enums.resize(counts.enums);
    prefix.concreteSymbols.resize(counts.symbols);
    prefix.classes.resize(counts.classes);
    prefix.functions.resize(counts.functions);
    prefix.nativeCallbacks.resize(counts.nativeCallbacks);
    prefix.threadTasks.resize(counts.threadTasks);
    prefix.constructors.resize(counts.constructors);
    prefix.destructors.resize(counts.destructors);
    prefix.lambdas.resize(counts.lambdas);
    return prefix;
  }

  // Program roots are the hosted entry, library exports, and the C boundary:
  // C declarations and records form the native interface that the native
  // header and layout checks publish whether or not GTI uses them. Native
  // code reaches GTI functions only through callback conversions that GTI
  // code performs. Every other executable instance is discovered from these
  // roots by ordinary enqueueing. Enum identities stay complete because
  // type-only uses do not enqueue them.
  void seedProgramRoots(const StmtList &declarations) {
    std::unordered_set<FunctionId> roots;
    if (const auto &entry = baseModel->hostedProgramEntryPlan()) {
      roots.insert(entry->entry);
    }
    if (const auto &embedding = baseModel->embeddingPlan()) {
      for (const SemanticEmbeddingExport &selected : embedding->exports) {
        roots.insert(selected.function);
      }
    }
    seedRootDeclarations(declarations, roots);
  }

  // Complete lowering already has an instance of every non-generic class. A
  // reachable prefix must also discover a class that an instance names only
  // in its type arguments or signature: no value of it may ever be
  // constructed, yet the instance's identity and signature spell it.
  // Complete lowering and constant closures need no class layout for an
  // identity-only type argument, but every enum it names still needs an exact
  // catalog row: declarations seed only non-generic enums, and a constant
  // closure seeds none. Signature types are discovered when the instance is
  // enqueued.
  void seedInstanceSignatureClasses(std::vector<SemanticType> typeArguments,
                                    SemanticType returnType,
                                    std::vector<SemanticType> parameterTypes) {
    // Both complete and reachable lowering retain generic argument identities.
    // Even when no value/signature uses the enum, MIR must have its exact row.
    // Do not instantiate otherwise unused class layouts merely to find enums.
    for (const SemanticType &argument : typeArguments)
      (void)seedEnumIdentities(argument);
    if (!reachablePrefixRequested)
      return;
    for (const SemanticType &argument : typeArguments) {
      seedConcreteDeclarationType(argument);
    }
    seedConcreteDeclarationType(returnType);
    for (const SemanticType &parameter : parameterTypes) {
      seedConcreteDeclarationType(parameter);
    }
  }

  // Template declarations are emitted whether or not they are instantiated,
  // so the concrete classes their signatures, fields, and bases name are
  // seeded exactly as complete seeding does for every declaration.
  void seedTemplateSignature(const SemanticType &returnType,
                             const std::vector<SemanticType> &parameters) {
    seedConcreteDeclarationType(returnType);
    for (const SemanticType &parameter : parameters) {
      seedConcreteDeclarationType(parameter);
    }
  }

  void seedRootDeclarations(const StmtList &declarations,
                            const std::unordered_set<FunctionId> &roots,
                            bool templateContext = false) {
    for (const StmtPtr &statement : declarations) {
      if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          seedRootDeclarations(*branch, roots, templateContext);
        }
      } else if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
        seedRootDeclarations(space->declarations(), roots, templateContext);
      } else if (const auto *externC = stmtAs<ExternCDecl>(statement.get())) {
        seedRootDeclarations(externC->declarations(), roots, templateContext);
      } else if (const auto *alias = stmtAs<TypeAliasDecl>(statement.get())) {
        if (const TypeAliasInfo *info = baseModel->findTypeAlias(*alias))
          seedEnumIdentities(info->type);
      } else if (const auto *classDeclaration =
                     stmtAs<ClassDecl>(statement.get())) {
        const ClassTypeInfo *info = baseModel->findClassType(*classDeclaration);
        const bool emittedAsTemplate =
            templateContext ||
            (info != nullptr && !info->genericParameters.empty());
        if (info != nullptr && !emittedAsTemplate &&
            (info->cAbiRecord || info->cOpaqueHandle)) {
          (void)enqueueClass(SemanticType::classType(info->id));
        }
        if (info != nullptr && emittedAsTemplate) {
          for (const ClassBaseTypeInfo &base : info->bases) {
            seedConcreteDeclarationType(base.type);
          }
          for (const ClassFieldTypeInfo &field : info->fields) {
            seedConcreteDeclarationType(field.type);
          }
          for (const ClassFieldTypeInfo &field : info->staticFields) {
            seedConcreteDeclarationType(field.type);
          }
        }
        seedRootDeclarations(classDeclaration->members(), roots,
                             emittedAsTemplate);
      } else if (const auto *enumDeclaration =
                     stmtAs<EnumDecl>(statement.get())) {
        const EnumTypeInfo *info = baseModel->findEnumType(*enumDeclaration);
        if (info != nullptr && info->genericParameters.empty()) {
          enqueueEnum(SemanticType::enumType(info->id));
        }
      } else if (const auto *constructor =
                     stmtAs<ConstructorDecl>(statement.get())) {
        const ConstructorInfo *info = baseModel->findConstructor(*constructor);
        if (info != nullptr &&
            (templateContext || !info->genericParameters.empty())) {
          seedTemplateSignature(SemanticType::Void, info->parameterTypes);
        }
      } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
        const FunctionInfo *info = baseModel->findFunction(*function);
        if (info == nullptr) {
          continue;
        }
        if (templateContext || !info->genericParameters.empty()) {
          seedTemplateSignature(info->returnType, info->parameterTypes);
          continue;
        }
        if (info->ownerClass != 0 || (!roots.contains(info->id) &&
                                      info->linkage != LanguageLinkage::C)) {
          continue;
        }
        (void)enqueueFunction(*info, {}, {}, {}, {}, info->returnType,
                              info->parameterTypes);
      }
    }
  }

  void seedDeclarations(const StmtList &declarations,
                        std::optional<ClassId> enclosingClass) {
    for (const StmtPtr &statement : declarations) {
      seedDeclaration(statement, enclosingClass);
    }
  }

  void seedDeclaration(const StmtPtr &statement,
                       std::optional<ClassId> enclosingClass) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        seedDeclarations(*branch, enclosingClass);
      }
      return;
    }
    if (const auto *namespaceDeclaration =
            stmtAs<NamespaceDecl>(statement.get())) {
      seedDeclarations(namespaceDeclaration->declarations(), enclosingClass);
      return;
    }
    if (const auto *externC = stmtAs<ExternCDecl>(statement.get())) {
      seedDeclarations(externC->declarations(), enclosingClass);
      return;
    }
    if (const auto *alias = stmtAs<TypeAliasDecl>(statement.get())) {
      if (const TypeAliasInfo *info = baseModel->findTypeAlias(*alias))
        seedEnumIdentities(info->type);
      return;
    }
    if (const auto *classDeclaration = stmtAs<ClassDecl>(statement.get())) {
      const ClassTypeInfo *classInfo =
          baseModel->findClassType(*classDeclaration);
      if (classInfo != nullptr) {
        for (const ClassBaseTypeInfo &base : classInfo->bases) {
          seedConcreteDeclarationType(base.type);
        }
        for (const ClassFieldTypeInfo &field : classInfo->fields) {
          seedConcreteDeclarationType(field.type);
        }
        for (const ClassFieldTypeInfo &field : classInfo->staticFields) {
          seedConcreteDeclarationType(field.type);
        }
      }
      if (classInfo != nullptr && classInfo->genericParameters.empty()) {
        (void)enqueueClass(SemanticType::classType(classInfo->id));
      }
      seedDeclarations(classDeclaration->members(),
                       classInfo == nullptr
                           ? std::optional<ClassId>{}
                           : std::optional<ClassId>{classInfo->id});
      return;
    }
    if (const auto *enumDeclaration = stmtAs<EnumDecl>(statement.get())) {
      const EnumTypeInfo *info = baseModel->findEnumType(*enumDeclaration);
      if (info != nullptr && info->genericParameters.empty()) {
        enqueueEnum(SemanticType::enumType(info->id));
      }
      return;
    }
    if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      const FunctionInfo *info = baseModel->findFunction(*function);
      if (info != nullptr) {
        seedConcreteDeclarationType(info->returnType);
        for (const SemanticType &parameter : info->parameterTypes) {
          seedConcreteDeclarationType(parameter);
        }
      }
      if (info == nullptr || !info->genericParameters.empty()) {
        return;
      }
      std::vector<SemanticType> classArguments;
      if (info->ownerClass != 0) {
        const ClassTypeInfo *owner = baseModel->findClassType(info->ownerClass);
        if (owner == nullptr || !owner->genericParameters.empty()) {
          return;
        }
      }
      (void)enqueueFunction(*info, classArguments, {}, {}, {}, info->returnType,
                            info->parameterTypes);
      return;
    }
    if (const auto *constructor = stmtAs<ConstructorDecl>(statement.get())) {
      if (const ConstructorInfo *info =
              baseModel->findConstructor(*constructor)) {
        for (const SemanticType &parameter : info->parameterTypes) {
          seedConcreteDeclarationType(parameter);
        }
      }
      return;
    }
    // Program-wide storage is lowered once from the semantic initialization
    // plan after declaration seeding. Its order is not the combined AST
    // visitation order, and class statics share the module body with globals.
  }

  void lowerEmbeddingPlan() {
    const std::optional<SemanticEmbeddingPlan> &semanticPlan =
        baseModel->embeddingPlan();
    if (!semanticPlan) {
      output.program.embeddingPlan_.reset();
      return;
    }
    HirEmbeddingPlan plan{.abiVersion = semanticPlan->abiVersion};
    plan.exports.reserve(semanticPlan->exports.size());
    for (const SemanticEmbeddingExport &selected : semanticPlan->exports) {
      const FunctionInfo *function = baseModel->findFunction(selected.function);
      const std::optional<std::size_t> existing =
          function == nullptr
              ? std::nullopt
              : instanceIndex.findFunction(function->id, {}, {}, {}, {});
      if (function == nullptr || !existing) {
        lifecycleValid = false;
        continue;
      }
      plan.exports.push_back({.semanticFunction = selected.function,
                              .function = *existing,
                              .qualifiedName = selected.qualifiedName,
                              .symbol = selected.symbol,
                              .returnType = selected.returnType,
                              .parameterTypes = selected.parameterTypes,
                              .requestSource = selected.requestSource});
    }
    if (plan.exports.size() != semanticPlan->exports.size()) {
      lifecycleValid = false;
    }
    output.program.embeddingPlan_ = std::move(plan);
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
    return enqueueConstructor(
        {.constructor = constructor->id,
         .declaration = constructor->declaration,
         .constructedType = type,
         // Owner arguments belong to constructedType. These constructors
         // have no constructor-local generic arguments, exactly as when
         // ordinary source code selects the same declarations.
         .parameterTypes = constructor->parameterTypes,
         .generatedDefault = constructor->declaration->specifier() &&
                             constructor->declaration->specifier()->kind ==
                                 SpecialMemberSpecifierKind::Defaulted &&
                             constructor->parameterTypes.empty()},
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
    while (!discoveryLimitReached &&
           (processedClasses < output.program.classes.size() ||
            processedFunctions < output.program.functions.size() ||
            processedConstructors < output.program.constructors.size() ||
            processedDestructors < output.program.destructors.size())) {
      while (!discoveryLimitReached &&
             processedClasses < output.program.classes.size()) {
        DiscoveryScope scope(
            discoveryDepth, discoverySite, discoveryOrigin,
            classDiscoveryDepths[processedClasses],
            output.program.classes[processedClasses].instantiationSite);
        processClass(processedClasses++);
      }
      enqueueVirtualContractRoots();
      while (!discoveryLimitReached &&
             processedFunctions < output.program.functions.size()) {
        DiscoveryScope scope(
            discoveryDepth, discoverySite, discoveryOrigin,
            functionDiscoveryDepths[processedFunctions],
            output.program.functions[processedFunctions].instantiationSite);
        processFunction(processedFunctions++);
      }
      while (!discoveryLimitReached &&
             processedConstructors < output.program.constructors.size()) {
        // Function bodies and earlier constructors can discover concrete
        // generic owners. Their field/default catalogs must be complete
        // before the next constructor selects its initialization schedule.
        while (!discoveryLimitReached &&
               processedClasses < output.program.classes.size()) {
          DiscoveryScope scope(
              discoveryDepth, discoverySite, discoveryOrigin,
              classDiscoveryDepths[processedClasses],
              output.program.classes[processedClasses].instantiationSite);
          processClass(processedClasses++);
        }
        if (discoveryLimitReached) {
          break;
        }
        DiscoveryScope scope(discoveryDepth, discoverySite, discoveryOrigin,
                             constructorDiscoveryDepths[processedConstructors],
                             output.program.constructors[processedConstructors]
                                 .instantiationSite);
        processConstructor(processedConstructors++);
      }
      while (!discoveryLimitReached &&
             processedDestructors < output.program.destructors.size()) {
        DiscoveryScope scope(
            discoveryDepth, discoverySite, discoveryOrigin,
            destructorDiscoveryDepths[processedDestructors],
            output.program
                .classes
                    [output.program.destructors[processedDestructors].owner - 1]
                .instantiationSite);
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
      if (const auto *conditional = stmtAs<ConditionalStmt>(member.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          enqueueVirtualMembers(*branch, substitution, classTypeArguments,
                                classValueArguments);
        }
        continue;
      }
      const auto *function = stmtAs<FunctionDecl>(member.get());
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
    // An empty concrete pack is still an instance. Its recorded field and
    // alias bindings must come from concrete reanalysis, not the open primary.
    if (!declaration->genericParameters.empty() ||
        !snapshot.typeArguments.empty() || !snapshot.valueArguments.empty()) {
      analysis = instanceProvider->analyzeClassFieldInitializers(
          snapshot.declaration, snapshot.typeArguments,
          snapshot.valueArguments);
      retainInstanceSymbols(analysis.model);
      appendInstanceDiagnostics(std::move(analysis.diagnostics),
                                snapshot.instantiationSite, discoveryOrigin);
      model = &analysis.model;
    }
    if (const auto *concreteLifecycle =
            model->findClassLifecycle(*declaration->declaration)) {
      auto &instance = output.program.classes[index];
      instance.defaultConstructor = concreteLifecycle->defaultConstructor;
      instance.copyConstructor = concreteLifecycle->copyConstructor;
      instance.moveConstructor = concreteLifecycle->moveConstructor;
      instance.copyAssignment = concreteLifecycle->copyAssignment;
      instance.moveAssignment = concreteLifecycle->moveAssignment;
    }
    std::vector<HirClassTypeAlias> aliases;
    for (const TypeAliasDecl *source : declaration->typeAliases) {
      const TypeAliasInfo *alias = model->findTypeAlias(*source);
      if (alias == nullptr) {
        lifecycleValid = false;
        continue;
      }
      aliases.push_back(
          {.declaration = alias->id, .source = source, .type = alias->type});
      // Naming an alias is not a layout or construction use. Preserve its
      // exact type catalog without discovering executable class bodies;
      // actual fields/calls/signatures enqueue their concrete types below.
      seedEnumIdentities(alias->type);
    }
    output.program.classes[index].typeAliases = std::move(aliases);
    std::vector<HirClassField> fields;
    HirBody fieldInitializers;
    fields.reserve(declaration->fields.size());
    for (const ClassFieldTypeInfo &field : declaration->fields) {
      SemanticType type = substitute(field.type, substitution);
      BindingInfo info{.type = type,
                       .access = field.declaration != nullptr &&
                                         field.declaration->isMutable()
                                     ? AccessMode::Mutable
                                     : AccessMode::ReadOnly,
                       .traits = instanceProvider->traitsFor(type)};
      if (field.declaration != nullptr) {
        if (const BindingInfo *recorded =
                model->findBinding(*field.declaration)) {
          info.symbol = recorded->symbol;
          info.constant = recorded->constant;
          info.type = recorded->type;
          info.traits = recorded->traits;
          type = recorded->type;
        }
      }
      (void)enqueueClass(type);
      const HirBindingId binding =
          field.declaration == nullptr
              ? 0
              : lowerBinding(*field.declaration, info, fieldInitializers);
      const std::optional<HirValueId> initializer =
          field.declaration == nullptr
              ? std::nullopt
              : lowerExpression(
                    model->resolvedFieldInitializer(*field.declaration), *model,
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
      fields.push_back({.declaration = field.declaration,
                        .binding = binding,
                        .initializer = initializer,
                        .info = info,
                        .requiresActiveCleanup =
                            instanceProvider->requiresActiveCleanupFor(type)});
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
                       .traits = instanceProvider->traitsFor(type),
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
           .requiresActiveCleanup =
               instanceProvider->requiresActiveCleanupFor(type)});
    }
    output.program.classes[index].staticFields = std::move(staticFields);
    finalizeLifetimes(staticFieldInitializers, *model, false);
    lowerLoans(*model, staticFieldInitializers);
    output.program.classes[index].staticFieldInitializers =
        std::move(staticFieldInitializers);
    output.program.classes[index].destructor = enqueueDestructor(snapshot.id);
  }

  void appendInstanceDiagnostics(std::vector<Diagnostic> diagnostics,
                                 const std::optional<SourceSpan> &site,
                                 const std::optional<SourceSpan> &origin) {
    for (Diagnostic &diagnostic : diagnostics) {
      const auto request = origin ? origin : site;
      const bool missingDefinition =
          diagnostic.code ==
          diagnosticDescriptor(DiagnosticId::MissingFunctionDefinition).code;
      if (missingDefinition && request && diagnostic.primary != *request) {
        diagnostic.related.push_back(
            {diagnostic.primary,
             "This selected call requires the missing definition."});
        diagnostic.primary = *request;
      }
      if (site && (!missingDefinition || diagnostic.primary != *site)) {
        diagnostic.related.push_back(
            {*site, "Concrete generic instance requested here."});
      }
      output.diagnostics.push_back(std::move(diagnostic));
    }
  }

  void retainInstanceSymbols(const SemanticModel &model) {
    const auto &symbols = model.database().symbols();
    output.program.concreteSymbols.insert(output.program.concreteSymbols.end(),
                                          symbols.begin(), symbols.end());
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
    if (declaration == nullptr || !requirePrimaryBody(*declaration)) {
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
    const ClassTypeInfo *genericOwner =
        declaration->ownerClass == 0
            ? nullptr
            : baseModel->findClassType(declaration->ownerClass);
    const bool concreteInstance =
        !declaration->genericParameters.empty() ||
        (genericOwner != nullptr && !genericOwner->genericParameters.empty()) ||
        !classArguments.empty() || !classValueArguments.empty() ||
        !snapshot.typeArguments.empty() || !snapshot.valueArguments.empty();
    if (!concreteInstance) {
      return nullptr;
    }
    auto analysis = std::make_unique<SemanticInstanceAnalysis>(
        instanceProvider->analyzeFunctionInstance(
            declaration->id, classArguments, classValueArguments,
            snapshot.typeArguments, snapshot.valueArguments));
    retainInstanceSymbols(analysis->model);
    appendInstanceDiagnostics(std::move(analysis->diagnostics),
                              snapshot.instantiationSite,
                              functionDiscoveryDepths[id - 1].origin);
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
    if (snapshot.generatedDefault)
      return nullptr;
    if (snapshot.owner == 0 || snapshot.owner > output.program.classes.size()) {
      return nullptr;
    }
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    auto analysis = std::make_unique<SemanticInstanceAnalysis>(
        instanceProvider->analyzeConstructorInstance(
            snapshot.declaration, owner.typeArguments, owner.valueArguments,
            snapshot.typeArguments, snapshot.valueArguments));
    retainInstanceSymbols(analysis->model);
    appendInstanceDiagnostics(std::move(analysis->diagnostics),
                              snapshot.instantiationSite,
                              constructorDiscoveryDepths[id - 1].origin);
    SemanticInstanceAnalysis *result = analysis.get();
    constructorAnalyses.emplace(id, std::move(analysis));
    return result;
  }

  // Whether `type` names only compiler-trusted classes and enums (see
  // `HirLoweringScope::omitLibraryOnlyBodies`). Mirrors the instance-reuse
  // eligibility of semantic analysis: no lambda, function item, open
  // parameter or symbolic value can appear.
  [[nodiscard]] bool trustedOnlyType(const SemanticType &type) const {
    if (type.genericParameterId != 0 || type.functionId != 0 ||
        type.lambdaId != 0 || type.arrayLengthParameterId != 0)
      return false;
    switch (type.kind) {
    case SemanticType::Unknown:
    case SemanticType::TypeParameter:
    case SemanticType::TypeName:
    case SemanticType::FunctionItem:
    case SemanticType::Lambda:
      return false;
    case SemanticType::TypePack:
      if (!type.concretePack)
        return false;
      break;
    case SemanticType::Class: {
      const ClassTypeInfo *info = baseModel->findClassType(type.classId);
      if (info == nullptr || !trustedUnits.contains(info->sourceUnit))
        return false;
      break;
    }
    case SemanticType::Enum: {
      const EnumTypeInfo *info = baseModel->findEnumType(type.enumId);
      if (info == nullptr || !trustedUnits.contains(info->sourceUnit))
        return false;
      break;
    }
    default:
      break;
    }
    if (!trustedOnlyValues(type.valueArguments))
      return false;
    return std::all_of(type.arguments.begin(), type.arguments.end(),
                       [&](const SemanticType &argument) {
                         return trustedOnlyType(argument);
                       });
  }

  [[nodiscard]] static bool
  trustedOnlyValues(const std::vector<CompileTimeValue> &values) {
    return std::all_of(values.begin(), values.end(), [](const auto &value) {
      return value.kind == CompileTimeValue::UInt64 && !value.expression;
    });
  }

  // Whether an instance of a declaration from `unit` with these arguments is
  // library-only, so its body is omitted under `omitLibraryOnlyBodies`.
  [[nodiscard]] bool
  omitInstanceBody(SourceUnitId unit,
                   const std::vector<SemanticType> &classTypeArguments,
                   const std::vector<CompileTimeValue> &classValueArguments,
                   const std::vector<SemanticType> &typeArguments,
                   const std::vector<CompileTimeValue> &valueArguments) const {
    if (!omitLibraryOnlyBodies || !trustedUnits.contains(unit))
      return false;
    if (!trustedOnlyValues(classValueArguments) ||
        !trustedOnlyValues(valueArguments))
      return false;
    for (const auto *types : {&classTypeArguments, &typeArguments})
      for (const SemanticType &type : *types)
        if (!trustedOnlyType(type))
          return false;
    return true;
  }

  // Concrete discovery needs the selected primary body's facts. Bounded
  // analysis may have deferred that body: the provider completes it in the
  // base model when it can (late demand), and otherwise HIR stays invalid and
  // reports it as a semantic request for a fresh analysis. Late demand only
  // appends records and fills the selected function's own facts; HIR holds
  // base records through node-stable lookups, never across a provider call by
  // vector position.
  bool requirePrimaryBody(const FunctionInfo &function) {
    const FunctionDecl *body = function.bodyDeclaration();
    if (body == nullptr || body->body() == nullptr ||
        baseModel->hasAnalyzedFunctionBody(*body))
      return true;
    if (instanceProvider->analyzeDemandedPrimaryBody(*body) &&
        baseModel->hasAnalyzedFunctionBody(*body))
      return true;
    auto &requests = output.requiredSemanticBodies;
    if (std::find(requests.begin(), requests.end(), body) == requests.end())
      requests.push_back(body);
    return false;
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
    // An omitted body needs no primary facts of its own; a concrete instance
    // analysis below still requires them before it runs.
    const bool omitBody = omitInstanceBody(
        declaration->sourceUnit, classArguments, classValueArguments,
        snapshot.typeArguments, snapshot.valueArguments);
    if (!omitBody && !requirePrimaryBody(*declaration))
      return;
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    const std::string enclosingSourceFunctionName = currentSourceFunctionName;
    currentSourceFunctionName = declaration->qualifiedName;
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
    const ClassTypeInfo *genericOwner =
        declaration->ownerClass == 0
            ? nullptr
            : baseModel->findClassType(declaration->ownerClass);
    const bool concreteInstance =
        !declaration->genericParameters.empty() ||
        (genericOwner != nullptr && !genericOwner->genericParameters.empty()) ||
        !classArguments.empty() || !classValueArguments.empty() ||
        !snapshot.typeArguments.empty() || !snapshot.valueArguments.empty();
    const SemanticModel *model = baseModel;
    if (concreteInstance) {
      SemanticInstanceAnalysis *analysis = ensureFunctionAnalysis(snapshot.id);
      if (analysis == nullptr) {
        lifecycleValid = false;
        currentReceiverType = enclosingReceiverType;
        currentReceiverAccess = enclosingReceiverAccess;
        currentSourceFunctionName = enclosingSourceFunctionName;
        return;
      }
      model = &analysis->model;
    }

    const FunctionInfo *concrete =
        model->findFunction(*declaration->declaration);
    if (concrete != nullptr) {
      output.program.functions[index].returnBorrowOrigin =
          concrete->returnBorrowOrigin;
      output.program.functions[index].returnBorrowParameter =
          concrete->returnBorrowParameter;
      output.program.functions[index].returnBorrowParameters =
          concrete->returnBorrowParameters;
      output.program.functions[index].returnBorrowFields =
          concrete->returnBorrowFields;
      output.program.functions[index].returnBorrowAccess =
          concrete->returnBorrowAccess;
      output.program.functions[index].returnBorrowPlace =
          concrete->returnBorrowPlace;
      output.program.functions[index].ownedPointeeProjection =
          concrete->ownedPointeeProjection;
    }

    if (omitBody) {
      ++output.omittedInstanceBodies;
      currentReceiverType = enclosingReceiverType;
      currentReceiverAccess = enclosingReceiverAccess;
      currentSourceFunctionName = enclosingSourceFunctionName;
      return;
    }

    lambdaTargets.clear();
    seedLambdaTargets(snapshot.typeArguments);
    HirBody body;
    const FunctionDecl *bodyDeclaration = declaration->bodyDeclaration();
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(bodyDeclaration->parameters().size());
    for (const Parameter &parameter : bodyDeclaration->parameters()) {
      parameterBindings.push_back(lowerBinding(parameter, *model, body));
    }
    if (bodyDeclaration->body()) {
      body.roots =
          lowerStatements(bodyDeclaration->body()->statements(), *model,
                          classArguments, classValueArguments, body);
    }
    finalizeLifetimes(body, *model, true);
    lowerLoans(*model, body, bodyDeclaration);
    std::vector<HirCallableParameter> callableParameters;
    callableParameters.reserve(declaration->callableParameters.size());
    for (const CallableParameterContract &parameter :
         declaration->instantiatedCallableParameters(snapshot.parameterTypes)) {
      if (parameter.parameterIndex >= snapshot.parameterTypes.size() ||
          (parameter.boundary == CallableBoundary::Owned &&
           (!parameter.ownedTransport ||
            parameter.ownedTransport->kind !=
                CallableOwnedTransportKind::ThreadTask) &&
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
        if (lowered.ownedTransport->kind == CallableOwnedTransportKind::Value) {
          lowered.ownedTransport->destinationType = lowered.callableType;
        } else if (lowered.ownedTransport->kind !=
                   CallableOwnedTransportKind::ThreadTask) {
          lowered.ownedTransport->destinationType = snapshot.returnType;
        } else if (declaration->ownerClass != 0) {
          const ClassTypeInfo *owner =
              baseModel->findClassType(declaration->ownerClass);
          if (owner != nullptr) {
            lowered.ownedTransport->destinationType = substitute(
                lowered.ownedTransport->destinationType,
                classSubstitution(*owner, classArguments, classValueArguments));
          }
        }
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
          } else if (const ResolvedCallInfo *resolved =
                         model->findCall(*signature.source);
                     resolved != nullptr &&
                     (resolved->nativeCalleeType ||
                      model->typeOf(*signature.source->callee())
                          .hasFunctionItemShape())) {
            concrete.returnType = resolved->returnType;
            concrete.parameterTypes = resolved->parameterTypes;
            concrete.selectedCapability = CallableInvocationCapability::Read;
          }
          const auto value =
              std::find_if(body.values.begin(), body.values.end(),
                           [&](const HirValue &candidate) {
                             return candidate.source == signature.source;
                           });
          if (value != body.values.end()) {
            concrete.functionTarget = value->functionTarget;
            concrete.lambdaTarget = value->lambdaTarget;
            concrete.nativeCalleeType = value->nativeCalleeType;
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
    currentSourceFunctionName = enclosingSourceFunctionName;
  }

  void processConstructor(std::size_t index) {
    const HirConstructorInstance snapshot = output.program.constructors[index];
    const HirClassInstance owner = output.program.classes[snapshot.owner - 1];
    const std::vector<SemanticType> &classArguments = owner.typeArguments;
    const std::vector<CompileTimeValue> &classValueArguments =
        owner.valueArguments;
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    const std::string enclosingSourceFunctionName = currentSourceFunctionName;
    const ClassTypeInfo *ownerInfo =
        baseModel->findClassType(owner.declaration);
    currentSourceFunctionName =
        (ownerInfo == nullptr ? std::string{} : ownerInfo->qualifiedName) +
        "::" +
        (snapshot.generatedDefault ? owner.source->name().lexeme
                                   : snapshot.source->name().lexeme);
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis *analysis =
        snapshot.generatedDefault ? nullptr
                                  : ensureConstructorAnalysis(snapshot.id);
    if (analysis == nullptr && !snapshot.generatedDefault) {
      lifecycleValid = false;
      currentReceiverType = enclosingReceiverType;
      currentReceiverAccess = enclosingReceiverAccess;
      currentSourceFunctionName = enclosingSourceFunctionName;
      return;
    }
    // A generated default constructor's initializers are verified against
    // its owner's fields (verifyHirProgramPlans), so it is always lowered.
    if (!snapshot.generatedDefault && ownerInfo != nullptr &&
        omitInstanceBody(ownerInfo->sourceUnit, classArguments,
                         classValueArguments, snapshot.typeArguments,
                         snapshot.valueArguments)) {
      ++output.omittedInstanceBodies;
      currentReceiverType = enclosingReceiverType;
      currentReceiverAccess = enclosingReceiverAccess;
      currentSourceFunctionName = enclosingSourceFunctionName;
      return;
    }

    lambdaTargets.clear();
    seedLambdaTargets(classArguments);
    seedLambdaTargets(snapshot.typeArguments);
    HirBody body;
    std::vector<HirBindingId> parameterBindings;
    if (!snapshot.generatedDefault) {
      parameterBindings.reserve(snapshot.source->parameters().size());
      for (const Parameter &parameter : snapshot.source->parameters()) {
        parameterBindings.push_back(
            lowerBinding(parameter, analysis->model, body));
      }
    }
    std::vector<HirConstructorInitializer> initializers;
    std::vector<HirValueId> initializerValues;
    bool hasExplicitBase = false;
    if (!snapshot.generatedDefault) {
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
                    targetClassValueArguments, body,
                    sourceLocationAt(initializer.target.name.last()))) {
              lowered.arguments.push_back(*value);
              initializerValues.push_back(*value);
            }
          }
        }
        initializers.push_back(std::move(lowered));
      }
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
          const bool generated =
              lifecycle->defaultConstructor == SpecialMemberStatus::Generated;
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
                      base->type.valueArguments, body,
                      sourceLocationAt(snapshot.generatedDefault
                                           ? owner.source->name()
                                           : snapshot.source->name()))) {
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
    // A source constructor owns the complete selected initialization order.
    // Declaration defaults are expressions, not already-live entry state:
    // select them only for fields not overridden by this constructor, and
    // lower them into the same body as explicit initializers so failure can
    // unwind previously completed fields and constructor parameters.
    SemanticInstanceAnalysis fieldAnalysis;
    const SemanticModel *fieldModel = baseModel;
    if (!classArguments.empty() || !classValueArguments.empty()) {
      fieldAnalysis = instanceProvider->analyzeClassFieldInitializers(
          owner.declaration, classArguments, classValueArguments);
      retainInstanceSymbols(fieldAnalysis.model);
      // processPendingInstances completes this owner through processClass
      // before any constructor. That class-owned pass already publishes these
      // substitution diagnostics. Reanalysis supplies constructor-local field
      // expression identities only; publishing again would repeat the same
      // class error for every constructor. Constructor-body diagnostics remain
      // owned by ensureConstructorAnalysis above.
      fieldModel = &fieldAnalysis.model;
    }
    for (const HirClassField &field : owner.fields) {
      if (field.declaration == nullptr ||
          !fieldModel->resolvedFieldInitializer(*field.declaration) ||
          std::any_of(initializers.begin(), initializers.end(),
                      [&](const HirConstructorInitializer &initializer) {
                        return initializer.kind ==
                                   ConstructorInitializerTargetKind::Field &&
                               initializer.field == field.info.symbol;
                      }))
        continue;
      const auto value = lowerExpression(
          fieldModel->resolvedFieldInitializer(*field.declaration), *fieldModel,
          classArguments, classValueArguments, body);
      if (!value) {
        lifecycleValid = false;
        continue;
      }
      initializers.push_back({.targetType = field.info.type,
                              .field = field.info.symbol,
                              .arguments = {*value},
                              .explicitArgumentCount = 1});
    }
    const auto fieldOrder = [&](const HirConstructorInitializer &initializer) {
      if (initializer.kind == ConstructorInitializerTargetKind::Base)
        return std::size_t{0};
      const auto found =
          std::find_if(owner.fields.begin(), owner.fields.end(),
                       [&](const HirClassField &field) {
                         return field.info.symbol == initializer.field;
                       });
      return std::size_t(found - owner.fields.begin()) + 1;
    };
    std::stable_sort(initializers.begin(), initializers.end(),
                     [&](const auto &left, const auto &right) {
                       return fieldOrder(left) < fieldOrder(right);
                     });
    initializerValues.clear();
    for (const auto &initializer : initializers)
      initializerValues.insert(initializerValues.end(),
                               initializer.arguments.begin(),
                               initializer.arguments.end());
    const SemanticModel &bodyModel =
        analysis == nullptr ? *fieldModel : analysis->model;
    if (!snapshot.generatedDefault)
      body.roots =
          lowerStatements(snapshot.source->body()->statements(), bodyModel,
                          classArguments, classValueArguments, body);
    finalizeLifetimes(body, bodyModel, true, initializers);
    lowerLoans(bodyModel, body);
    output.program.constructors[index].initializerValues =
        std::move(initializerValues);
    output.program.constructors[index].parameterBindings =
        std::move(parameterBindings);
    output.program.constructors[index].initializers = std::move(initializers);
    output.program.constructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
    currentSourceFunctionName = enclosingSourceFunctionName;
  }

  void processDestructor(std::size_t index) {
    const HirDestructorInstance snapshot = output.program.destructors[index];
    const HirClassInstance &owner = output.program.classes[snapshot.owner - 1];
    const SemanticType enclosingReceiverType = currentReceiverType;
    const AccessMode enclosingReceiverAccess = currentReceiverAccess;
    const std::string enclosingSourceFunctionName = currentSourceFunctionName;
    const ClassTypeInfo *ownerInfo =
        baseModel->findClassType(owner.declaration);
    currentSourceFunctionName =
        (ownerInfo == nullptr ? std::string{} : ownerInfo->qualifiedName) +
        "::~" + snapshot.source->name().lexeme;
    currentReceiverType = owner.type;
    currentReceiverAccess = AccessMode::Mutable;
    SemanticInstanceAnalysis analysis;
    const SemanticModel *model = baseModel;
    if (!owner.typeArguments.empty() || !owner.valueArguments.empty()) {
      analysis = instanceProvider->analyzeDestructorInstance(
          owner.declaration, owner.typeArguments, owner.valueArguments);
      retainInstanceSymbols(analysis.model);
      appendInstanceDiagnostics(std::move(analysis.diagnostics), std::nullopt,
                                discoveryOrigin);
      model = &analysis.model;
    }
    if (ownerInfo != nullptr &&
        omitInstanceBody(ownerInfo->sourceUnit, owner.typeArguments,
                         owner.valueArguments, {}, {})) {
      ++output.omittedInstanceBodies;
      currentReceiverType = enclosingReceiverType;
      currentReceiverAccess = enclosingReceiverAccess;
      currentSourceFunctionName = enclosingSourceFunctionName;
      return;
    }

    lambdaTargets.clear();
    seedLambdaTargets(owner.typeArguments);
    HirBody body;
    body.roots =
        lowerStatements(snapshot.source->body()->statements(), *model,
                        owner.typeArguments, owner.valueArguments, body);
    finalizeLifetimes(body, *model, true);
    lowerLoans(*model, body, snapshot.source);
    output.program.destructors[index].body = std::move(body);
    currentReceiverType = enclosingReceiverType;
    currentReceiverAccess = enclosingReceiverAccess;
    currentSourceFunctionName = enclosingSourceFunctionName;
  }

  [[nodiscard]] HirValueId lowerImplicitReceiver(HirBody &body) {
    const HirValueId id = nextValueId++;
    body.values.push_back(
        {.id = id,
         .kind = HirValueKind::This,
         .info = {.type = currentReceiverType,
                  .category = ValueCategory::Place,
                  .access = currentReceiverAccess,
                  .traits = instanceProvider->traitsFor(currentReceiverType)}});
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
      (void)enqueueClass(info->type, tokenSpan(parameter.sourceAnchor()));
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

  void lowerLoans(const SemanticModel &model, HirBody &body,
                  const Stmt *declaration = nullptr) {
    ensurePlaceDomain(body);
    body.loans.clear();
    std::unordered_map<SymbolId, HirBindingId> bindings;
    for (const HirBinding &binding : body.bindings) {
      if (binding.info.symbol != 0) {
        bindings.insert_or_assign(binding.info.symbol, binding.id);
      }
    }
    std::unordered_set<SemanticLoanId> included;
    std::vector<SemanticLoanId> pending;
    for (const SemanticLoanInfo &loan : model.loans()) {
      if ((declaration != nullptr && loan.entryDeclaration == declaration) ||
          std::any_of(
              loan.carriers.begin(), loan.carriers.end(),
              [&](SymbolId carrier) { return bindings.contains(carrier); }))
        pending.push_back(loan.id);
    }
    while (!pending.empty()) {
      const SemanticLoanId id = pending.back();
      pending.pop_back();
      const auto *loan = model.findLoan(id);
      if (loan == nullptr || !included.insert(id).second)
        continue;
      if (loan->parent != 0)
        pending.push_back(loan->parent);
      if (loan->storedDependency != 0)
        pending.push_back(loan->storedDependency);
    }
    for (const SemanticLoanInfo &loan : model.loans()) {
      if (!included.contains(loan.id))
        continue;
      HirLoan lowered{.semanticLoan = loan.id,
                      .kind = loan.kind,
                      .parent = loan.parent,
                      .storedDependency = loan.storedDependency,
                      .place = qualifyPlace(loan.place, body.placeDomain),
                      .access = loan.access,
                      .designatedFields = loan.designatedFields,
                      .additionalDependencies = loan.additionalDependencies,
                      .lifecycleBound = loan.lifecycleBound,
                      .entry = loan.entry};
      for (auto &dependency : lowered.additionalDependencies) {
        dependency.place = qualifyPlace(dependency.place, body.placeDomain);
      }
      for (const SymbolId carrier : loan.carriers) {
        const auto found = bindings.find(carrier);
        if (found != bindings.end() &&
            std::find(lowered.carriers.begin(), lowered.carriers.end(),
                      found->second) == lowered.carriers.end()) {
          lowered.carriers.push_back(found->second);
        }
      }
      body.loans.push_back(std::move(lowered));
    }
  }

  [[nodiscard]] HirDropType dropTypeFor(const SemanticType &type) {
    HirDropType result{.type = type,
                       .requiresActiveCleanup =
                           instanceProvider->requiresActiveCleanupFor(type)};
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

  [[nodiscard]] static bool fixedClassArrayType(const SemanticType &type) {
    return type.kind == SemanticType::Array && type.arguments.size() == 1 &&
           type.arguments.front().kind == SemanticType::Class &&
           type.arrayLengthParameterId == 0 && type.arrayLength != 0;
  }

  // A fixed-array chain of exact concrete extents whose leaf element is a
  // class: T[N], T[N][M], and deeper. Every level must carry an exact
  // non-zero extent.
  [[nodiscard]] static bool fixedClassLeafArrayType(const SemanticType &type) {
    const SemanticType *level = &type;
    while (level->kind == SemanticType::Array && level->arguments.size() == 1 &&
           level->arrayLengthParameterId == 0 && level->arrayLength != 0) {
      level = &level->arguments.front();
    }
    return level != &type && level->kind == SemanticType::Class;
  }

  [[nodiscard]] bool
  materializesFixedArrayOwnership(const HirValue &value,
                                  const HirBody &body) const {
    if (value.info.category != ValueCategory::Value ||
        value.info.traits.drop != DropKind::Lexical) {
      return false;
    }
    if (value.kind == HirValueKind::ArrayInitializer &&
        fixedClassLeafArrayType(value.info.type) &&
        value.operands.size() == value.info.type.arrayLength) {
      return true;
    }
    if (value.info.type.kind != SemanticType::Class) {
      return false;
    }
    std::size_t consumers = 0;
    const HirValue *consumer = nullptr;
    for (const HirValue &candidate : body.values) {
      for (const HirValueId operand : candidate.operands) {
        if (operand == value.id) {
          ++consumers;
          consumer = &candidate;
        }
      }
    }
    return consumers == 1 && consumer != nullptr &&
           consumer->kind == HirValueKind::ArrayInitializer &&
           fixedClassArrayType(consumer->info.type) &&
           consumer->info.type.arguments.front() == value.info.type &&
           consumer->operands.size() == consumer->info.type.arrayLength;
  }

  [[nodiscard]] bool materializesDropValue(const HirValue &value,
                                           const HirBody &body) const {
    if (value.retainedReceiverProjection) {
      return false;
    }
    if (value.info.category != ValueCategory::Value ||
        value.info.type == SemanticType::Unknown ||
        value.info.type.kind == SemanticType::Reference ||
        value.info.traits.drop != DropKind::Lexical) {
      return false;
    }
    if (!instanceProvider->requiresActiveCleanupFor(value.info.type) &&
        !materializesFixedArrayOwnership(value, body) &&
        !collectionResults.contains(value.id)) {
      return false;
    }
    switch (value.kind) {
    case HirValueKind::ArrayInitializer:
    case HirValueKind::Binary:
    case HirValueKind::Call:
    case HirValueKind::Conditional:
    case HirValueKind::Move:
    case HirValueKind::NativeCallback:
    case HirValueKind::FunctionItem:
    case HirValueKind::Conversion:
    case HirValueKind::DirectInitializer:
    case HirValueKind::ExpectedPropagation:
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
    case HirValueKind::PlaceSet:
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

  // Lookups over one completed body's values while its full expressions are
  // assigned. Scanning the body for each value made finalization quadratic in
  // the body's length. Each lookup returns what the scan it replaces found.
  struct FullExpressionLookup {
    explicit FullExpressionLookup(const HirBody &body) {
      for (std::size_t index = 0; index < body.values.size(); ++index) {
        // The first value with an identity, as HirBody::findValue finds it.
        positions.try_emplace(body.values[index].id, index);
        // The last value with a source, as HirBody::valueForSource finds it.
        if (body.values[index].source != nullptr) {
          sourceValues[body.values[index].source] = index;
        }
      }
      for (const HirDirectInitialization &alias : body.directInitializations) {
        aliases[alias.source] = alias.value;
      }
      for (const HirBody::SourceAlias &alias : body.sourceAliases) {
        aliases[alias.source] = alias.value;
      }
    }

    [[nodiscard]] HirValue *find(HirBody &body, HirValueId id) const {
      const auto found = positions.find(id);
      return found == positions.end() ? nullptr : &body.values[found->second];
    }

    [[nodiscard]] const HirValue *forSource(const HirBody &body,
                                            const Expr &source) const {
      if (const auto found = sourceValues.find(&source);
          found != sourceValues.end()) {
        return &body.values[found->second];
      }
      const auto alias = aliases.find(&source);
      if (alias == aliases.end()) {
        return nullptr;
      }
      const auto found = positions.find(alias->second);
      return found == positions.end() ? nullptr : &body.values[found->second];
    }

    std::unordered_map<HirValueId, std::size_t> positions;
    std::unordered_map<const Expr *, std::size_t> sourceValues;
    std::unordered_map<const Expr *, HirValueId> aliases;
  };

  void assignFullExpression(HirBody &body, const FullExpressionLookup &lookup,
                            HirValueId valueId,
                            HirFullExpressionId fullExpression,
                            std::unordered_set<HirValueId> &visited) {
    if (valueId == 0 || !visited.insert(valueId).second) {
      return;
    }
    HirValue *const found = lookup.find(body, valueId);
    if (found == nullptr) {
      return;
    }
    if (found->fullExpression != 0 && found->fullExpression != fullExpression) {
      lifecycleValid = false;
      return;
    }
    found->fullExpression = fullExpression;
    for (const HirValueId operand : found->operands) {
      assignFullExpression(body, lookup, operand, fullExpression, visited);
    }
    for (const HirValueId input : found->conditionalInputs) {
      assignFullExpression(body, lookup, input, fullExpression, visited);
    }
    for (const HirValueId input : found->sequencedInputs) {
      assignFullExpression(body, lookup, input, fullExpression, visited);
    }
    if (found->receiver) {
      assignFullExpression(body, lookup, *found->receiver, fullExpression,
                           visited);
    }
  }

  void appendFullExpression(HirBody &body, const FullExpressionLookup &lookup,
                            std::vector<HirValueId> roots,
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
      assignFullExpression(body, lookup, root, id, visited);
    }
  }

  [[nodiscard]] std::vector<HirValueId>
  mapFullExpressionRoots(const HirBody &body,
                         const FullExpressionLookup &lookup,
                         const SemanticFullExpression &expression) {
    std::vector<HirValueId> roots;
    roots.reserve(expression.roots.size());
    for (const Expr *source : expression.roots) {
      const HirValue *found =
          source == nullptr ? nullptr : lookup.forSource(body, *source);
      if (found == nullptr) {
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
      if (value.kind == HirValueKind::Call && value.functionTarget) {
        const HirFunctionInstance *callee =
            output.program.findFunctionInstance(*value.functionTarget);
        if (callee != nullptr) {
          value.neverReturns = callee->neverReturns;
          value.tracksCaller = callee->tracksCaller;
        }
      }
      if (value.tracksCaller) {
        const CallerLocationOrigin *origin =
            value.source == nullptr ? nullptr
                                    : model.findCallerLocation(*value.source);
        if (origin == nullptr) {
          lifecycleValid = false;
        } else {
          value.callerLocation = *origin;
        }
      }
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
          pendingFullExpressions.push_back(
              {.constructorInitializer = index + 1});
        }
        continue;
      }
      const auto &expressions = model.fullExpressionsFor(*initializer.source);
      // An explicit initializer without source arguments, such as `: Base()`,
      // has no semantic full expression, yet the constructor's omitted
      // default arguments still evaluate here. Like an implicit base, they
      // form one full expression so owning defaults receive prepared
      // parameter obligations.
      if (expressions.empty()) {
        if (!initializer.arguments.empty()) {
          pendingFullExpressions.push_back(
              {.constructorInitializer = index + 1});
        }
        continue;
      }
      for (const SemanticFullExpression &expression : expressions) {
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
    std::stable_sort(
        pendingFullExpressions.begin(), pendingFullExpressions.end(),
        [](const PendingFullExpression &left,
           const PendingFullExpression &right) {
          if (left.constructorInitializer != 0 ||
              right.constructorInitializer != 0) {
            if (left.constructorInitializer == 0)
              return false;
            if (right.constructorInitializer == 0)
              return true;
            return left.constructorInitializer < right.constructorInitializer;
          }
          return left.expression->order < right.expression->order;
        });
    const FullExpressionLookup lookup(body);
    for (const PendingFullExpression &pending : pendingFullExpressions) {
      const std::vector<HirValueId> roots =
          pending.constructorInitializer != 0 &&
                  pending.constructorInitializer <= initializers.size()
              ? initializers[pending.constructorInitializer - 1].arguments
              : mapFullExpressionRoots(body, lookup, *pending.expression);
      appendFullExpression(body, lookup, roots, pending.statement,
                           pending.constructorInitializer);
    }

    if (lexicalBindings) {
      for (HirBinding &binding : body.bindings) {
        if (binding.parameter != nullptr &&
            binding.info.type.kind == SemanticType::TypePack &&
            binding.info.type.concretePack) {
          for (std::size_t element = 0;
               element < binding.info.type.arguments.size(); ++element) {
            const SemanticType &type = binding.info.type.arguments[element];
            if (instanceProvider->traitsFor(type).drop != DropKind::Lexical)
              continue;
            const HirDropObligationId id = body.dropObligations.size() + 1;
            body.dropObligations.push_back(
                {.id = id,
                 .constructionOrder = id,
                 .kind = HirDropObligationKind::Binding,
                 .binding = binding.id,
                 .packElement = element,
                 .dropType = dropTypeFor(type),
                 .initiallyActive = true});
          }
          continue;
        }
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

    std::unordered_set<HirValueId> temporaryBorrowValues;
    std::unordered_set<HirValueId> borrowedMoveValues;
    for (const HirValue &call : body.values) {
      // An eagerly retained closure needs its own full-expression identity,
      // even when its class captures have only generated passive cleanup.
      for (const HirValueId input : call.conditionalInputs) {
        const HirValue *eager = body.findValue(input);
        if (eager != nullptr && eager->kind == HirValueKind::Lambda &&
            eager->info.traits.drop == DropKind::Lexical)
          temporaryBorrowValues.insert(input);
      }
      if (!call.callPlan) {
        continue;
      }
      if (call.callPlan->receiver) {
        const HirCallReceiver &receiver = *call.callPlan->receiver;
        const HirValue *source = body.findValue(receiver.value);
        if (receiver.kind == HirCallInputKind::ReadTemporaryBorrow ||
            receiver.kind == HirCallInputKind::MutableTemporaryBorrow ||
            (receiver.kind == HirCallInputKind::Value && source != nullptr &&
             source->info.type.kind == SemanticType::Class &&
             source->info.category == ValueCategory::Value &&
             source->info.traits.containsBorrowedState)) {
          temporaryBorrowValues.insert(receiver.value);
        }
      }
      for (const HirCallArgument &argument : call.callPlan->arguments) {
        if (argument.kind == HirCallInputKind::ReadTemporaryBorrow) {
          temporaryBorrowValues.insert(argument.value);
        } else if (argument.kind == HirCallInputKind::MoveBorrowedTemporary) {
          borrowedMoveValues.insert(argument.value);
        }
      }
    }
    for (HirValue &value : body.values) {
      if (value.fullExpression == 0 ||
          (!materializesDropValue(value, body) &&
           !temporaryBorrowValues.contains(value.id) &&
           !borrowedMoveValues.contains(value.id))) {
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
      std::unordered_set<SemanticLoanId> visiting;
      const auto visit = [&](const auto &self,
                             SemanticLoanId current) -> std::size_t {
        const SemanticLoanInfo *loan = model.findLoan(current);
        if (loan == nullptr || !visiting.insert(current).second) {
          return 0;
        }
        const auto result = 1 + std::max(self(self, loan->parent),
                                         self(self, loan->storedDependency));
        visiting.erase(current);
        return result;
      };
      return visit(visit, id);
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

  [[nodiscard]] std::optional<HirValueId> lowerReturnExpression(
      const ExprPtr &expression, const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    const std::optional<HirValueId> source = lowerExpression(
        expression, model, classArguments, classValueArguments, body);
    if (!source || !expression) {
      return source;
    }
    const ImplicitReturnMoveInfo *implicitMove =
        model.findImplicitReturnMove(*expression);
    if (implicitMove == nullptr) {
      return source;
    }

    const auto found = std::find_if(
        body.values.begin(), body.values.end(),
        [&](const HirValue &value) { return value.id == *source; });
    if (found == body.values.end() || !found->ownership ||
        found->ownership->kind != OwnershipEventKind::Move) {
      lifecycleValid = false;
      return source;
    }

    ExpressionInfo info = found->info;
    info.category = ValueCategory::Value;
    info.access = AccessMode::ReadOnly;
    const OwnershipEvent ownership = *found->ownership;
    found->ownership.reset();

    HirValue move{.id = nextValueId++,
                  .kind = HirValueKind::Move,
                  .source = expression.get(),
                  .info = std::move(info),
                  .operands = {*source},
                  .intrinsic = IntrinsicKind::Move,
                  .borrowOrigin = implicitMove->borrowOrigin,
                  .borrowArgument = 0,
                  .borrowAccess = implicitMove->borrowAccess,
                  .place = ownership.place,
                  .ownership = ownership};
    const HirValueId moveId = move.id;
    body.values.push_back(std::move(move));
    output.program.sourceValueIds[expression.get()].push_back(moveId);
    return moveId;
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
    if (const auto *block = stmtAs<BlockStmt>(statement)) {
      return appendStatement({.kind = HirStatementKind::Block,
                              .source = statement,
                              .unsafeBlock = block->isUnsafe(),
                              .statements = lowerStatements(
                                  block->statements(), model, classArguments,
                                  classValueArguments, body)},
                             body);
    }
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
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
    if (stmtAs<CompileErrorDirective>(statement) != nullptr) {
      return std::nullopt;
    }
    if (stmtAs<StaticAssertDecl>(statement) != nullptr) {
      return std::nullopt;
    }
    if (const auto *expression = stmtAs<ExpressionStmt>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Expression,
           .source = statement,
           .value = lowerExpression(expression->expression(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *forStatement = stmtAs<ForStmt>(statement)) {
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
    if (const auto *rangeFor = stmtAs<RangeForStmt>(statement)) {
      const std::optional<HirStatementId> loweredBody =
          lowerStatement(rangeFor->lowered().get(), model, classArguments,
                         classValueArguments, body);
      const ResolvedRangeIterationInfo *resolved =
          model.findRangeIteration(*rangeFor);
      const auto bindingFor = [&](const VariableDecl *declaration) {
        if (declaration == nullptr)
          return HirBindingId{0};
        const auto found =
            std::find_if(body.bindings.begin(), body.bindings.end(),
                         [&](const HirBinding &binding) {
                           return binding.variable == declaration;
                         });
        return found == body.bindings.end() ? HirBindingId{0} : found->id;
      };
      const auto valueFor = [&](const ExprPtr &expression) {
        if (!expression) {
          return HirValueId{0};
        }
        const HirValue *found = body.valueForSource(*expression);
        return found == nullptr ? HirValueId{0} : found->id;
      };
      const auto statementFor = [&](const Stmt *source) {
        if (source == nullptr)
          return HirStatementId{0};
        const auto found =
            std::find_if(body.statements.begin(), body.statements.end(),
                         [&](const HirStatement &candidate) {
                           return candidate.source == source;
                         });
        return found == body.statements.end() ? HirStatementId{0} : found->id;
      };
      HirRangeIteration range;
      if (resolved != nullptr) {
        range.strategy = resolved->strategy;
        range.rangeType = resolved->rangeType;
        range.iteratorType = resolved->iteratorType;
        range.sentinelType = resolved->sentinelType;
        range.elementType = resolved->elementType;
        range.elementAccess = resolved->elementAccess;
        range.rangeLoan = resolved->rangeLoan;
        range.elementLoan = resolved->elementLoan;
        range.elementOwnerLoan = resolved->elementOwnerLoan;
        range.beginFunction = resolved->beginFunction;
        range.endFunction = resolved->endFunction;
        range.compareFunction = resolved->compareFunction;
        range.dereferenceFunction = resolved->dereferenceFunction;
        range.incrementFunction = resolved->incrementFunction;
        range.arrayLength = resolved->arrayLength;
        range.ownsRangeTemporary = resolved->ownsRangeTemporary;
        range.expectedPayloadProjection = resolved->expectedPayloadProjection;
        range.nonBorrowingEndpoint = resolved->nonBorrowingEndpoint;
        range.materializesElementValue = resolved->materializesElementValue;
      }
      range.owner = bindingFor(rangeFor->ownerBinding());
      range.rangeCarrier = bindingFor(rangeFor->rangeBinding());
      range.iterator = bindingFor(rangeFor->iteratorBinding());
      range.sentinel = bindingFor(rangeFor->sentinelBinding());
      range.element = bindingFor(rangeFor->elementBinding());
      range.elementOwner = bindingFor(rangeFor->elementOwnerBinding());
      range.elementOwnerStatement =
          statementFor(rangeFor->elementOwnerBinding());
      range.source = rangeFor->ownerBinding() == nullptr
                         ? 0
                         : valueFor(rangeFor->ownerBinding()->initializer());
      range.begin = rangeFor->iteratorBinding() == nullptr
                        ? 0
                        : valueFor(rangeFor->iteratorBinding()->initializer());
      range.end = rangeFor->sentinelBinding() == nullptr
                      ? 0
                      : valueFor(rangeFor->sentinelBinding()->initializer());
      const auto *accessBinding = rangeFor->elementOwnerBinding() == nullptr
                                      ? rangeFor->elementBinding()
                                      : rangeFor->elementOwnerBinding();
      range.elementAccessValue =
          accessBinding == nullptr ? 0 : valueFor(accessBinding->initializer());
      range.coreLoop = statementFor(rangeFor->coreLoop());
      if (rangeFor->coreLoop() != nullptr) {
        range.comparison = valueFor(rangeFor->coreLoop()->condition());
        range.increment = valueFor(rangeFor->coreLoop()->increment());
      }
      return appendStatement({.kind = HirStatementKind::RangeFor,
                              .source = statement,
                              .body = loweredBody,
                              .rangeIteration = std::move(range)},
                             body);
    }
    if (const auto *loop = stmtAs<ConstexprForStmt>(statement)) {
      // Each expanded element is an ordinary block; the sequence expression
      // itself is compile-time only and never executes.
      std::vector<HirStatementId> statements;
      if (const ConstexprForExpansion *expansion =
              model.findConstexprForExpansion(*loop)) {
        for (const ConstexprForIteration &iteration : expansion->iterations) {
          if (const std::optional<HirStatementId> lowered =
                  lowerStatement(iteration.block.get(), model, classArguments,
                                 classValueArguments, body)) {
            statements.push_back(*lowered);
          }
        }
      } else {
        // Deferred in the symbolic primary; concrete reanalysis expands it.
        lifecycleValid = false;
      }
      return appendStatement({.kind = HirStatementKind::CompileTimeBranch,
                              .source = statement,
                              .statements = std::move(statements)},
                             body);
    }
    if (const auto *ifStatement = stmtAs<IfStmt>(statement)) {
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
        } else {
          // A symbolic primary is not an executable body. Concrete semantic
          // reanalysis must choose the branch before HIR can publish it.
          lifecycleValid = false;
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
    if (const auto *loopControl = stmtAs<LoopControlStmt>(statement)) {
      return appendStatement(
          {.kind = loopControl->keyword().kind == TokenKind::BREAK
                       ? HirStatementKind::Break
                       : HirStatementKind::Continue,
           .source = statement},
          body);
    }
    if (const auto *returnStatement = stmtAs<ReturnStmt>(statement)) {
      return appendStatement({.kind = HirStatementKind::Return,
                              .source = statement,
                              .value = lowerReturnExpression(
                                  returnStatement->value(), model,
                                  classArguments, classValueArguments, body)},
                             body);
    }
    if (const auto *switchStatement = stmtAs<SwitchStmt>(statement)) {
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
              const bool borrowed =
                  payload.binding.type.kind == SemanticType::Reference;
              const SemanticType extractedType =
                  borrowed ? payload.binding.type.arguments.front()
                           : payload.binding.type;
              body.values.push_back(
                  {.id = extraction,
                   .kind = HirValueKind::PayloadExtraction,
                   .source = payload.source,
                   .info = {.type = extractedType,
                            .category = borrowed ? ValueCategory::Place
                                                 : ValueCategory::Value,
                            .access = borrowed
                                          ? payload.binding.type.referenceAccess
                                          : AccessMode::ReadOnly,
                            .traits =
                                instanceProvider->traitsFor(extractedType)},
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
    if (const auto *structured = stmtAs<StructuredBindingDecl>(statement)) {
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
    if (const auto *variable = stmtAs<VariableDecl>(statement)) {
      return appendStatement(
          {.kind = HirStatementKind::Variable,
           .source = statement,
           .binding = lowerBinding(*variable, model, body),
           .value = lowerExpression(variable->initializer(), model,
                                    classArguments, classValueArguments, body)},
          body);
    }
    if (const auto *doWhile = stmtAs<DoWhileStmt>(statement)) {
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
    if (const auto *whileStatement = stmtAs<WhileStmt>(statement)) {
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
    if (stmtAs<EmptyStmt>(statement) != nullptr) {
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
    if (const auto *member = exprAs<Get>(callee.get())) {
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
    if (const auto *member = exprAs<Get>(callee.get())) {
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
    const std::string enclosingSourceFunctionName = currentSourceFunctionName;
    currentSourceFunctionName =
        enclosingSourceFunctionName.empty()
            ? "<lambda>"
            : enclosingSourceFunctionName + "::<lambda>";
    std::vector<HirBindingId> parameterBindings;
    parameterBindings.reserve(lambda.parameters().size());
    for (const Parameter &parameter : lambda.parameters()) {
      parameterBindings.push_back(lowerBinding(parameter, model, body));
    }
    body.roots = lowerStatements(lambda.body(), model, classArguments,
                                 classValueArguments, body);
    currentSourceFunctionName = enclosingSourceFunctionName;
    finalizeLifetimes(body, model, true);
    lowerLoans(model, body);
    std::vector<bool> captureRequiresActiveCleanup;
    captureRequiresActiveCleanup.reserve(info->captures.size());
    for (const LambdaCaptureInfo &capture : info->captures) {
      captureRequiresActiveCleanup.push_back(
          instanceProvider->requiresActiveCleanupFor(capture.type));
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

  [[nodiscard]] bool orderedValueParameterType(const SemanticType &type) const {
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
      return true;
    case SemanticType::Array: {
      // A concrete, cleanup-free array is an ordinary by-value snapshot, not
      // a decayed pointer or a borrowed pack element. Owning/borrow-carrying
      // arrays still require their explicit lifetime transport representation.
      const SemanticType *leaf = &type;
      while (leaf->kind == SemanticType::Array) {
        if (leaf->arguments.size() != 1 || leaf->arrayLength == 0 ||
            leaf->arrayLengthParameterId != 0)
          return false;
        leaf = &leaf->arguments.front();
      }
      const auto traits = instanceProvider->traitsFor(type);
      return traits.copyable && !traits.containsBorrowedState &&
             !instanceProvider->requiresActiveCleanupFor(type);
    }
    case SemanticType::NativeFunction:
      // A named native callback value is a passive pointer, not a confined
      // GTI callable. Packs retain its exact native signature by value.
      return type.hasNativeFunctionShape();
    case SemanticType::FunctionItem:
      // Exact namespace identities have no environment to transfer or drop.
      return type.hasFunctionItemShape();
    case SemanticType::Enum:
      return !instanceProvider->requiresActiveCleanupFor(type);
    default:
      return false;
    }
  }

  [[nodiscard]] bool
  orderedOwningParameterType(const SemanticType &type) const {
    return type.kind == SemanticType::Class ||
           type.kind == SemanticType::Lambda ||
           (type.kind == SemanticType::Enum &&
            instanceProvider->requiresActiveCleanupFor(type));
  }

  [[nodiscard]] std::optional<std::vector<HirCallArgument>> orderedArguments(
      const std::vector<HirValueId> &arguments,
      const std::vector<SemanticType> &parameterTypes, const HirBody &body,
      std::size_t explicitArgumentCount =
          std::numeric_limits<std::size_t>::max(),
      std::span<const std::size_t> ephemeralBorrowArguments = {}) const {
    if (arguments.size() != parameterTypes.size()) {
      return std::nullopt;
    }
    const std::unordered_set<std::size_t> ephemeralBorrowSet(
        ephemeralBorrowArguments.begin(), ephemeralBorrowArguments.end());
    if (ephemeralBorrowSet.size() != ephemeralBorrowArguments.size() ||
        std::any_of(ephemeralBorrowSet.begin(), ephemeralBorrowSet.end(),
                    [&](std::size_t index) {
                      return index >= arguments.size() ||
                             index >= explicitArgumentCount;
                    })) {
      return std::nullopt;
    }

    std::vector<HirCallArgument> result;
    result.reserve(arguments.size());
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      HirValueId source = arguments[index];
      const HirValue *input = body.findValue(source);
      const SemanticType &parameter = parameterTypes[index];
      if (input == nullptr || input->kind == HirValueKind::PackExpansion) {
        return std::nullopt;
      }

      HirCallInputKind kind = HirCallInputKind::Value;
      if (ephemeralBorrowSet.contains(index)) {
        while (input != nullptr && input->kind == HirValueKind::Grouping &&
               input->operands.size() == 1) {
          source = input->operands.front();
          input = body.findValue(source);
        }
        if (input == nullptr || parameter.kind != SemanticType::Reference ||
            parameter.referenceAccess != AccessMode::ReadOnly ||
            parameter.arguments.size() != 1 ||
            parameter.arguments.front() != input->info.type ||
            input->info.type.kind != SemanticType::Class ||
            input->info.category != ValueCategory::Value ||
            input->info.traits.containsBorrowedState) {
          return std::nullopt;
        }
        kind = HirCallInputKind::ReadTemporaryBorrow;
      } else if (parameter.kind == SemanticType::Reference) {
        kind = parameter.referenceAccess == AccessMode::Mutable
                   ? HirCallInputKind::MutableBorrow
                   : HirCallInputKind::ReadBorrow;
      } else if (orderedValueParameterType(parameter)) {
        kind = HirCallInputKind::Value;
      } else if (!orderedOwningParameterType(parameter) ||
                 input->info.type != parameter ||
                 (input->info.traits.containsBorrowedState &&
                  parameter.kind != SemanticType::Class)) {
        return std::nullopt;
      } else if (input->info.category == ValueCategory::Place &&
                 input->info.traits.copyable) {
        kind = HirCallInputKind::CopyValue;
      } else if (input->info.category == ValueCategory::Value &&
                 input->info.traits.movable &&
                 (!input->info.traits.containsBorrowedState ||
                  input->kind != HirValueKind::Call ||
                  !input->functionTarget)) {
        kind = HirCallInputKind::MoveValue;
      } else if (input->kind == HirValueKind::Call && input->functionTarget &&
                 input->borrowOrigin != BorrowOriginKind::None &&
                 input->info.category == ValueCategory::Value &&
                 input->info.traits.movable &&
                 input->info.traits.containsBorrowedState) {
        kind = HirCallInputKind::MoveBorrowedTemporary;
      } else {
        return std::nullopt;
      }

      result.push_back({.parameterIndex = index,
                        .value = source,
                        .parameterType = parameter,
                        .kind = kind,
                        .defaultArgument = index >= explicitArgumentCount});
    }
    return result;
  }

  [[nodiscard]] std::optional<std::vector<HirCallArgument>>
  orderedConstructorArguments(const HirValue &constructor,
                              const std::vector<HirValueId> &arguments,
                              const std::vector<SemanticType> &parameterTypes,
                              const HirBody &body,
                              std::size_t explicitArgumentCount) const {
    if (!constructor.constructorTarget ||
        constructor.constructorKind != ConstructorKind::Ordinary ||
        constructor.intrinsic != IntrinsicKind::None) {
      return std::nullopt;
    }
    return orderedArguments(arguments, parameterTypes, body,
                            explicitArgumentCount);
  }

  [[nodiscard]] std::optional<HirCallReceiver>
  orderedReceiver(HirValueId receiver, ReceiverMutability mutability,
                  CallReceiverMode receiverMode, const HirBody &body) const {
    HirValueId source = receiver;
    const HirValue *input = body.findValue(source);
    while (input != nullptr && input->kind == HirValueKind::Grouping &&
           input->operands.size() == 1 &&
           (input->retainedReceiverProjection ||
            (receiverMode == CallReceiverMode::Value &&
             input->info.type.kind == SemanticType::Class &&
             input->info.traits.containsBorrowedState))) {
      source = input->operands.front();
      input = body.findValue(source);
    }
    if (input == nullptr) {
      return std::nullopt;
    }

    // A selected raw member call borrows its pointee, not the pointer value
    // or the member-access expression's function type. Preserve that exact
    // source identity so MIR can evaluate the pointer once before arguments.
    if (input->kind == HirValueKind::MemberAccess &&
        input->unsafeOperation == UnsafeOperationKind::RawMember &&
        input->info.category != ValueCategory::Place &&
        input->operands.size() == 1 &&
        (receiverMode == CallReceiverMode::ReadOnlyPlace ||
         receiverMode == CallReceiverMode::MutablePlace)) {
      const HirValue *pointer = body.findValue(input->operands.front());
      if (pointer == nullptr ||
          pointer->info.type.kind != SemanticType::RawPointer ||
          pointer->info.type.arguments.size() != 1) {
        return std::nullopt;
      }
      return HirCallReceiver{
          .value = source,
          .kind = receiverMode == CallReceiverMode::MutablePlace
                      ? HirCallInputKind::MutableBorrow
                      : HirCallInputKind::ReadBorrow,
          .type = pointer->info.type.arguments.front(),
      };
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
          input->info.category != ValueCategory::Value) {
        return std::nullopt;
      }
      kind = receiverMode == CallReceiverMode::FreshTemporaryMutable
                 ? HirCallInputKind::MutableTemporaryBorrow
                 : HirCallInputKind::ReadTemporaryBorrow;
    }
    const bool consumesReceiver = receiverMode == CallReceiverMode::Consuming ||
                                  mutability == ReceiverMutability::Consuming;
    if (consumesReceiver) {
      if ((input->info.type.kind != SemanticType::Class &&
           input->info.type.kind != SemanticType::Lambda) ||
          input->info.category != ValueCategory::Value ||
          !input->info.traits.movable ||
          input->info.traits.containsBorrowedState) {
        return std::nullopt;
      }
      while (input->kind == HirValueKind::Grouping &&
             input->operands.size() == 1) {
        source = input->operands.front();
        input = body.findValue(source);
        if (input == nullptr)
          return std::nullopt;
      }
      kind = input->retainedReceiverProjection
                 ? HirCallInputKind::RetainedReceiver
             : input->kind == HirValueKind::Move
                 ? HirCallInputKind::MoveValue
                 : HirCallInputKind::MutableTemporaryBorrow;
    } else if (!temporaryReceiver &&
               input->info.category == ValueCategory::Place) {
      kind = receiverMode == CallReceiverMode::MutablePlace
                 ? HirCallInputKind::MutableBorrow
                 : HirCallInputKind::ReadBorrow;
    } else if (!temporaryReceiver && receiverMode != CallReceiverMode::Value) {
      return std::nullopt;
    } else if (!temporaryReceiver && input->kind == HirValueKind::Move &&
               input->info.type.kind == SemanticType::Lambda &&
               input->info.traits.drop == DropKind::Lexical &&
               instanceProvider->requiresActiveCleanupFor(input->info.type)) {
      // Moving an environment and invoking it are distinct operations. Even a
      // read-only invocation must stage the explicitly moved owner as such;
      // its full-expression obligation, not the call, retains capture cleanup.
      // A cleanup-free environment has no such obligation, so it keeps its
      // ordinary value stage, as in MIR's pack schedule; the preceding Move
      // still records source consumption.
      kind = HirCallInputKind::MoveValue;
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
      SemanticType referent = type.arguments.front();
      type = std::move(referent);
      category = ValueCategory::Place;
    }
    return {.type = type,
            .category = category,
            .access = access,
            .traits = instanceProvider->traitsFor(type)};
  }

  [[nodiscard]] static bool isAssignmentOperator(OverloadedOperator kind) {
    switch (kind) {
    case OverloadedOperator::Assignment:
    case OverloadedOperator::AddAssign:
    case OverloadedOperator::SubtractAssign:
    case OverloadedOperator::MultiplyAssign:
    case OverloadedOperator::DivideAssign:
    case OverloadedOperator::RemainderAssign:
    case OverloadedOperator::BitwiseAndAssign:
    case OverloadedOperator::BitwiseOrAssign:
    case OverloadedOperator::BitwiseXorAssign:
    case OverloadedOperator::ShiftLeftAssign:
    case OverloadedOperator::ShiftRightAssign:
      return true;
    default:
      return false;
    }
  }

  void configureOperatorCall(HirValue &value,
                             const ResolvedOperatorInfo &resolved,
                             HirBody &body) {
    value.info = operatorResultInfo(resolved);
    value.parameterTypes = resolved.parameterTypes;
    value.dispatch = resolved.dispatch;
    value.dispatchOwner = resolved.dispatchOwner;
    value.borrowOrigin = resolved.borrowOrigin;
    value.borrowArgument = resolved.borrowArgument;
    value.borrowAccess = resolved.borrowAccess;
    value.borrowArguments = resolved.borrowArguments;
    value.borrowFields = resolved.borrowFields;
    value.callableBoundary = resolved.boundary;
    if (resolved.receiverMode == CallReceiverMode::Consuming)
      value.callableInvocation = resolved.capability;
    value.definedFailure.propagation =
        resolved.dispatch == CallDispatch::Virtual
            ? FailurePropagationKind::VirtualCall
            : FailurePropagationKind::DirectCall;
    if (value.operands.empty()) {
      lifecycleValid = false;
      return;
    }

    const HirValueId receiverValue = value.operands.front();
    value.receiver = receiverValue;
    if (const FunctionInfo *target =
            baseModel->findFunction(resolved.function)) {
      SemanticType ownerType = resolved.dispatchOwner;
      if (ownerType.kind != SemanticType::Class ||
          ownerType.classId != target->ownerClass) {
        if (const HirValue *receiver = body.findValue(receiverValue)) {
          ownerType = receiver->info.type;
        }
      }
      value.functionTarget = enqueueFunction(
          *target,
          ownerType.kind == SemanticType::Class ? ownerType.arguments
                                                : std::vector<SemanticType>{},
          ownerType.kind == SemanticType::Class
              ? ownerType.valueArguments
              : std::vector<CompileTimeValue>{},
          {}, {}, resolved.returnType, resolved.parameterTypes);
    }

    if (!value.functionTarget ||
        value.operands.size() != resolved.parameterTypes.size() + 1) {
      lifecycleValid = false;
      return;
    }
    std::vector<HirValueId> arguments(value.operands.begin() + 1,
                                      value.operands.end());
    const std::optional<HirCallReceiver> plannedReceiver =
        orderedReceiver(receiverValue, resolved.receiverMutability,
                        resolved.receiverMode, body);
    const std::optional<std::vector<HirCallArgument>> plannedArguments =
        orderedArguments(arguments, resolved.parameterTypes, body);
    if (!plannedReceiver || !plannedArguments) {
      lifecycleValid = false;
      return;
    }
    value.callPlan = HirCallPlan{.receiver = *plannedReceiver,
                                 .arguments = *plannedArguments};
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
        .definedFailure = {},
        .borrowOrigin = value.borrowOrigin,
        .borrowArgument = value.borrowArgument,
        .borrowAccess = value.borrowAccess,
        .borrowArguments = value.borrowArguments,
        .borrowFields = value.borrowFields,
        .borrowPlace = value.borrowPlace,
        .dispatch = value.dispatch,
        .dispatchOwner = value.dispatchOwner,
        .receiver = receiver,
        .functionTarget = value.functionTarget,
        .callableBoundary = value.callableBoundary,
    };
    // For a store the source expression's failure facts describe assignment,
    // not the separately selected checked-access call.
    arrowCall.definedFailure.propagation =
        resolved.dispatch == CallDispatch::Virtual
            ? FailurePropagationKind::VirtualCall
            : FailurePropagationKind::DirectCall;
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
    // A read carries its access token; a store carries its assignment token.
    // Both project from the selected arrow call, but a store must retain '='.
    if (value.kind == HirValueKind::MemberAccess) {
      value.operation = TokenKind::DOT;
    }
    value.parameterTypes.clear();
    value.intrinsic = IntrinsicKind::None;
    value.synchronization = {};
    if (value.kind == HirValueKind::MemberSet) {
      // A built-in compound store retains its checked arithmetic outcomes.
      // Only the selected access call's propagation belongs to arrowCall.
      value.definedFailure.propagation = FailurePropagationKind::None;
    } else {
      value.definedFailure = {};
    }
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

  [[nodiscard]] static bool sourceLocationIntrinsic(IntrinsicKind intrinsic) {
    return intrinsic == IntrinsicKind::SourceFileName ||
           intrinsic == IntrinsicKind::SourceFunctionName ||
           intrinsic == IntrinsicKind::SourceLine ||
           intrinsic == IntrinsicKind::SourceColumn;
  }

  [[nodiscard]] SourceLocationLiteral
  sourceLocationAt(const Token &token) const {
    return {.file = token.source,
            .function = currentSourceFunctionName,
            .line = static_cast<std::uint32_t>(std::max(token.line, 0)),
            // Token offsets are retained, but HIR does not own source text or
            // line starts. Zero is the standard unavailable-column value.
            .column = 0};
  }

  [[nodiscard]] SourceLocationLiteral sourceLocationAt(const Call &call) const {
    return sourceLocationAt(call.leftParen() ? *call.leftParen()
                                             : call.paren());
  }

  [[nodiscard]] static SourceSpan callInstantiationSite(const Call &call) {
    const Expr *callee = call.callee().get();
    while (const auto *group = exprAs<Grouping>(callee))
      callee = group->expression().get();
    if (const auto *name = exprAs<Variable>(callee))
      return tokenSpan(name->name());
    if (const auto *name = exprAs<QualifiedName>(callee))
      return tokenSpan(name->name().last());
    if (const auto *member = exprAs<Get>(callee))
      return tokenSpan(member->name());
    return tokenSpan(call.leftParen() ? *call.leftParen() : call.paren());
  }

  [[nodiscard]] std::optional<SourceLocationLiteral>
  sourceLocationAt(const Expr &expression) const {
    if (const auto *call = exprAs<Call>(&expression)) {
      return sourceLocationAt(*call);
    }
    if (const auto *initializer = exprAs<DirectInitializer>(&expression)) {
      return sourceLocationAt(initializer->brace());
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<HirValueId> lowerDefaultArgument(
      const Expr *expression, const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body,
      std::optional<SourceLocationLiteral> caller = std::nullopt) {
    if (expression == nullptr) {
      return std::nullopt;
    }
    if (!activeDefaultArguments.insert(expression).second) {
      lifecycleValid = false;
      return std::nullopt;
    }
    const std::optional<SourceLocationLiteral> enclosing =
        activeDefaultSourceLocation;
    if (!activeDefaultSourceLocation && caller) {
      activeDefaultSourceLocation = std::move(caller);
    }
    const std::optional<HirValueId> result = lowerExpression(
        *expression, model, classArguments, classValueArguments, body);
    activeDefaultSourceLocation = enclosing;
    activeDefaultArguments.erase(expression);
    return result;
  }

  [[nodiscard]] std::optional<HirValueId> lowerDefaultArrayElement(
      const ArrayInitializer &initializer,
      const ResolvedConstructionInfo &construction, const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    HirValue value{
        .id = nextValueId++,
        .kind = HirValueKind::DirectInitializer,
        // Empty-brace array elements have no source expression of their own.
        // The parent ArrayInitializer remains the source-authored root.
        .source = nullptr,
        .info = {.type = construction.constructedType,
                 .category = ValueCategory::Value,
                 .access = AccessMode::ReadOnly,
                 .traits =
                     instanceProvider->traitsFor(construction.constructedType)},
        .parameterTypes = construction.parameterTypes,
        .definedFailure = {.propagation =
                               construction.constructor != 0 &&
                                       !construction.generatedDefault
                                   ? FailurePropagationKind::Constructor
                                   : FailurePropagationKind::None},
        .borrowOrigin = construction.borrowOrigin,
        .borrowArgument = construction.borrowArgument,
        .borrowAccess = construction.borrowAccess,
        .borrowArguments = construction.borrowArguments,
        .borrowFields = construction.borrowFields,
        .constructorKind = construction.kind,
    };

    const HirConstructorInstanceId target =
        enqueueConstructor(construction, tokenSpan(initializer.brace()));
    if (target != 0) {
      value.constructorTarget = target;
      value.definedFailure.propagation = FailurePropagationKind::Constructor;
    } else if (construction.constructor != 0 &&
               !construction.generatedDefault) {
      lifecycleValid = false;
      return std::nullopt;
    }

    const SemanticModel *defaultModel = &model;
    std::vector<SemanticType> targetClassArguments = classArguments;
    std::vector<CompileTimeValue> targetClassValueArguments =
        classValueArguments;
    if (target != 0 && target <= output.program.constructors.size()) {
      const HirConstructorInstance &targetInstance =
          output.program.constructors[target - 1];
      if (targetInstance.owner != 0 &&
          targetInstance.owner <= output.program.classes.size()) {
        const HirClassInstance &owner =
            output.program.classes[targetInstance.owner - 1];
        targetClassArguments = owner.typeArguments;
        targetClassValueArguments = owner.valueArguments;
      }
      if (const SemanticInstanceAnalysis *analysis =
              ensureConstructorAnalysis(target)) {
        defaultModel = &analysis->model;
      }
    }
    for (const Expr *defaultArgument : construction.defaultArguments) {
      if (const std::optional<HirValueId> lowered = lowerDefaultArgument(
              defaultArgument, *defaultModel, targetClassArguments,
              targetClassValueArguments, body,
              sourceLocationAt(initializer.brace()))) {
        value.operands.push_back(*lowered);
      } else {
        lifecycleValid = false;
        return std::nullopt;
      }
    }
    if (value.operands.size() != value.parameterTypes.size()) {
      lifecycleValid = false;
      return std::nullopt;
    }
    if (!value.parameterTypes.empty()) {
      if (std::optional<std::vector<HirCallArgument>> arguments =
              orderedConstructorArguments(value, value.operands,
                                          value.parameterTypes, body, 0)) {
        value.callPlan = HirCallPlan{.receiver = std::nullopt,
                                     .arguments = std::move(*arguments)};
      } else {
        lifecycleValid = false;
        return std::nullopt;
      }
    }

    const HirValueId id = value.id;
    body.values.push_back(std::move(value));
    return id;
  }

  // One element of an empty-brace fixed-array initializer. A class element is
  // the recorded leaf default construction; an array element is a synthetic
  // nested ArrayInitializer whose own elements recurse toward that leaf.
  [[nodiscard]] std::optional<HirValueId> lowerDefaultNestedArrayElement(
      const SemanticType &type, const ArrayInitializer &initializer,
      const ResolvedConstructionInfo &construction, const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    if (type.kind == SemanticType::Class) {
      if (type != construction.constructedType) {
        return std::nullopt;
      }
      return lowerDefaultArrayElement(initializer, construction, model,
                                      classArguments, classValueArguments,
                                      body);
    }
    if (!fixedClassLeafArrayType(type)) {
      return std::nullopt;
    }
    std::vector<HirValueId> elements;
    elements.reserve(static_cast<std::size_t>(type.arrayLength));
    for (std::uint64_t index = 0; index < type.arrayLength; ++index) {
      if (const std::optional<HirValueId> element =
              lowerDefaultNestedArrayElement(
                  type.arguments.front(), initializer, construction, model,
                  classArguments, classValueArguments, body)) {
        elements.push_back(*element);
      } else {
        return std::nullopt;
      }
    }
    HirValue value{
        .id = nextValueId++,
        .kind = HirValueKind::ArrayInitializer,
        // Synthetic nested levels share the parent initializer's source root.
        .source = nullptr,
        .info = {.type = type,
                 .category = ValueCategory::Value,
                 .access = AccessMode::ReadOnly,
                 .traits = instanceProvider->traitsFor(type)},
        .operands = std::move(elements),
    };
    const HirValueId id = value.id;
    body.values.push_back(std::move(value));
    return id;
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

  [[nodiscard]] std::optional<HirValueId> lowerExpectedPropagation(
      const Unary &expression, const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    const std::optional<HirValueId> operand = lowerExpression(
        expression.right(), model, classArguments, classValueArguments, body);
    const HirValue *operandValue = operand ? body.findValue(*operand) : nullptr;
    const ExpressionInfo *resultInfo = model.findExpression(expression);
    if (!operand || operandValue == nullptr || resultInfo == nullptr ||
        operandValue->info.type.kind != SemanticType::Expected ||
        operandValue->info.type.arguments.size() != 2) {
      lifecycleValid = false;
      return std::nullopt;
    }

    const SemanticType expectedType = operandValue->info.type;
    const bool borrowedSuccess =
        expectedType.arguments[0].kind == SemanticType::Reference;
    const SemanticType valueType =
        borrowedSuccess ? expectedType.arguments[0].arguments.front()
                        : expectedType.arguments[0];
    const SemanticType errorType = expectedType.arguments[1];
    const bool moves = operandValue->info.category == ValueCategory::Value;
    const AccessMode extractionAccess =
        moves ? AccessMode::Mutable : AccessMode::ReadOnly;
    const auto appendExtraction = [&](IntrinsicKind intrinsic,
                                      const SemanticType &type) {
      const HirValueId id = nextValueId++;
      HirValue extraction{
          .id = id,
          .kind = HirValueKind::Call,
          .info = {.type = type,
                   .category = ValueCategory::Place,
                   .access = borrowedSuccess &&
                                     intrinsic == IntrinsicKind::ExpectedValue
                                 ? AccessMode::ReadOnly
                                 : extractionAccess,
                   .traits = instanceProvider->traitsFor(type)},
          .operands = {*operand},
          .intrinsic = intrinsic,
          .borrowOrigin =
              borrowedSuccess && intrinsic == IntrinsicKind::ExpectedValue
                  ? BorrowOriginKind::Receiver
                  : BorrowOriginKind::None,
          .borrowAccess = AccessMode::ReadOnly,
          .receiver = *operand,
      };
      body.values.push_back(std::move(extraction));
      return id;
    };

    const auto appendTransport = [&](HirValueId extraction,
                                     const SemanticType &type) {
      if (!moves) {
        return extraction;
      }
      const HirValueId id = nextValueId++;
      body.values.push_back(
          {.id = id,
           .kind = HirValueKind::Move,
           .info = {.type = type,
                    .category = ValueCategory::Value,
                    .access = AccessMode::ReadOnly,
                    .traits = instanceProvider->traitsFor(type)},
           .operands = {extraction},
           .intrinsic = IntrinsicKind::Move});
      return id;
    };

    const HirValueId successExtraction =
        appendExtraction(IntrinsicKind::ExpectedValue, valueType);
    const HirValueId successValue =
        valueType == SemanticType::Void ? HirValueId{0}
        : borrowedSuccess               ? successExtraction
                          : appendTransport(successExtraction, valueType);
    const HirValueId errorExtraction =
        appendExtraction(IntrinsicKind::ExpectedError, errorType);
    const HirValueId errorValue = appendTransport(errorExtraction, errorType);

    const SemanticType unexpectedType(SemanticType::Unexpected, {errorType});
    const HirValueId unexpected = nextValueId++;
    body.values.push_back(
        {.id = unexpected,
         .kind = HirValueKind::Unexpected,
         .info = {.type = unexpectedType,
                  .category = ValueCategory::Value,
                  .access = AccessMode::ReadOnly,
                  .traits = instanceProvider->traitsFor(unexpectedType)},
         .operands = {errorValue}});

    std::vector<HirValueId> operands{*operand, successExtraction};
    if (successValue != 0) {
      operands.push_back(successValue);
    }
    operands.push_back(errorExtraction);
    operands.push_back(errorValue);
    operands.push_back(unexpected);

    const HirValueId id = nextValueId++;
    body.values.push_back({.id = id,
                           .kind = HirValueKind::ExpectedPropagation,
                           .source = &expression,
                           .info = *resultInfo,
                           .operands = std::move(operands),
                           .operation = TokenKind::QUESTION,
                           .expectedPropagation = HirExpectedPropagationPlan{
                               .operand = *operand,
                               .successExtraction = successExtraction,
                               .successValue = successValue,
                               .errorExtraction = errorExtraction,
                               .errorValue = errorValue,
                               .unexpected = unexpected,
                               .moves = moves}});
    output.program.sourceValueIds[&expression].push_back(id);
    return id;
  }

  [[nodiscard]] std::optional<HirValueId> lowerExpectedExpect(
      const Call &expression, const ExpectedCombinatorInfo &expect,
      const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    const auto *member = exprAs<Get>(expression.callee().get());
    const ExpressionInfo *resultInfo = model.findExpression(expression);
    const DefinedFailureOperation *failure =
        model.findDefinedFailure(expression);
    const bool borrowedSuccess =
        expect.sourceType.arguments.size() == 2 &&
        expect.sourceType.arguments.front().kind == SemanticType::Reference;
    const SemanticType payload =
        borrowedSuccess ? expect.sourceType.arguments.front().arguments.front()
        : expect.sourceType.arguments.empty()
            ? SemanticType::Unknown
            : expect.sourceType.arguments.front();
    if (member == nullptr || expression.arguments().size() != 1 ||
        resultInfo == nullptr || failure == nullptr ||
        expect.sourceType.kind != SemanticType::Expected ||
        expect.sourceType.arguments.size() != 2 ||
        expect.resultType != payload || expect.resultType != resultInfo->type ||
        expect.mapper != 0 || expect.lambda != 0 ||
        expect.mapperType != SemanticType::StringView) {
      lifecycleValid = false;
      return std::nullopt;
    }
    const auto receiver = lowerExpression(
        member->object(), model, classArguments, classValueArguments, body);
    const auto message =
        lowerExpression(expression.arguments().front(), model, classArguments,
                        classValueArguments, body);
    const HirValue *receiverValue =
        receiver ? body.findValue(*receiver) : nullptr;
    const HirValue *messageValue = message ? body.findValue(*message) : nullptr;
    if (receiverValue == nullptr || messageValue == nullptr ||
        receiverValue->info.type != expect.sourceType ||
        receiverValue->info.category != ValueCategory::Value ||
        messageValue->info.type != SemanticType::StringView) {
      lifecycleValid = false;
      return std::nullopt;
    }

    const auto appendObserver = [&](IntrinsicKind intrinsic,
                                    const SemanticType &type,
                                    ValueCategory category) {
      const HirValueId id = nextValueId++;
      body.values.push_back(
          {.id = id,
           .kind = HirValueKind::Call,
           .info = {.type = type,
                    .category = category,
                    .access = borrowedSuccess &&
                                      intrinsic == IntrinsicKind::ExpectedValue
                                  ? AccessMode::ReadOnly
                                  : AccessMode::Mutable,
                    .traits = instanceProvider->traitsFor(type)},
           .operands = {*receiver},
           .intrinsic = intrinsic,
           .borrowOrigin =
               borrowedSuccess && intrinsic == IntrinsicKind::ExpectedValue
                   ? BorrowOriginKind::Receiver
                   : BorrowOriginKind::None,
           .borrowAccess = AccessMode::ReadOnly,
           .receiver = *receiver});
      return id;
    };
    const HirValueId hasValue =
        appendObserver(IntrinsicKind::ExpectedHasValue, SemanticType::Bool,
                       ValueCategory::Value);
    const SemanticType valueType = expect.resultType;
    const HirValueId extraction =
        appendObserver(IntrinsicKind::ExpectedValue, valueType,
                       valueType == SemanticType::Void ? ValueCategory::Value
                                                       : ValueCategory::Place);
    HirValueId success = extraction;
    if (valueType != SemanticType::Void && !borrowedSuccess) {
      success = nextValueId++;
      body.values.push_back({.id = success,
                             .kind = HirValueKind::Move,
                             .info = *resultInfo,
                             .operands = {extraction},
                             .intrinsic = IntrinsicKind::Move});
    }
    const auto arguments =
        orderedArguments({*message}, {SemanticType::StringView}, body, 1);
    if (!arguments) {
      lifecycleValid = false;
      return std::nullopt;
    }
    const HirValueId panic = nextValueId++;
    body.values.push_back(
        {.id = panic,
         .kind = HirValueKind::Call,
         .source = &expression,
         .info = {.type = SemanticType::Void,
                  .category = ValueCategory::Value,
                  .access = AccessMode::ReadOnly,
                  .traits = instanceProvider->traitsFor(SemanticType::Void)},
         .operands = {*message},
         .parameterTypes = {SemanticType::StringView},
         .intrinsic = IntrinsicKind::Panic,
         .neverReturns = true,
         .tracksCaller = true,
         .definedFailure = *failure,
         .callPlan =
             HirCallPlan{.receiver = std::nullopt, .arguments = *arguments}});
    const HirValueId id = nextValueId++;
    body.values.push_back({.id = id,
                           .kind = HirValueKind::Conditional,
                           .source = &expression,
                           .info = *resultInfo,
                           .operands = {hasValue, success, panic},
                           .conditionalInputs = {*receiver, *message}});
    output.program.sourceValueIds[&expression].push_back(id);
    return id;
  }

  [[nodiscard]] std::optional<HirValueId> lowerExpectedCombinator(
      const Call &expression, const ExpectedCombinatorInfo &transform,
      const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    if (transform.kind == ExpectedCombinatorKind::Expect) {
      return lowerExpectedExpect(expression, transform, model, classArguments,
                                 classValueArguments, body);
    }
    const auto *member = exprAs<Get>(expression.callee().get());
    const ExpressionInfo *resultInfo = model.findExpression(expression);
    if (member == nullptr || expression.arguments().size() != 1 ||
        resultInfo == nullptr ||
        transform.sourceType.kind != SemanticType::Expected ||
        transform.sourceType.arguments.size() != 2 ||
        transform.resultType.kind != SemanticType::Expected ||
        transform.resultType.arguments.size() != 2 ||
        transform.resultType != resultInfo->type ||
        ((transform.mapper == 0) == (transform.lambda == 0))) {
      lifecycleValid = false;
      return std::nullopt;
    }

    const std::optional<HirValueId> operand = lowerExpression(
        member->object(), model, classArguments, classValueArguments, body);
    std::optional<HirValueId> mapper =
        lowerExpression(expression.arguments().front(), model, classArguments,
                        classValueArguments, body);
    const HirValue *operandValue = operand ? body.findValue(*operand) : nullptr;
    const HirValue *mapperValue = mapper ? body.findValue(*mapper) : nullptr;
    // Grouping does not create another closure environment. Preserve the
    // exact producer as ordinary concrete-lambda calls do.
    while (mapperValue != nullptr &&
           mapperValue->kind == HirValueKind::Grouping &&
           mapperValue->operands.size() == 1) {
      mapper = mapperValue->operands.front();
      mapperValue = body.findValue(*mapper);
    }
    if (!operand || !mapper || operandValue == nullptr ||
        mapperValue == nullptr ||
        operandValue->info.type != transform.sourceType ||
        operandValue->info.category != ValueCategory::Value ||
        mapperValue->info.type != transform.mapperType ||
        (transform.mapper != 0 &&
         (mapperValue->kind != HirValueKind::FunctionItem ||
          !mapperValue->functionTarget))) {
      lifecycleValid = false;
      return std::nullopt;
    }

    const SemanticType valueType = transform.sourceType.arguments[0];
    const SemanticType errorType = transform.sourceType.arguments[1];
    const bool mapsError =
        transform.kind == ExpectedCombinatorKind::TransformError;
    const bool chainsExpected =
        transform.kind == ExpectedCombinatorKind::AndThen;
    const SemanticType mappedType =
        chainsExpected ? transform.resultType
                       : transform.resultType.arguments[mapsError ? 1 : 0];
    const SemanticType inputType = mapsError ? errorType : valueType;
    // Appending synthetic values can reallocate body.values. Retain the
    // selected instance identity, never a pointer into that growing vector.
    const auto mapperTarget = mapperValue->functionTarget;
    const HirFunctionInstance *mapperFunction =
        mapperTarget ? output.program.findFunctionInstance(*mapperTarget)
                     : nullptr;
    const bool mapperNeverReturns =
        mapperFunction != nullptr && mapperFunction->neverReturns;
    const bool mapperTracksCaller =
        mapperFunction != nullptr && mapperFunction->tracksCaller;
    std::optional<HirLambdaId> mapperLambda;
    if (transform.lambda != 0) {
      if (const auto target = lambdaTargets.find(transform.lambda);
          target != lambdaTargets.end()) {
        mapperLambda = target->second;
      } else {
        const auto existing = std::find_if(
            output.program.lambdas.begin(), output.program.lambdas.end(),
            [&](const HirLambda &candidate) {
              return lambdaMatchesType(candidate, transform.mapperType);
            });
        if (existing != output.program.lambdas.end())
          mapperLambda = existing->id;
      }
      if (!mapperLambda) {
        lifecycleValid = false;
        return std::nullopt;
      }
    }
    const auto appendExtraction = [&](IntrinsicKind intrinsic,
                                      const SemanticType &type) {
      const HirValueId id = nextValueId++;
      body.values.push_back(
          {.id = id,
           .kind = HirValueKind::Call,
           .info = {.type = type,
                    .category = ValueCategory::Place,
                    .access = AccessMode::Mutable,
                    .traits = instanceProvider->traitsFor(type)},
           .operands = {*operand},
           .intrinsic = intrinsic,
           .receiver = *operand});
      return id;
    };
    const auto appendMove = [&](HirValueId source, const SemanticType &type) {
      const HirValueId id = nextValueId++;
      body.values.push_back(
          {.id = id,
           .kind = HirValueKind::Move,
           .info = {.type = type,
                    .category = ValueCategory::Value,
                    .access = AccessMode::ReadOnly,
                    .traits = instanceProvider->traitsFor(type)},
           .operands = {source},
           .intrinsic = IntrinsicKind::Move});
      return id;
    };

    const HirValueId hasValue = nextValueId++;
    body.values.push_back(
        {.id = hasValue,
         .kind = HirValueKind::Call,
         .info = {.type = SemanticType::Bool,
                  .category = ValueCategory::Value,
                  .access = AccessMode::ReadOnly,
                  .traits = instanceProvider->traitsFor(SemanticType::Bool)},
         .operands = {*operand},
         .intrinsic = IntrinsicKind::ExpectedHasValue,
         .receiver = *operand});

    const auto appendVoidSuccess = [&]() {
      const HirValueId success = nextValueId++;
      body.values.push_back({.id = success,
                             .kind = HirValueKind::Call,
                             .info = {.type = transform.resultType,
                                      .category = ValueCategory::Value,
                                      .access = AccessMode::ReadOnly,
                                      .traits = instanceProvider->traitsFor(
                                          transform.resultType)}});
      return success;
    };

    std::vector<HirValueId> mapperInputs;
    std::vector<SemanticType> mapperParameters;
    if (inputType != SemanticType::Void) {
      const HirValueId extraction =
          appendExtraction(mapsError ? IntrinsicKind::ExpectedError
                                     : IntrinsicKind::ExpectedValue,
                           inputType);
      mapperInputs.push_back(appendMove(extraction, inputType));
      mapperParameters.push_back(inputType);
    }
    std::vector<HirValueId> mapperOperands{*mapper};
    mapperOperands.insert(mapperOperands.end(), mapperInputs.begin(),
                          mapperInputs.end());

    const HirValueId mapped = nextValueId++;
    HirValue mapperCall{
        .id = mapped,
        .kind = HirValueKind::Call,
        .source = &expression,
        .info = {.type = mappedType,
                 .category = ValueCategory::Value,
                 .access = AccessMode::ReadOnly,
                 .traits = instanceProvider->traitsFor(mappedType)},
        .operands = std::move(mapperOperands),
        .parameterTypes = mapperParameters,
        .neverReturns = mapperNeverReturns,
        .tracksCaller = mapperTracksCaller,
        .functionTarget = mapperTarget,
        .lambdaTarget = mapperLambda,
    };
    const std::optional<std::vector<HirCallArgument>> mapperArguments =
        orderedArguments(mapperInputs, mapperParameters, body, 1);
    if (!mapperArguments) {
      lifecycleValid = false;
      return std::nullopt;
    }
    mapperCall.callPlan =
        HirCallPlan{.receiver = std::nullopt, .arguments = *mapperArguments};
    if (mapperLambda) {
      mapperCall.receiver = *mapper;
      mapperCall.callableInvocation = CallableInvocationCapability::Read;
      mapperCall.callPlan->receiver = orderedReceiver(
          *mapper, ReceiverMutability::ReadOnly, CallReceiverMode::Value, body);
    }
    if (const DefinedFailureOperation *failure =
            model.findDefinedFailure(expression)) {
      mapperCall.definedFailure = *failure;
    }
    body.values.push_back(std::move(mapperCall));

    HirValueId successArm = mapped;
    HirValueId errorValue = mapped;
    if (mapsError) {
      successArm =
          valueType == SemanticType::Void
              ? appendVoidSuccess()
              : appendMove(
                    appendExtraction(IntrinsicKind::ExpectedValue, valueType),
                    valueType);
    } else {
      errorValue = appendMove(
          appendExtraction(IntrinsicKind::ExpectedError, errorType), errorType);
      if (mappedType == SemanticType::Void && !mapperNeverReturns) {
        successArm = appendVoidSuccess();
        body.values.back().sequencedInputs = {mapped};
      }
    }
    const SemanticType unexpectedType(SemanticType::Unexpected,
                                      {mapsError ? mappedType : errorType});
    HirValueId unexpected = mapped;
    if (!mapsError || !mapperNeverReturns) {
      unexpected = nextValueId++;
      body.values.push_back(
          {.id = unexpected,
           .kind = HirValueKind::Unexpected,
           .info = {.type = unexpectedType,
                    .category = ValueCategory::Value,
                    .access = AccessMode::ReadOnly,
                    .traits = instanceProvider->traitsFor(unexpectedType)},
           .operands = {errorValue}});
    }

    const HirValueId id = nextValueId++;
    body.values.push_back({.id = id,
                           .kind = HirValueKind::Conditional,
                           .source = &expression,
                           .info = *resultInfo,
                           .operands = {hasValue, successArm, unexpected},
                           .conditionalInputs = {*operand, *mapper}});
    output.program.sourceValueIds[&expression].push_back(id);
    return id;
  }

  [[nodiscard]] std::optional<HirValueId> lowerCollection(
      const ArrayInitializer &expression, const ResolvedCollectionInfo &plan,
      const SemanticModel &model,
      const std::vector<SemanticType> &classArguments,
      const std::vector<CompileTimeValue> &classValueArguments, HirBody &body) {
    const auto call =
        [&](FunctionId function, std::vector<HirValueId> operands,
            std::vector<SemanticType> parameters) -> std::optional<HirValueId> {
      const FunctionInfo *declaration = baseModel->findFunction(function);
      const auto arguments = orderedArguments(operands, parameters, body);
      if (declaration == nullptr || !arguments) {
        lifecycleValid = false;
        return std::nullopt;
      }
      const auto target = enqueueFunction(
          *declaration, plan.type.arguments, plan.type.valueArguments, {}, {},
          plan.type, parameters, tokenSpan(expression.brace()));
      if (target == 0) {
        lifecycleValid = false;
        return std::nullopt;
      }
      const HirValueId id = nextValueId++;
      // Even a trivially destructible collection needs a concrete temporary:
      // each protocol call must publish successfully before the next element
      // starts, rather than remaining an inline nested class-valued call.
      collectionResults.insert(id);
      body.values.push_back(
          {.id = id,
           .kind = HirValueKind::Call,
           .source = &expression,
           .info = {.type = plan.type,
                    .category = ValueCategory::Value,
                    .access = AccessMode::ReadOnly,
                    .traits = instanceProvider->traitsFor(plan.type)},
           .operands = std::move(operands),
           .parameterTypes = std::move(parameters),
           .definedFailure = {.propagation =
                                  FailurePropagationKind::DirectCall},
           .callPlan =
               HirCallPlan{.receiver = std::nullopt, .arguments = *arguments},
           .functionTarget = target});
      output.program.sourceValueIds[&expression].push_back(id);
      return id;
    };
    const HirValueId count =
        lowerStructuredIndex(expression.elements().size(), body);
    auto result = call(plan.initialize, {count}, {SemanticType::UInt64});
    for (const ExprPtr &element : expression.elements()) {
      if (!result) {
        return std::nullopt;
      }
      const auto value = lowerExpression(element, model, classArguments,
                                         classValueArguments, body);
      if (!value) {
        lifecycleValid = false;
        return std::nullopt;
      }
      result =
          call(plan.insert, {*result, *value}, {plan.type, plan.elementType});
    }
    return result;
  }

  [[nodiscard]] std::optional<HirValueId>
  lowerExpression(const Expr &expression, const SemanticModel &model,
                  const std::vector<SemanticType> &classArguments,
                  const std::vector<CompileTimeValue> &classValueArguments,
                  HirBody &body) {
    ensurePlaceDomain(body);
    if (const Expr *alias = model.findExpressionAlias(expression)) {
      // A reflected field access lowers as the field projection it denotes.
      const auto id = lowerExpression(*alias, model, classArguments,
                                      classValueArguments, body);
      if (id) {
        body.sourceAliases.push_back({.source = &expression, .value = *id});
        output.program.sourceValueIds[&expression].push_back(*id);
      }
      return id;
    }
    if (const Expr *producer = model.directInitializationSource(expression)) {
      const ExpressionInfo *info = model.findExpression(expression);
      const ExpressionInfo *input = model.findExpression(*producer);
      const Binary *comma = model.builtinComma(expression);
      const bool wrapper =
          exprAs<Grouping>(&expression) != nullptr || comma != nullptr;
      if (info == nullptr || input == nullptr || info->type != input->type ||
          (!wrapper && !model.isFreshClassResult(*producer))) {
        lifecycleValid = false;
        return std::nullopt;
      }
      // A comma's left operand completes, with its result discarded, before
      // the right operand begins; its temporaries keep the enclosing
      // full-expression lifetime. Sequence it before the producer itself.
      std::optional<HirValueId> discarded;
      if (comma != nullptr) {
        discarded = lowerExpression(comma->left(), model, classArguments,
                                    classValueArguments, body);
        if (!discarded) {
          lifecycleValid = false;
          return std::nullopt;
        }
      }
      const auto id = lowerExpression(*producer, model, classArguments,
                                      classValueArguments, body);
      if (id && discarded) {
        HirValue *produced = nullptr;
        for (HirValue &candidate : body.values)
          if (candidate.id == *id)
            produced = &candidate;
        if (produced == nullptr) {
          lifecycleValid = false;
          return std::nullopt;
        }
        produced->sequencedInputs.insert(produced->sequencedInputs.begin(),
                                         *discarded);
      }
      if (id) {
        const bool grouping = wrapper;
        std::optional<SemanticDirectInitializationProof> proof;
        if (!grouping) {
          proof = model.directInitializationProof(expression);
          if (!proof) {
            lifecycleValid = false;
            return std::nullopt;
          }
        }
        body.directInitializations.push_back(
            {.source = &expression,
             .producer = producer,
             .value = *id,
             .type = info->type,
             .constructionProof = std::move(proof)});
        output.program.sourceValueIds[&expression].push_back(*id);
      }
      return id;
    }
    if (const auto *initializer = exprAs<ArrayInitializer>(&expression)) {
      if (const auto *collection = model.findCollection(*initializer)) {
        return lowerCollection(*initializer, *collection, model, classArguments,
                               classValueArguments, body);
      }
    }
    if (const auto *unary = dynamic_cast<const Unary *>(&expression);
        unary != nullptr && unary->oper().kind == TokenKind::QUESTION) {
      return lowerExpectedPropagation(*unary, model, classArguments,
                                      classValueArguments, body);
    }
    if (const auto *call = exprAs<Call>(&expression)) {
      if (const ExpectedCombinatorInfo *transform =
              model.findExpectedCombinator(*call)) {
        return lowerExpectedCombinator(*call, *transform, model, classArguments,
                                       classValueArguments, body);
      }
    }
    // Grouping does not create another closure environment. A parenthesized
    // lambda with its operand's exact type, category, access and traits is
    // that operand's value, as lambda calls and expected mappers already
    // treat it, so every lambda consumer sees the producer itself.
    if (const auto *grouping = exprAs<Grouping>(&expression)) {
      const ExpressionInfo *outer = model.findExpression(expression);
      const ExpressionInfo *inner =
          grouping->expression() ? model.findExpression(*grouping->expression())
                                 : nullptr;
      if (outer != nullptr && inner != nullptr &&
          outer->type.kind == SemanticType::Lambda && *outer == *inner) {
        const std::optional<HirValueId> id =
            lowerExpression(grouping->expression(), model, classArguments,
                            classValueArguments, body);
        if (id) {
          body.sourceAliases.push_back({.source = &expression, .value = *id});
          output.program.sourceValueIds[&expression].push_back(*id);
        }
        return id;
      }
    }
    const Expr *raw = &expression;
    HirValueKind kind = HirValueKind::Literal;
    std::vector<HirValueId> operands;
    std::optional<TokenKind> operation;
    std::optional<Literal> literal;
    std::optional<HirValueId> receiver;
    std::optional<HirLambdaId> lambdaTarget;
    std::optional<HirNativeCallbackAdapterId> nativeCallbackAdapter;
    std::optional<HirThreadTaskAdapterId> threadTaskAdapter;
    std::optional<HirFunctionInstanceId> functionItemTarget;
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

    if (const auto *assign = exprAs<Assign>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*assign);
      if (resolved != nullptr && isAssignmentOperator(resolved->kind)) {
        kind = HirValueKind::Call;
        const SymbolId symbol = model.findResolvedSymbol(*assign);
        HirValue target;
        target.id = nextValueId++;
        target.kind = assign->path().segments.size() == 1
                          ? HirValueKind::Variable
                          : HirValueKind::QualifiedName;
        target.symbol = symbol;
        // Assignment receivers are synthetic: semantic analysis owns their
        // exact referenced type, access, and nominal lifecycle/borrow traits.
        // Reconstructing those facts from the declaration symbol creates a
        // second, incomplete type-classification path in HIR.
        target.info = resolved->receiverInfo;
        if (target.info.type == SemanticType::Unknown ||
            target.info.category != ValueCategory::Place) {
          lifecycleValid = false;
          return std::nullopt;
        }
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
    } else if (const auto *initializer = exprAs<ArrayInitializer>(raw)) {
      kind = HirValueKind::ArrayInitializer;
      if (initializer->elements().empty()) {
        const ResolvedConstructionInfo *construction =
            model.findArrayElementConstruction(*initializer);
        const ExpressionInfo *arrayInfo = model.findExpression(*initializer);
        if (construction != nullptr && arrayInfo != nullptr &&
            fixedClassLeafArrayType(arrayInfo->type)) {
          operands.reserve(
              static_cast<std::size_t>(arrayInfo->type.arrayLength));
          for (std::uint64_t index = 0; index < arrayInfo->type.arrayLength;
               ++index) {
            if (const std::optional<HirValueId> element =
                    lowerDefaultNestedArrayElement(
                        arrayInfo->type.arguments.front(), *initializer,
                        *construction, model, classArguments,
                        classValueArguments, body)) {
              operands.push_back(*element);
            } else {
              lifecycleValid = false;
              break;
            }
          }
        }
      } else {
        for (const ExprPtr &element : initializer->elements()) {
          lowerOperand(element);
        }
      }
    } else if (const auto *binary = exprAs<Binary>(raw)) {
      kind = HirValueKind::Binary;
      operation = binary->oper().kind;
      lowerOperand(binary->left());
      lowerOperand(binary->right());
    } else if (const auto *call = exprAs<Call>(raw)) {
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
        if (construction == nullptr &&
            (resolved == nullptr || resolved->argumentReceiver == nullptr) &&
            model.findExpression(*call->callee()) != nullptr) {
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
            exprAs<Variable>(call->callee().get()) != nullptr &&
            currentReceiverType.kind == SemanticType::Class) {
          receiver = lowerImplicitReceiver(body);
        }
      }
      if (!validResolvedCallArguments(*call, resolved))
        lifecycleValid = false;
      for (std::size_t index = 0;
           index < resolvedCallArgumentCount(*call, resolved); ++index) {
        lowerOperand(*resolvedCallArgument(*call, resolved, index));
      }
    } else if (const auto *conditional = exprAs<ConditionalExpr>(raw)) {
      kind = HirValueKind::Conditional;
      lowerOperand(conditional->condition());
      lowerOperand(conditional->thenExpression());
      lowerOperand(conditional->elseExpression());
    } else if (const auto *conversion = exprAs<Conversion>(raw)) {
      kind = HirValueKind::Conversion;
      lowerOperand(conversion->value());
    } else if (const auto *initializer = exprAs<DirectInitializer>(raw)) {
      kind = HirValueKind::DirectInitializer;
      const auto *construction = model.findConstruction(*raw);
      if (construction != nullptr &&
          !construction->designatedArguments.empty()) {
        const SemanticType &target = construction->constructedType;
        const ClassTypeInfo *owner = baseModel->findClassType(target.classId);
        std::optional<SemanticInstanceAnalysis> defaults;
        for (const Expr *argument : construction->designatedArguments) {
          const bool declarationDefault =
              owner != nullptr &&
              std::any_of(owner->fields.begin(), owner->fields.end(),
                          [&](const ClassFieldTypeInfo &field) {
                            return field.declaration != nullptr &&
                                   field.declaration->initializer().get() ==
                                       argument;
                          });
          if (declarationDefault && !defaults &&
              (!target.arguments.empty() || !target.valueArguments.empty())) {
            defaults.emplace(instanceProvider->analyzeClassFieldInitializers(
                target.classId, target.arguments, target.valueArguments));
            retainInstanceSymbols(defaults->model);
            appendInstanceDiagnostics(std::move(defaults->diagnostics),
                                      tokenSpan(initializer->brace()),
                                      discoveryOrigin);
          }
          if (auto lowered = lowerDefaultArgument(
                  argument,
                  declarationDefault && defaults ? defaults->model : model,
                  declarationDefault ? target.arguments : classArguments,
                  declarationDefault ? target.valueArguments
                                     : classValueArguments,
                  body)) {
            operands.push_back(*lowered);
          } else {
            lifecycleValid = false;
          }
        }
      } else {
        for (const ExprPtr &argument : initializer->arguments()) {
          lowerOperand(argument);
        }
      }
    } else if (const auto *set = exprAs<DereferenceSet>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*set);
      if (resolved != nullptr && isAssignmentOperator(resolved->kind)) {
        kind = HirValueKind::Call;
        lowerOperand(set->object());
        if (!operands.empty()) {
          HirValue target{
              .id = nextValueId++,
              .kind = HirValueKind::Unary,
              .source = raw,
              .info = ExpressionInfo{.type = resolved->dispatchOwner,
                                     .category = ValueCategory::Place,
                                     .access = AccessMode::Mutable,
                                     .traits = semanticTraits(
                                         resolved->dispatchOwner)},
              .unsafeOperation = model.unsafeOperation(*raw),
              .operands = {operands.front()},
              .operation = TokenKind::STAR,
          };
          if (const PlaceKey *place = model.findPlace(*raw)) {
            target.place = qualifyPlace(*place, body.placeDomain);
          }
          if (const ResolvedOperatorInfo *projection =
                  model.findProjectionOperator(*set)) {
            configureOperatorCall(target, *projection, body);
          }
          const HirValueId targetId = target.id;
          body.values.push_back(std::move(target));
          operands = {targetId};
          receiver = targetId;
        }
        lowerOperand(set->value());
      } else {
        kind = HirValueKind::DereferenceSet;
        operation = set->oper().kind;
        lowerOperand(set->object());
        lowerOperand(set->value());
      }
    } else if (const auto *set = exprAs<PlaceSet>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*set);
      lowerOperand(set->target());
      if (resolved != nullptr && isAssignmentOperator(resolved->kind)) {
        kind = HirValueKind::Call;
        if (!operands.empty())
          receiver = operands.front();
      } else {
        kind = HirValueKind::PlaceSet;
        operation = set->oper().kind;
      }
      lowerOperand(set->value());
    } else if (const auto *get = exprAs<Get>(raw)) {
      kind = HirValueKind::MemberAccess;
      operation = get->access().kind;
      lowerOperand(get->object());
    } else if (const auto *grouping = exprAs<Grouping>(raw)) {
      kind = HirValueKind::Grouping;
      lowerOperand(grouping->expression());
    } else if (const auto *index = exprAs<Index>(raw)) {
      kind = HirValueKind::Index;
      lowerOperand(index->object());
      lowerOperand(index->index());
    } else if (const auto *set = exprAs<IndexSet>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*set);
      if (resolved != nullptr && isAssignmentOperator(resolved->kind)) {
        kind = HirValueKind::Call;
        lowerOperand(set->object());
        lowerOperand(set->index());
        if (operands.size() == 2) {
          HirValue target{
              .id = nextValueId++,
              .kind = HirValueKind::Index,
              .source = raw,
              .info = ExpressionInfo{.type = resolved->dispatchOwner,
                                     .category = ValueCategory::Place,
                                     .access = AccessMode::Mutable,
                                     .traits = semanticTraits(
                                         resolved->dispatchOwner)},
              .unsafeOperation = model.unsafeOperation(*raw),
              .operands = operands,
          };
          if (const PlaceKey *place = model.findPlace(*raw)) {
            target.place = qualifyPlace(*place, body.placeDomain);
          }
          if (const ResolvedOperatorInfo *projection =
                  model.findProjectionOperator(*set)) {
            configureOperatorCall(target, *projection, body);
          }
          const HirValueId targetId = target.id;
          body.values.push_back(std::move(target));
          operands = {targetId};
          receiver = targetId;
        }
        lowerOperand(set->value());
      } else {
        kind = HirValueKind::IndexSet;
        operation = set->oper().kind;
        lowerOperand(set->object());
        lowerOperand(set->index());
        lowerOperand(set->value());
      }
    } else if (const auto *lambda = exprAs<Lambda>(raw)) {
      kind = HirValueKind::Lambda;
      const HirLambdaId target =
          lowerLambda(*lambda, model, classArguments, classValueArguments);
      if (target != 0) {
        lambdaTarget = target;
      }
      for (const LambdaCapture &capture : lambda->captures()) {
        lowerOperand(capture.initializer);
      }
    } else if (exprAs<MemberRequirement>(raw) != nullptr) {
      kind = HirValueKind::Literal;
      if (const auto constant = model.findConstant(*raw)) {
        if (const auto *value = std::get_if<bool>(&*constant))
          literal = Literal{*value};
      }
    } else if (exprAs<LayoutQuery>(raw) != nullptr) {
      kind = HirValueKind::LayoutQuery;
      if (const std::optional<ConstantValue> constant =
              model.findConstant(*raw)) {
        if (const auto *integer = std::get_if<ConstantInteger>(&*constant);
            integer != nullptr && !integer->negative) {
          literal = Literal{integer->magnitude};
        }
      }
    } else if (const auto *literalExpression = exprAs<LiteralExpr>(raw)) {
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
    } else if (const auto *logical = exprAs<Logical>(raw)) {
      kind = HirValueKind::Logical;
      operation = logical->oper().kind;
      lowerOperand(logical->left());
      lowerOperand(logical->right());
    } else if (const auto *fold = exprAs<PackFold>(raw)) {
      kind = HirValueKind::PackFold;
      if (const auto *pattern = exprAs<Call>(fold->pattern().get())) {
        for (const ExprPtr &argument : pattern->arguments()) {
          lowerOperand(argument);
        }
      }
    } else if (exprAs<PackExpansion>(raw) != nullptr) {
      kind = HirValueKind::PackExpansion;
    } else if (const auto *postfix = exprAs<Postfix>(raw)) {
      kind = HirValueKind::Postfix;
      operation = postfix->oper().kind;
      lowerOperand(postfix->expression());
    } else if (const auto *qualified = exprAs<QualifiedName>(raw)) {
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
    } else if (exprAs<This>(raw) != nullptr) {
      kind = HirValueKind::This;
    } else if (const auto *set = exprAs<Set>(raw)) {
      const ResolvedOperatorInfo *resolved = model.findOperator(*set);
      if (resolved != nullptr && isAssignmentOperator(resolved->kind)) {
        kind = HirValueKind::Call;
        lowerOperand(set->object());
        if (!operands.empty()) {
          HirValue target{
              .id = nextValueId++,
              .kind = HirValueKind::MemberAccess,
              .source = raw,
              .info = ExpressionInfo{.type = resolved->dispatchOwner,
                                     .category = ValueCategory::Place,
                                     .access = AccessMode::Mutable,
                                     .traits = semanticTraits(
                                         resolved->dispatchOwner)},
              .unsafeOperation = model.unsafeOperation(*raw),
              .symbol = model.findResolvedSymbol(*raw),
              .operands = {operands.front()},
              .operation = set->access().kind,
          };
          HirValueId targetId = target.id;
          if (const ResolvedOperatorInfo *projection =
                  model.findProjectionOperator(*set)) {
            const ExpressionInfo projectedFieldInfo = target.info;
            configureOperatorCall(target, *projection, body);
            target.info = projectedFieldInfo;
            targetId = splitOverloadedArrowProjection(std::move(target),
                                                      *projection, body);
          } else {
            if (const PlaceKey *place = model.findPlace(*raw)) {
              target.place = qualifyPlace(*place, body.placeDomain);
            }
            body.values.push_back(std::move(target));
          }
          operands = {targetId};
          receiver = targetId;
        }
        lowerOperand(set->value());
      } else {
        kind = HirValueKind::MemberSet;
        operation = set->oper().kind;
        lowerOperand(set->object());
        lowerOperand(set->value());
      }
    } else if (const auto *unary = exprAs<Unary>(raw)) {
      const SemanticType *rangeType =
          unary->rangeObject() ? model.findType(*unary->rangeObject())
                               : nullptr;
      const SemanticType *concreteRangeType = rangeType;
      if (concreteRangeType != nullptr &&
          concreteRangeType->kind == SemanticType::Reference &&
          concreteRangeType->arguments.size() == 1) {
        concreteRangeType = &concreteRangeType->arguments.front();
      }
      if (concreteRangeType != nullptr &&
          concreteRangeType->kind == SemanticType::Array) {
        kind = HirValueKind::Index;
        lowerOperand(unary->rangeObject());
        lowerOperand(unary->right());
      } else {
        kind = HirValueKind::Unary;
        operation = unary->oper().kind;
        lowerOperand(unary->right());
      }
    } else if (const auto *unexpected = exprAs<Unexpected>(raw)) {
      kind = HirValueKind::Unexpected;
      lowerOperand(unexpected->error());
    } else if (exprAs<Variable>(raw) != nullptr) {
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
    if (const FunctionItemSelectionInfo *selection =
            model.findFunctionItemSelection(*raw)) {
      kind = HirValueKind::FunctionItem;
      if (const FunctionInfo *target =
              baseModel->findFunction(selection->function)) {
        const HirFunctionInstanceId instance =
            enqueueFunction(*target, {}, {}, {}, {}, target->returnType,
                            target->parameterTypes);
        if (instance != 0) {
          functionItemTarget = instance;
        }
      }
      if (!functionItemTarget) {
        lifecycleValid = false;
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
                   .functionTarget = functionItemTarget,
                   .nativeCallbackAdapter = nativeCallbackAdapter,
                   .threadTaskAdapter = threadTaskAdapter,
                   .lambdaTarget = lambdaTarget,
                   .enumOwner = enumOwner,
                   .enumValue = enumValue,
                   .enumVariant = enumVariant,
                   .payloadIndex = payloadIndex};
    if (const auto *call = exprAs<Call>(raw);
        call != nullptr && exprAs<Get>(call->callee().get()) != nullptr &&
        model.typeOf(*call->callee()).kind != SemanticType::FunctionItem &&
        (model.findCall(*call) == nullptr ||
         model.findCall(*call)->argumentReceiver == nullptr) &&
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
    if (const auto *assertion = model.findRawReferenceAssertion(*raw)) {
      const auto *dereference = exprAs<Unary>(raw);
      if (!dereference || value.operands.size() != 1 ||
          assertion->ownerReference.kind != SemanticType::Reference ||
          assertion->ownerReference.arguments.size() != 1) {
        lifecycleValid = false;
      } else {
        const auto access = assertion->ownerReference.referenceAccess;
        HirValueId witness = 0;
        if (assertion->owner.receiver) {
          witness = lowerImplicitReceiver(body);
        } else {
          witness = nextValueId++;
          const auto &owner = assertion->ownerReference.arguments.front();
          body.values.push_back(
              {.id = witness,
               .kind = HirValueKind::Variable,
               .info = {.type = owner,
                        .category = ValueCategory::Place,
                        .access = access,
                        .traits = instanceProvider->traitsFor(owner)},
               .symbol = assertion->owner.root,
               .place = qualifyPlace(assertion->owner, body.placeDomain)});
        }
        value.kind = kind = HirValueKind::Call;
        value.operation.reset();
        value.place.reset();
        value.intrinsic = access == AccessMode::Mutable
                              ? IntrinsicKind::RawBorrowMut
                              : IntrinsicKind::RawBorrow;
        value.parameterTypes = {model.typeOf(*dereference->right()),
                                assertion->ownerReference};
        value.operands.push_back(witness);
        value.borrowOrigin = BorrowOriginKind::Argument;
        value.borrowArgument = 1;
        value.borrowAccess = access;
        value.info.access = access;
      }
    }
    if (value.kind == HirValueKind::Grouping && value.operands.size() == 1) {
      if (const auto *inner = body.findValue(value.operands.front())) {
        value.retainedReceiverProjection = inner->retainedReceiverProjection;
        value.retainedReceiverOwner = inner->retainedReceiverOwner;
      }
    }
    if (kind == HirValueKind::PackExpansion) {
      if (value.info.type.kind != SemanticType::TypePack ||
          !value.info.type.concretePack || value.symbol == 0) {
        lifecycleValid = false;
      } else {
        value.packExpansionElements.reserve(value.info.type.arguments.size());
        for (std::size_t index = 0; index < value.info.type.arguments.size();
             ++index) {
          const SemanticType &element = value.info.type.arguments[index];
          const SemanticTypeTraits traits =
              instanceProvider->traitsFor(element);
          const bool storedEntry =
              element.kind == SemanticType::Class &&
              std::any_of(model.loans().begin(), model.loans().end(),
                          [&](const SemanticLoanInfo &loan) {
                            return loan.entry &&
                                   loan.kind == SemanticLoanKind::StoredValue &&
                                   loan.place.root == value.symbol &&
                                   !loan.place.receiver &&
                                   loan.place.projections.size() == 1 &&
                                   loan.place.projections.front().kind ==
                                       PlaceProjectionKind::ConstantIndex &&
                                   loan.place.projections.front().index ==
                                       index &&
                                   !loan.designatedFields.empty();
                          });
          HirCallInputKind inputKind = HirCallInputKind::Value;
          if (element.kind == SemanticType::Reference) {
            inputKind = element.referenceAccess == AccessMode::Mutable
                            ? HirCallInputKind::MutableBorrow
                            : HirCallInputKind::ReadBorrow;
          } else if (orderedValueParameterType(element)) {
            inputKind = HirCallInputKind::Value;
          } else if ((element.kind == SemanticType::Class ||
                      element.kind == SemanticType::Lambda) &&
                     (!traits.containsBorrowedState || storedEntry) &&
                     traits.copyable) {
            inputKind = HirCallInputKind::CopyValue;
          } else if ((element.kind == SemanticType::Class ||
                      element.kind == SemanticType::Lambda) &&
                     (!traits.containsBorrowedState || storedEntry) &&
                     traits.movable) {
            inputKind = HirCallInputKind::MoveValue;
          } else {
            lifecycleValid = false;
          }
          value.packExpansionElements.push_back(
              {.type = element, .traits = traits, .kind = inputKind});
        }
      }
    }
    const ResolvedOperatorInfo *resolvedValueOperator =
        model.findOperator(*raw);
    if (resolvedValueOperator == nullptr &&
        (value.kind == HirValueKind::DereferenceSet ||
         value.kind == HirValueKind::IndexSet)) {
      // A built-in compound update still has a separately selected target
      // accessor. Preserve that exact call instead of manufacturing a raw
      // dereference/index projection of the accessor's class receiver.
      resolvedValueOperator = model.findProjectionOperator(*raw);
    }
    if (resolvedValueOperator == nullptr &&
        value.kind == HirValueKind::MemberSet) {
      const ResolvedOperatorInfo *projection =
          model.findProjectionOperator(*raw);
      if (projection != nullptr &&
          projection->kind == OverloadedOperator::Arrow) {
        // Scalar compound assignment has no overloaded mutation operator.
        // Its separately resolved arrow still owns evaluation of the target.
        resolvedValueOperator = projection;
      }
    }
    const bool loweredAssignmentCall =
        value.kind == HirValueKind::Call && resolvedValueOperator != nullptr &&
        isAssignmentOperator(resolvedValueOperator->kind);
    if (!loweredAssignmentCall && !model.findRawReferenceAssertion(*raw)) {
      if (const PlaceKey *place = model.findPlace(*raw)) {
        value.place = qualifyPlace(*place, body.placeDomain);
      }
    }
    if (const OwnershipEvent *event = model.findOwnershipEvent(*raw)) {
      value.ownership = qualifyOwnershipEvent(*event, body.placeDomain);
      if (!value.place) {
        value.place = value.ownership->place;
      }
    }
    if ((value.kind == HirValueKind::Variable ||
         value.kind == HirValueKind::Assignment) &&
        value.place && value.place->receiver && value.symbol != 0) {
      // Preserve the resolved meaning of an unqualified instance field in
      // HIR. It is the same receiver projection as `this.field`, even though
      // the source omitted the explicit receiver.
      kind = value.kind == HirValueKind::Assignment
                 ? HirValueKind::MemberSet
                 : HirValueKind::MemberAccess;
      value.kind = kind;
      value.operands.insert(value.operands.begin(),
                            lowerImplicitReceiver(body));
    }
    if (const auto *call = exprAs<Call>(raw)) {
      if (const ResolvedCallInfo *resolved = model.findCall(*call)) {
        value.nativeCalleeType = resolved->nativeCalleeType;
        value.callableBoundary = resolved->callableBoundary;
        if (value.nativeCalleeType)
          value.callableInvocation = CallableInvocationCapability::Read;
        value.intrinsic = resolved->intrinsic;
        if ((value.intrinsic == IntrinsicKind::ObjectDestroy ||
             value.intrinsic == IntrinsicKind::ObjectRelocate ||
             value.intrinsic == IntrinsicKind::ObjectRelocateRange ||
             value.intrinsic == IntrinsicKind::ObjectConstruct) &&
            resolved->typeArguments.size() == 1)
          (void)seedConcreteDeclarationType(resolved->typeArguments.front());
        if ((value.intrinsic == IntrinsicKind::ObjectSize ||
             value.intrinsic == IntrinsicKind::ObjectAlignment) &&
            resolved->typeArguments.size() == 1) {
          value.objectLayoutType = resolved->typeArguments.front();
          (void)seedConcreteDeclarationType(*value.objectLayoutType);
        }
        value.neverReturns = resolved->neverReturns;
        value.tracksCaller = resolved->tracksCaller;
        if (sourceLocationIntrinsic(value.intrinsic)) {
          value.sourceLocation = activeDefaultSourceLocation
                                     ? activeDefaultSourceLocation
                                     : std::optional<SourceLocationLiteral>(
                                           sourceLocationAt(*call));
        }
        value.retainedReceiverProjection = resolved->retainedReceiverProjection;
        if (resolved->retainedReceiverOwner != nullptr) {
          const auto anchor = std::find_if(
              body.values.rbegin(), body.values.rend(), [&](const HirValue &v) {
                return v.source == resolved->retainedReceiverOwner;
              });
          if (anchor == body.values.rend()) {
            lifecycleValid = false;
          } else {
            value.retainedReceiverOwner = anchor->id;
          }
        }
        value.dispatch = resolved->dispatch;
        value.dispatchOwner = resolved->dispatchOwner;
        value.parameterTypes = resolved->parameterTypes;
        value.borrowOrigin = resolved->borrowOrigin;
        value.borrowArgument = resolved->borrowArgument;
        value.borrowArguments = resolved->borrowArguments;
        value.borrowFields = resolved->borrowFields;
        value.borrowAccess = resolved->borrowAccess;
        value.borrowPlace = resolved->borrowPlace;
        if (value.borrowOrigin == BorrowOriginKind::Global &&
            value.borrowPlace) {
          ensurePlaceDomain(body);
          value.place =
              qualifyBorrowPlace(*value.borrowPlace, body.placeDomain);
        }
        value.callableArguments = resolved->callableArguments;
        if (resolved->intrinsic == IntrinsicKind::ThreadScope) {
          ensurePlaceDomain(body);
          if (resolved->scopedThreadTasks.size() !=
              resolved->parameterTypes.size())
            lifecycleValid = false;
          for (std::size_t index = 0;
               index < resolved->scopedThreadTasks.size(); ++index) {
            const auto &task = resolved->scopedThreadTasks[index];
            const auto &taskType = task.shape.taskType;
            const auto &entry = task.shape.entry;
            std::optional<HirFunctionInstanceId> functionTarget;
            std::optional<HirLambdaId> lambdaTarget;
            if (entry.function != 0 && entry.lambda == 0) {
              if (const auto *target =
                      baseModel->findFunction(entry.function)) {
                const auto instance =
                    enqueueFunction(*target,
                                    taskType.kind == SemanticType::Class
                                        ? taskType.arguments
                                        : std::vector<SemanticType>{},
                                    taskType.kind == SemanticType::Class
                                        ? taskType.valueArguments
                                        : std::vector<CompileTimeValue>{},
                                    {}, {}, SemanticType::Void, {});
                if (instance != 0)
                  functionTarget = instance;
              }
            } else if (entry.lambda != 0 && entry.function == 0) {
              if (const auto target = lambdaTargets.find(entry.lambda);
                  target != lambdaTargets.end())
                lambdaTarget = target->second;
            }
            if (functionTarget.has_value() == lambdaTarget.has_value()) {
              lifecycleValid = false;
              continue;
            }
            HirThreadTaskAdapter adapter{
                .id = output.program.threadTasks.size() + 1,
                .taskType = taskType,
                .spawnValue = value.id,
                .functionTarget = functionTarget,
                .lambdaTarget = lambdaTarget,
                .targetInvocation = entry.capability,
                .invocation = CallableInvocationCapability::Once,
                .scoped = true,
                .scopeArgument = index,
                .scopeDependencies = task.dependencies};
            for (auto &dependency : adapter.scopeDependencies)
              dependency.place =
                  qualifyPlace(dependency.place, body.placeDomain);
            value.scopedThreadTasks.push_back(adapter.id);
            output.program.threadTasks.push_back(std::move(adapter));
          }
          value.synchronization.kind =
              SynchronizationOperationKind::ThreadScope;
        }
        if (resolved->intrinsic == IntrinsicKind::ThreadSpawn &&
            resolved->parameterTypes.size() == 2 &&
            resolved->parameterTypes[1] != SemanticType::Unknown &&
            resolved->threadTaskEntry) {
          const SemanticType &taskType = resolved->parameterTypes[1];
          const SemanticType &handleReference = resolved->parameterTypes[0];
          const SemanticType *handleType =
              handleReference.kind == SemanticType::Reference &&
                      handleReference.arguments.size() == 1
                  ? &handleReference.arguments.front()
                  : nullptr;
          const SemanticType resultType =
              handleType != nullptr && handleType->arguments.size() == 1
                  ? handleType->arguments.front()
                  : SemanticType::Void;
          const ResolvedThreadTaskEntry &entry = *resolved->threadTaskEntry;
          std::optional<HirFunctionInstanceId> functionTarget;
          std::optional<HirLambdaId> lambdaTarget;
          if (entry.function != 0 && entry.lambda == 0 &&
              (taskType.kind == SemanticType::Class ||
               taskType.kind == SemanticType::FunctionItem)) {
            if (const FunctionInfo *target =
                    baseModel->findFunction(entry.function)) {
              const HirFunctionInstanceId instance =
                  enqueueFunction(*target,
                                  taskType.kind == SemanticType::Class
                                      ? taskType.arguments
                                      : std::vector<SemanticType>{},
                                  taskType.kind == SemanticType::Class
                                      ? taskType.valueArguments
                                      : std::vector<CompileTimeValue>{},
                                  {}, {}, resultType, {});
              if (instance != 0) {
                functionTarget = instance;
              }
            }
          } else if (entry.lambda != 0 && entry.function == 0 &&
                     taskType.hasLambdaShape()) {
            if (const auto target = lambdaTargets.find(entry.lambda);
                target != lambdaTargets.end()) {
              lambdaTarget = target->second;
            }
          }
          if (functionTarget.has_value() == lambdaTarget.has_value()) {
            lifecycleValid = false;
          } else {
            const HirThreadTaskAdapterId adapter =
                output.program.threadTasks.size() + 1;
            value.threadTaskAdapter = adapter;
            output.program.threadTasks.push_back(
                {.id = adapter,
                 .taskType = taskType,
                 .spawnValue = value.id,
                 .functionTarget = functionTarget,
                 .lambdaTarget = lambdaTarget,
                 .targetInvocation = entry.capability,
                 .invocation = CallableInvocationCapability::Once});
          }
        } else if (resolved->intrinsic == IntrinsicKind::ThreadSpawn &&
                   resolved->parameterTypes.size() == 2 &&
                   resolved->parameterTypes[1].kind !=
                       SemanticType::TypeParameter) {
          lifecycleValid = false;
        }
        if (resolved->intrinsic == IntrinsicKind::ThreadSpawn) {
          value.synchronization.kind =
              SynchronizationOperationKind::ThreadSpawn;
        } else if (resolved->intrinsic == IntrinsicKind::ThreadJoin ||
                   resolved->intrinsic == IntrinsicKind::ThreadAutoJoin) {
          value.synchronization.kind = SynchronizationOperationKind::ThreadJoin;
        } else if (resolved->intrinsic == IntrinsicKind::MutexLock ||
                   resolved->intrinsic == IntrinsicKind::MutexUnlock) {
          value.synchronization = resolved->synchronization;
          const SynchronizationOperationKind expected =
              resolved->intrinsic == IntrinsicKind::MutexLock
                  ? SynchronizationOperationKind::MutexLock
                  : SynchronizationOperationKind::MutexUnlock;
          if (value.synchronization.kind != expected ||
              !validSynchronizationOperation(value.synchronization)) {
            lifecycleValid = false;
          }
        } else if (resolved->intrinsic == IntrinsicKind::AtomicLoad ||
                   resolved->intrinsic == IntrinsicKind::AtomicWait ||
                   resolved->intrinsic == IntrinsicKind::AtomicNotifyOne ||
                   resolved->intrinsic == IntrinsicKind::AtomicNotifyAll ||
                   resolved->intrinsic == IntrinsicKind::AtomicStore ||
                   resolved->intrinsic == IntrinsicKind::AtomicExchange ||
                   resolved->intrinsic ==
                       IntrinsicKind::AtomicCompareExchange) {
          value.synchronization = resolved->synchronization;
          const SynchronizationOperationKind expected =
              resolved->intrinsic == IntrinsicKind::AtomicLoad
                  ? SynchronizationOperationKind::AtomicLoad
              : resolved->intrinsic == IntrinsicKind::AtomicWait
                  ? SynchronizationOperationKind::AtomicWait
              : resolved->intrinsic == IntrinsicKind::AtomicNotifyOne ||
                      resolved->intrinsic == IntrinsicKind::AtomicNotifyAll
                  ? SynchronizationOperationKind::AtomicNotify
              : resolved->intrinsic == IntrinsicKind::AtomicStore
                  ? SynchronizationOperationKind::AtomicStore
              : resolved->intrinsic == IntrinsicKind::AtomicExchange
                  ? SynchronizationOperationKind::AtomicReadModifyWrite
                  : SynchronizationOperationKind::AtomicCompareExchange;
          if (value.synchronization.kind != expected ||
              !validSynchronizationOperation(value.synchronization)) {
            lifecycleValid = false;
          }
        }
        if (resolved->intrinsic == IntrinsicKind::None &&
            resolved->function != 0 && resolved->declaration != nullptr) {
          value.synchronization = resolved->synchronization;
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
                callInstantiationSite(*call));
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
                    targetClassValueArguments, body, sourceLocationAt(*call))) {
              value.operands.push_back(*lowered);
            }
          }
        }
      }
      if (const ResolvedLambdaCallInfo *resolved =
              model.findLambdaCall(*call)) {
        value.parameterTypes = resolved->parameterTypes;
        // The callee is an explicit receiver even when a final pack prevents
        // formation of the fixed-arity call plan. MIR stages it before the
        // expanded arguments rather than losing the closure environment.
        if (value.operands.size() == call->arguments().size() + 1) {
          // Grouping has no evaluation or ownership effect. Preserve the
          // exact callable producer rather than creating a second closure
          // value merely to invoke a parenthesized lambda literal.
          const HirValue *callee = body.findValue(value.operands.front());
          while (callee != nullptr && callee->kind == HirValueKind::Grouping &&
                 callee->operands.size() == 1) {
            value.operands.front() = callee->operands.front();
            callee = body.findValue(value.operands.front());
          }
          value.receiver = value.operands.front();
        }
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
          model.findOperator(*call) == nullptr &&
          model.findCall(*call) == nullptr) {
        if (const DeferredCallableCallInfo *deferred =
                model.findDeferredCallableCall(*call)) {
          value.parameterTypes = deferred->parameterTypes;
          value.callableBoundary = deferred->boundary;
          value.callableInvocation = deferred->capability;
        }
      }
    }
    if (const auto *fold = exprAs<PackFold>(raw)) {
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
      value.designatedConstruction = !construction->designatedArguments.empty();
      const bool inPlaceStorageConstruction =
          value.intrinsic == IntrinsicKind::ObjectConstruct ||
          value.intrinsic == IntrinsicKind::OptionalStorageConstruct;
      if (!inPlaceStorageConstruction) {
        value.parameterTypes = construction->parameterTypes;
      }
      value.borrowOrigin = construction->borrowOrigin;
      value.borrowArgument = construction->borrowArgument;
      value.borrowArguments = construction->borrowArguments;
      value.borrowFields = construction->borrowFields;
      value.borrowAccess = construction->borrowAccess;
      value.constructorKind = construction->kind;
      std::optional<SourceSpan> site;
      if (const auto *call = exprAs<Call>(raw)) {
        site = callInstantiationSite(*call);
      } else if (const auto *initializer = exprAs<DirectInitializer>(raw)) {
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
                    targetClassValueArguments, body, sourceLocationAt(*raw))) {
              value.operands.push_back(*lowered);
            }
          }
        }
      }
      if (value.intrinsic == IntrinsicKind::AllocateUniqueOwner &&
          construction->kind == ConstructorKind::Move &&
          !value.constructorTarget &&
          construction->constructedType.kind == SemanticType::Class &&
          value.parameterTypes.size() == 1) {
        // A generated move has no callable reference formal. Its selected
        // operation consumes one same-type value into allocated storage;
        // preserve that transfer role when the source pack expands in MIR.
        value.parameterTypes = {construction->constructedType};
      }
    }
    if ((kind == HirValueKind::MemberAccess ||
         kind == HirValueKind::MemberSet) &&
        value.symbol != 0) {
      const SymbolRecord *member = model.database().findSymbol(value.symbol);
      value.storedReferenceAccess =
          member != nullptr && member->type.kind == SemanticType::Reference;
    }
    if (const ResolvedOperatorInfo *resolved = resolvedValueOperator;
        resolved != nullptr && resolved->function != 0) {
      value.parameterTypes = resolved->parameterTypes;
      value.dispatch = resolved->dispatch;
      value.dispatchOwner = resolved->dispatchOwner;
      value.callableBoundary = resolved->boundary;
      if (resolved->kind == OverloadedOperator::Call ||
          resolved->receiverMode == CallReceiverMode::Consuming) {
        value.callableInvocation = resolved->capability;
      }
      value.borrowOrigin = resolved->borrowOrigin;
      value.borrowArgument = resolved->borrowArgument;
      value.borrowAccess = resolved->borrowAccess;
      value.borrowArguments = resolved->borrowArguments;
      value.borrowFields = resolved->borrowFields;
      if ((resolved->kind == OverloadedOperator::Call ||
           isAssignmentOperator(resolved->kind)) &&
          !value.operands.empty()) {
        value.receiver = value.operands.front();
      }
      if (const FunctionInfo *target =
              baseModel->findFunction(resolved->function)) {
        SemanticType receiverType = resolved->dispatchOwner;
        if (const auto *binary = exprAs<Binary>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*binary->left());
          }
        } else if (const auto *get = exprAs<Get>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*get->object());
          }
        } else if (const auto *index = exprAs<Index>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*index->object());
          }
        } else if (const auto *call = exprAs<Call>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*call->callee());
          }
        } else if (const auto *unary = exprAs<Unary>(raw)) {
          if (receiverType.kind != SemanticType::Class ||
              receiverType.classId != target->ownerClass) {
            receiverType = model.typeOf(*unary->right());
          }
        } else if (exprAs<Assign>(raw) != nullptr) {
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
    // A function target is this instance's exact selection. A generic
    // callable call inherits its deferred record from the primary analysis,
    // but once the instance selects operator() or a function item, that
    // signature's argument conversions are planned like any ordinary call.
    if (const auto *call = exprAs<Call>(raw);
        call != nullptr && kind == HirValueKind::Call &&
        value.intrinsic == IntrinsicKind::None && !value.constructorTarget &&
        value.functionTarget &&
        (model.findOperator(*call) == nullptr ||
         model.findOperator(*call)->kind == OverloadedOperator::Call) &&
        model.findLambdaCall(*call) == nullptr &&
        value.parameterTypes.size() >=
            resolvedCallArgumentCount(*call, model.findCall(*call)) &&
        value.parameterTypes.size() <= value.operands.size()) {
      const std::size_t argumentCount = value.parameterTypes.size();
      const bool exactOperands = argumentCount <= value.operands.size();
      std::vector<HirValueId> arguments;
      if (exactOperands) {
        arguments.assign(value.operands.end() -
                             static_cast<std::ptrdiff_t>(argumentCount),
                         value.operands.end());
      }
      const ResolvedCallInfo *resolved = model.findCall(*call);
      const std::span<const std::size_t> ephemeralBorrowArguments =
          resolved == nullptr ? std::span<const std::size_t>{}
                              : std::span<const std::size_t>{
                                    resolved->ephemeralBorrowArguments};
      const std::optional<std::vector<HirCallArgument>> plannedArguments =
          exactOperands
              ? orderedArguments(arguments, value.parameterTypes, body,
                                 resolvedCallArgumentCount(*call, resolved),
                                 ephemeralBorrowArguments)
              : std::nullopt;
      const ResolvedOperatorInfo *resolvedOperator = model.findOperator(*call);
      const FunctionInfo *resolvedTarget =
          resolved == nullptr || resolved->function == 0
              ? nullptr
              : baseModel->findFunction(resolved->function);
      std::optional<HirValueId> callReceiver;
      ReceiverMutability receiverMutability = ReceiverMutability::ReadOnly;
      CallReceiverMode receiverMode = CallReceiverMode::None;
      bool supportedReceiver = true;
      if (resolvedTarget != nullptr && resolved != nullptr &&
          resolved->declaration != nullptr &&
          resolved->receiverMode == CallReceiverMode::Consuming) {
        value.callableInvocation = callableInvocationCapability(
            resolved->declaration->receiverMutability());
      }
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
          callReceiver =
              callee->unsafeOperation == UnsafeOperationKind::RawMember
                  ? callee->id
                  : callee->operands.front();
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
    if (value.nativeCalleeType && !value.operands.empty()) {
      value.receiver = value.operands.front();
      const std::vector<HirValueId> arguments(value.operands.begin() + 1,
                                              value.operands.end());
      const bool expandsPack =
          std::any_of(arguments.begin(), arguments.end(), [&](HirValueId id) {
            const HirValue *argument = body.findValue(id);
            return argument && argument->kind == HirValueKind::PackExpansion;
          });
      if (expandsPack) {
        // MIR's shared pack schedule stages this value receiver before
        // expanding the concrete trailing argument pack.
      } else if (auto planned = orderedArguments(
                     arguments, value.parameterTypes, body, arguments.size())) {
        value.callPlan = HirCallPlan{
            .receiver = HirCallReceiver{.value = *value.receiver,
                                        .kind = HirCallInputKind::Value,
                                        .type = *value.nativeCalleeType},
            .arguments = std::move(*planned)};
      } else {
        lifecycleValid = false;
      }
    }
    // A fixed-arity lambda invocation is an ordinary ordered call. Its closure
    // is the explicit receiver, and the lambda's selected signature plans
    // each argument, so a derived class object for a base-reference
    // parameter is the same staged borrow as in a direct function call. A
    // closure place is read-borrowed, an explicitly moved environment with
    // capture cleanup is a MoveValue stage, and a retained field is consumed
    // exactly once. A final pack expansion keeps MIR's shared pack schedule.
    if (value.lambdaTarget && value.receiver && !value.callPlan &&
        value.operands.size() == value.parameterTypes.size() + 1) {
      const HirValue *receiver = body.findValue(*value.receiver);
      const std::vector<HirValueId> arguments(value.operands.begin() + 1,
                                              value.operands.end());
      const bool expandsPack =
          std::any_of(arguments.begin(), arguments.end(), [&](HirValueId id) {
            const HirValue *argument = body.findValue(id);
            return argument && argument->kind == HirValueKind::PackExpansion;
          });
      if (receiver != nullptr && !expandsPack) {
        const auto planned = orderedArguments(arguments, value.parameterTypes,
                                              body, arguments.size());
        const auto receiverPlan = orderedReceiver(
            *value.receiver, ReceiverMutability::ReadOnly,
            receiver->retainedReceiverProjection ? CallReceiverMode::Consuming
                                                 : CallReceiverMode::Value,
            body);
        if (planned && receiverPlan) {
          value.callPlan =
              HirCallPlan{.receiver = receiverPlan, .arguments = *planned};
        }
      }
    }
    if ((value.intrinsic == IntrinsicKind::TextViewFromAddress ||
         value.intrinsic == IntrinsicKind::CStringFromAddress) &&
        !value.parameterTypes.empty() &&
        value.operands.size() >= value.parameterTypes.size()) {
      // A raw text carrier assertion has ordinary ordered scalar/reference
      // inputs. Keep the checked witness in the same exact schedule as its
      // address and count instead of inventing backend-only origin recovery.
      const std::vector<HirValueId> arguments(
          value.operands.end() -
              static_cast<std::ptrdiff_t>(value.parameterTypes.size()),
          value.operands.end());
      if (auto planned =
              orderedArguments(arguments, value.parameterTypes, body))
        value.callPlan = HirCallPlan{.receiver = std::nullopt,
                                     .arguments = std::move(*planned)};
      else
        lifecycleValid = false;
    }
    if (value.intrinsic == IntrinsicKind::ObjectConstruct &&
        ((value.constructorTarget &&
          value.constructorKind == ConstructorKind::Ordinary) ||
         (!value.constructorTarget &&
          (value.constructorKind == ConstructorKind::Copy ||
           value.constructorKind == ConstructorKind::Move))) &&
        !value.parameterTypes.empty() &&
        value.operands.size() >= value.parameterTypes.size()) {
      // Intrinsics need not retain a callable-expression operand. Select the
      // exact signature-sized argument suffix, including storage-only calls.
      // Generated copy/move transport needs the same staged ownership as a
      // concrete pack: a native temporary must not own failure cleanup.
      const std::vector<HirValueId> arguments(
          value.operands.end() -
              static_cast<std::ptrdiff_t>(value.parameterTypes.size()),
          value.operands.end());
      std::vector<SemanticType> roles = value.parameterTypes;
      if (auto planned = orderedArguments(arguments, roles, body)) {
        // Storage is a capability-place input, not a source reference formal.
        planned->front().parameterType = value.parameterTypes.front();
        value.callPlan = HirCallPlan{.receiver = std::nullopt,
                                     .arguments = std::move(*planned)};
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
          exprAs<Call>(raw) != nullptr ? exprAs<Call>(raw)->arguments().size()
          : exprAs<DirectInitializer>(raw) != nullptr &&
                  exprAs<DirectInitializer>(raw)->designators().empty()
              ? exprAs<DirectInitializer>(raw)->arguments().size()
              : value.parameterTypes.size();
      if (std::optional<std::vector<HirCallArgument>> plannedArguments =
              orderedConstructorArguments(value, arguments,
                                          value.parameterTypes, body,
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
           resolved->receiverMode == CallReceiverMode::FreshTemporaryMutable ||
           (resolved->receiverMode == CallReceiverMode::Consuming &&
            resolved->kind != OverloadedOperator::Call));
      const HirValue *receiver = body.findValue(value.operands.front());
      const bool borrowedValueReceiver =
          resolved != nullptr &&
          resolved->receiverMode == CallReceiverMode::Value &&
          resolved->receiverMutability == ReceiverMutability::ReadOnly &&
          receiver != nullptr &&
          receiver->info.category == ValueCategory::Value &&
          receiver->info.type.kind == SemanticType::Class &&
          receiver->info.traits.containsBorrowedState;
      if ((freshReceiver || borrowedValueReceiver) &&
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
    if (((value.kind == HirValueKind::MemberAccess &&
          value.operation == TokenKind::ARROW) ||
         value.kind == HirValueKind::MemberSet) &&
        value.functionTarget && !value.operands.empty()) {
      if (const ResolvedOperatorInfo *resolved = resolvedValueOperator;
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
  bool reachablePrefixRequested = false;
  // Top-level declaration range of the entry unit when lowering is scoped to
  // it; see `HirLoweringScope`.
  std::optional<std::pair<std::size_t, std::size_t>> entryDeclarations;
  // Declaration ranges lowered completely after a reachable prefix; see
  // `HirLoweringScope`.
  std::optional<std::vector<std::pair<std::size_t, std::size_t>>>
      completeDeclarations;
  // Library-only instance bodies are analysed but not lowered; see
  // `HirLoweringScope::omitLibraryOnlyBodies`.
  bool omitLibraryOnlyBodies = false;
  std::unordered_set<SourceUnitId> trustedUnits;
  SemanticInstanceProvider *instanceProvider = nullptr;
  const SemanticModel *baseModel = nullptr;
  HirLoweringResult output;
  HirValueId nextValueId = 1;
  HirBindingId nextBindingId = 1;
  HirStatementId nextStatementId = 1;
  std::size_t placeSnapshotId = 0;
  std::size_t nextPlaceBodyId = 1;
  bool lifecycleValid = true;
  bool constantClosure = false;
  std::size_t discoveryDepth = 0;
  std::optional<SourceSpan> discoverySite;
  // Diagnostic provenance only; this does not participate in instance identity,
  // discovery limits, overload selection, or executable HIR/MIR semantics.
  std::optional<SourceSpan> discoveryOrigin;
  bool discoveryLimitReached = false;
  std::size_t discoveredGenericInstances = 0;
  std::vector<InstanceDiscovery> classDiscoveryDepths;
  std::vector<InstanceDiscovery> functionDiscoveryDepths;
  std::vector<InstanceDiscovery> constructorDiscoveryDepths;
  std::vector<InstanceDiscovery> destructorDiscoveryDepths;
  std::vector<FunctionId> constantFunctions;
  std::vector<ConstructorId> constantConstructors;
  std::size_t processedClasses = 0;
  HirInstanceIndex instanceIndex;
  std::size_t processedFunctions = 0;
  std::size_t processedConstructors = 0;
  std::size_t processedDestructors = 0;
  SemanticType currentReceiverType = SemanticType::Unknown;
  AccessMode currentReceiverAccess = AccessMode::ReadOnly;
  std::string currentSourceFunctionName;
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
  std::optional<SourceLocationLiteral> activeDefaultSourceLocation;
  std::unordered_set<HirValueId> collectionResults;
};

HirLowerer::HirLowerer(TargetInfo target, HirLoweringScope scope)
    : impl(std::make_unique<Impl>(std::move(target), scope)) {}

HirLowerer::~HirLowerer() = default;
HirLowerer::HirLowerer(HirLowerer &&) noexcept = default;
HirLowerer &HirLowerer::operator=(HirLowerer &&) noexcept = default;

HirLoweringResult
HirLowerer::lower(const Program &source, const SemanticModel &semantics,
                  SemanticInstanceProvider &instanceProvider) {
  return impl->lower(source, semantics, instanceProvider);
}

HirLoweringResult HirLowerer::lowerConstantFunction(
    const SemanticModel &semantics, SemanticInstanceProvider &instanceProvider,
    FunctionId root, const std::vector<FunctionId> &completedFunctions,
    const GenericSubstitution &substitution,
    const std::vector<ConstructorId> &completedConstructors) {
  return impl->lowerConstantFunction(semantics, instanceProvider, root,
                                     completedFunctions, substitution,
                                     completedConstructors);
}

HirLoweringResult HirLowerer::lowerConstantConstructor(
    const SemanticModel &semantics, SemanticInstanceProvider &instanceProvider,
    const ResolvedConstructionInfo &root,
    const std::vector<FunctionId> &completedFunctions,
    const std::vector<ConstructorId> &completedConstructors) {
  return impl->lowerConstantConstructor(semantics, instanceProvider, root,
                                        completedFunctions,
                                        completedConstructors);
}

} // namespace lang
