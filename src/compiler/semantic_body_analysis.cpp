#include "semantic_analyzer_impl.h"

#include <iterator>

namespace lang {

auto SemanticVisitor::Impl::resetRequestState(const Program *program) -> void {
  diagnostics.clear();
  catalog.clear();
  valueFlow.clear();
  reportedCompilerPrivateAccess.clear();
  defaultArgumentSyntax.clear();
  defaultArgumentDependencies.clear();
  reportedDefaultArgumentCycles.clear();
  externCSymbols.clear();
  constexprDefinitionsAnalyzed.clear();
  constexprConstructorsAnalyzed.clear();
  requestSession.constexprWork.reset();
  requestSession.deferredFlowOwners.clear();
  genericDefaultConstants.clear();
  genericDefaultConstantUses.clear();
  requirementScopes.clear();
  requestSession.mutableLocalCandidates.clear();
  requestSession.mutableBindingUses.clear();
  requestSession.unsafeBlockCandidates.clear();
  requestSession.activeUnsafeBlocks.clear();
  requestSession.usedUnsafeBlocks.clear();
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  instanceTypeSubstitution.clear();
  instanceValueSubstitution.clear();
  nextLambdaId = 1;
  nextSemanticLoanId = 1;
  nextPlaceSelectionId = 1;
  instanceSymbolCeiling = 0;
  instanceLambdaCeiling = 0;
  lateDemandReady = false;
  lateDemandFailed = false;
  lateDemandedBodies.clear();
  resetInstanceReuse();
  deferredBodyIndex.clear();
  initializerEffectOwners.clear();
  lateAnalyzedBodies = nullptr;
  lateFinalization = nullptr;
  requestSession.invalidOrdinaryLayouts.clear();
  requestSession.preambleBodies.clear();
  requestSession.preambleBodyOrder.clear();
  requestSession.preambleForwardings = 0;
  requestSession.preambleCThreadCallUses = 0;
  requestSession.preambleUnsafeBlockCandidates = 0;
  requestSession.preambleMutableLocalCandidates = 0;
  requestSession.preambleGenericDefaultConstantUses = 0;
  requestSession.preambleOccurrenceCounts.clear();
  requestSession.preambleFirstLambda = 0;
  requestSession.cAbiRecordShapes.clear();
  requestSession.cAbiRecordStates.clear();
  requestSession.unionShapes.clear();
  requestSession.unionStates.clear();
  resumeDeclarations.clear();
  reachedPreambleBodies.clear();
  registrationFloor = {};
  currentNamespace.clear();
  currentSourceUnit = 0;
  structuredBindingElements.clear();
  requestSession.recoveredExhaustiveSwitches.clear();
  deferredBodies.clear();
  demandedFunctions.clear();
  visitedFunctionDeclarations.clear();
  analyzedBodies.clear();
  bodiesInProgress.clear();
  bodyPhaseActive = false;
  onDemandBodyAnalysis = false;
  declarationOrders.clear();
  declarationOrderCounter = 0;
  currentDeclarationOrder = 0;
  lexicalFrames.clear();
  lexicalFrameVersion = 0;
  lexicalLoanFlow = {};
  heldDiagnostics.clear();
  lexicallyReachableFunctions.clear();
  semanticModel.clear();
  semanticModel.setExecutionProfile(target.executionProfile);
  semanticModel.setPlaceSnapshot(acquirePlaceSnapshotIdentity());
  semanticModel.setAnalysisSeal(
      program != nullptr
          ? makeSemanticAnalysisSeal(*program, target, sourceGraph)
          : SemanticAnalysisSeal{.target = target});
  semanticModel.setToolingOccurrencesEnabled(toolingOccurrences);
  measureTopLevelStatements = false;
  currentType = SemanticType::Unknown;
  currentClass.reset();
  analyzingFieldInitializer = false;
  analyzingConstructorInitializer = false;
  analyzingDefaultArgument = false;
  currentDefaultArgumentRoot = nullptr;
  currentDefaultParameterNames.clear();
  analyzingCallCallee = false;
  expectedPayloadRangeCandidate = nullptr;
  analyzingPackFoldElement = false;
  analyzingDependentConstantCondition = false;
  constantConditionGenericDependency = false;
  allowPayloadEnumeratorReference = false;
  currentStaticMemberFunction = false;
  allowPackTypeReference = false;
  deferringTypeAliasConstraints = false;
  loanFlow = {};
  instanceClassContextActive = false;
  instanceAnalysisActive = false;
  instanceBaseModel = nullptr;
  currentFunctionDeclaration = nullptr;
  currentProgramEffectOwner = {};
  programEffects.clear();
  requestSession.programConcreteEffects =
      std::make_shared<ProgramConcreteEffectState>();
  requestSession.cThreadCallUses.clear();
  requestSession.rawBorrowCallUses.clear();
  requestSession.rawResultQueries.reset();
  requestSession.collectingRawResult.reset();
  requestSession.rawAccessObservation = 0;
  requestSession.hasCThreadRequirements = false;
  programStoragePlaceUseDepth = 0;
  preferReadOnlySubscriptReceiverDepth = 0;
  contextualInitializerType.reset();
  contextualInitializerExpression = nullptr;
  contextualOperatorIntegerType.reset();
  contextualCallableResult.reset();
  currentReceiverMutability = ReceiverMutability::ReadOnly;
  suppressedProjectedPlaceReads.clear();
  constructorDepth = 0;
  destructorDepth = 0;
  functionDepth = 0;
  loopDepth = 0;
  switchDepth = 0;
  lambdaDepth = 0;
  unsafeDepth = 0;
  suppressLoanPlaceAccessDepth = 0;
  lambdaUncapturedLocals.clear();
  currentReturnType = SemanticType::Unknown;
}

auto SemanticVisitor::Impl::check(const Program &program) -> bool {
  if (adoptedPreamble != nullptr)
    return checkResumed(program);
  resetRequestState(&program);
  analyzedProgram = &program;
  validateModuleExports(program.declarations());

  const auto registrationStart = std::chrono::steady_clock::now();
  registerDeclarations(program.declarations());
  catalog.seal();
  registrationSymbolCeiling = semanticModel.database().symbolIdentityCeiling();
  valueFlow.beginBody(catalog);
  if (workMetrics != nullptr) {
    workMetrics->semanticRegistrationMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - registrationStart)
            .count());
  }
  const auto bodyStart = std::chrono::steady_clock::now();
  analyzeBodies(program.declarations(), nullptr);
  if (workMetrics != nullptr) {
    workMetrics->semanticBodyMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - bodyStart)
            .count());
  }
  const auto finalizationStart = std::chrono::steady_clock::now();
  discoverOwnedCallableContracts();
  semanticModel.finalizeCallableForwardings();
  discoverOwnedCallableValueParameters();
  validateOnceCallableForwardings();
  validateOwnedCallableArguments();
  validateProvisionalCallableArguments();
  validateConfinedCallableContracts();
  validateConfinedCallableUses();
  semanticModel.finalizeCallableArguments();
  finalizeCThreadRequirements();
  finalizeRawBorrowContracts();
  // A prefix is not a program: initialization and the hosted entry are
  // planned by the analysis that resumes from it.
  if (!buildingPreamble)
    finalizeProgramInitialization(program);
  semanticModel.finalizeOccurrences();
  finalizeAdvisoryDiagnostics();
  catalog.assertUnchangedSinceSeal();
  if (workMetrics != nullptr) {
    workMetrics->semanticFinalizationMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - finalizationStart)
            .count());
  }
  lateDemandReady = trustedBodies == TrustedBodyAnalysis::Reachable;
  return !hadError();
}

auto SemanticVisitor::Impl::registerDeclarations(const StmtList &declarations)
    -> void {
  const RegistrationFloor &floor = registrationFloor;
  registerNamespaces(declarations, {});
  registerNamespaceAliases(declarations, {});
  registerTypeAliases(declarations, {});
  registerEnums(declarations, {});
  registerConcepts(declarations, {});
  registerEnumGenericParameters();
  genericDefaultConstants.clear();
  registerClasses(declarations, {});
  for (ClassId id = floor.classes + 1; id <= classes.size(); ++id)
    resolveClassGenericDefaults(id);
  for (EnumId id = floor.enums + 1; id <= enums.size(); ++id)
    resolveEnumGenericDefaults(id);
  resolveTypeAliases();
  for (ClassId id = floor.classes + 1; id <= classes.size(); ++id) {
    resolveClassArgumentPatterns(id);
  }
  registerClassSpecializations(declarations, {});
  resolveConcepts();
  recordClassTypeIdentities();
  resolveEnumPayloads();
  resolveClassInheritance();
  genericDefaultConstants.clear();
  registerFunctionGenericParameters(declarations, {}, false);
  genericDefaultConstants.clear();
  registerNamespaceSymbols(declarations, {});
  collectRootNativeStorageSymbols(declarations);
  collectClassMembers(declarations, {});
  bindOutOfClassDefinitions(declarations, {});
  if (requireFunctionDefinitions) {
    for (const auto &owner : registering(classes, floor.classes)) {
      for (const auto &[name, member] : owner.members) {
        for (const auto &candidate : member.symbol.overloads) {
          if (candidate.virtualMethod && !candidate.pureVirtual &&
              candidate.bodyDeclaration() != nullptr &&
              !candidate.bodyDeclaration()->body()) {
            requireFunctionDefinition(candidate, candidate.declaration->name());
          }
        }
      }
    }
  }
  recordClassFieldSymbols();
  validateOrdinaryClassLayouts();
  registerGlobalBorrowStorage(declarations, {});
  resolveInheritedMembers();
  validateNativeRecords();
  validateNativeFacingNamespaces(declarations);
  validateStoredReferenceContracts();
  resolveFunctionBorrowSummaries();
  validateInterfaceCapabilities();
  recordClassTypes();
  recordClassLifecycles();
  validateTypeAliasConstraints();
  registerDesignatedConstructors();
}

auto SemanticVisitor::Impl::analyzeBodies(const StmtList &declarations,
                                          const LateFinalizationScope *scope)
    -> void {
  beginScope();
  measureTopLevelStatements = workMetrics != nullptr;
  bodyPhaseActive = true;
  analyzeDeclarationAttributes();
  indexLexicallyReachableFunctions(declarations);
  {
    FunctionDemandScope demandScope(
        semanticModel, trustedBodies == TrustedBodyAnalysis::Reachable
                           ? &demandedFunctions
                           : nullptr);
    runLexicalPass(declarations);
    measureTopLevelStatements = false;
    analyzeDeferredBodies();
  }
  bodyPhaseActive = false;
  // A resumed analysis records and validates only what it added.
  lateFinalization = scope;
  recordGenericDefaultConstantUses();
  validateDefaultArgumentCycles();
  lateFinalization = nullptr;
  endScope();
}

auto SemanticVisitor::Impl::check(const Expr &expr) -> bool {
  resetRequestState(nullptr);
  beginScope();
  analyze(expr);
  endScope();
  semanticModel.finalizeOccurrences();
  return !hadError();
}

auto SemanticVisitor::Impl::hadError() const -> bool {
  return std::any_of(diagnostics.begin(), diagnostics.end(),
                     [](const Diagnostic &diagnostic) {
                       return diagnostic.severity == DiagnosticSeverity::Error;
                     });
}

auto SemanticVisitor::Impl::errors() const
    -> const std::vector<SemanticDiagnostic> & {
  return diagnostics;
}

auto SemanticVisitor::Impl::expressionType() const -> SemanticType {
  return currentType;
}

auto SemanticVisitor::Impl::model() const -> const SemanticModel & {
  return semanticModel;
}

auto SemanticVisitor::Impl::takeModel() -> SemanticModel {
  semanticModel.adoptExpansionArena(std::move(expansionArena),
                                    std::move(synthesizedExpressions));
  expansionArena.clear();
  synthesizedExpressions.clear();
  return std::move(semanticModel);
}

auto SemanticVisitor::Impl::traitsFor(const SemanticType &type) const
    -> SemanticTypeTraits {
  return typeTraits(type);
}

auto SemanticVisitor::Impl::requiresActiveCleanupFor(
    const SemanticType &type) const -> bool {
  ActiveCleanupQuery query;
  return requiresActiveCleanup(type, query);
}

auto SemanticVisitor::Impl::containsOwningPayloadEnum(
    const SemanticType &type) const -> bool {
  if (type.kind == SemanticType::Enum) {
    return requiresActiveCleanupFor(type);
  }
  if (type.kind == SemanticType::Class) {
    // Nominal values own their concrete fields through the ordinary class
    // lifecycle contract. A generic argument is not a stored ownership edge:
    // an empty tag, inline holder, and indirect collection may all name T.
    // Direct enum fields are validated at their field declaration instead.
    return false;
  }
  if (type.kind == SemanticType::Reference ||
      type.kind == SemanticType::RawPointer) {
    // An indirection does not transport the referent's ownership state.
    // Referent construction and storage retain their own lifecycle checks.
    return false;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const SemanticType &argument) {
                       return containsOwningPayloadEnum(argument);
                     });
}

auto SemanticVisitor::Impl::validateOwningPayloadEnumByValueBoundary(
    const SemanticType &type, const Token &location, std::string_view boundary,
    bool directTransport) -> void {
  if (!containsOwningPayloadEnum(type)) {
    return;
  }
  if (directTransport && type.kind == SemanticType::Enum) {
    return;
  }
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2067", DiagnosticPhase::Semantics, location,
                     "Owning payload enum type '" + typeSpelling(type) +
                         "' cannot cross a by-value " + std::string(boundary) +
                         " boundary in this slice.");
  diagnostic.hints.emplace_back(
      type.kind == SemanticType::Enum
          ? "Use an ordinary non-polymorphic GTI callable definition for "
            "direct owning payload-enum transport, or pass it by checked "
            "reference."
          : "Use an ordinary class or struct with a verified owning field "
            "lifecycle, or pass the enum directly by checked reference; "
            "this structural transport family is not yet supported.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::attachDeclarationDocumentation(
    SymbolId symbol, const Stmt &declaration) -> void {
  const std::optional<std::string> &documentation =
      declaration.documentationMarkdown();
  if (symbol != 0 && documentation && !documentation->empty()) {
    semanticModel.setSymbolDocumentation(symbol, *documentation);
  }
}

auto SemanticVisitor::Impl::attachTokenDocumentation(SymbolId symbol,
                                                     const Token &declaration)
    -> void {
  if (symbol != 0 && declaration.leadingDocumentationMarkdown &&
      !declaration.leadingDocumentationMarkdown->empty()) {
    semanticModel.setSymbolDocumentation(
        symbol, *declaration.leadingDocumentationMarkdown);
  }
}

auto SemanticVisitor::Impl::analyzeFunctionInstance(
    FunctionId functionId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &functionTypeArguments,
    const std::vector<CompileTimeValue> &functionValueArguments)
    -> SemanticInstanceAnalysis {
  const FunctionInfo *function = semanticModel.findFunction(functionId);
  InstanceAnalysisScope scope(*this);
  if (function == nullptr || function->declaration == nullptr) {
    return scope.finish();
  }

  prepareInstanceAnalysis();
  instanceAnalysisActive = true;
  instanceBaseModel = &scope.baseModel;
  seedExternalLambdaTypes(functionTypeArguments, scope.baseModel);
  if (!prepareInstanceContext(*function, classTypeArguments,
                              classValueArguments, functionTypeArguments,
                              functionValueArguments)) {
    return scope.finish();
  }
  FunctionInfo concreteFunction = *function;
  const GenericSubstitution substitution{.types = instanceTypeSubstitution,
                                         .values = instanceValueSubstitution};
  concreteFunction.returnType =
      substituteType(concreteFunction.returnType, substitution);
  for (SemanticType &parameter : concreteFunction.parameterTypes) {
    parameter = substituteType(parameter, substitution);
  }
  for (AppliedConceptRequirement &requirement : concreteFunction.requirements) {
    for (SemanticType &argument : requirement.arguments) {
      argument = substituteType(argument, substitution);
    }
  }
  if (!concreteFunction.explicitReturnBorrowOrigin) {
    if (const std::optional<FunctionReturnBorrowSummary> dynamicText =
            dynamicTextReturnBorrowSummary(concreteFunction)) {
      concreteFunction.returnBorrowOrigin = dynamicText->origin;
      concreteFunction.returnBorrowParameter = dynamicText->parameter;
      concreteFunction.returnBorrowAccess = dynamicText->access;
      concreteFunction.returnBorrowPlace = dynamicText->place;
    } else if (concreteFunction.returnBorrowOrigin == BorrowOriginKind::None) {
      const FunctionReturnBorrowSummary borrow = functionReturnBorrowSummary(
          concreteFunction.returnType, concreteFunction.parameterTypes,
          concreteFunction.ownerClass, concreteFunction.staticMember,
          concreteFunction.intrinsic, exactMovedReturnParameter(*function));
      concreteFunction.returnBorrowOrigin = borrow.origin;
      concreteFunction.returnBorrowParameter = borrow.parameter;
      concreteFunction.returnBorrowAccess = borrow.access;
      concreteFunction.returnBorrowParameters = borrow.parameters;
      concreteFunction.returnBorrowPlace = borrow.place;
    }
    if (const auto fields = storedFieldReturnBorrowSummary(concreteFunction)) {
      concreteFunction.returnBorrowOrigin = fields->origin;
      concreteFunction.returnBorrowParameter = fields->parameter;
      concreteFunction.returnBorrowAccess = fields->access;
      concreteFunction.returnBorrowParameters = fields->parameters;
      concreteFunction.returnBorrowFields = fields->fields;
      concreteFunction.returnBorrowPlace = fields->place;
    }
  }
  semanticModel.record(*function->declaration, std::move(concreteFunction));
  function->bodyDeclaration()->accept(*this);
  validateDefaultArgumentCycles();
  finalizeRawBorrowContracts();
  semanticModel.finalizeCallableArguments();
  finishInstanceContext(*function);
  return scope.finish();
}

auto SemanticVisitor::Impl::analyzeConstructorInstance(
    ConstructorId constructorId,
    const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments,
    const std::vector<SemanticType> &constructorTypeArguments,
    const std::vector<CompileTimeValue> &constructorValueArguments)
    -> SemanticInstanceAnalysis {
  const ConstructorInfo *constructor = nullptr;
  for (const ClassInfo &owner : classes) {
    if (owner.designatedConstructor &&
        owner.designatedConstructor->id == constructorId) {
      constructor = &*owner.designatedConstructor;
      break;
    }
    if (owner.reversedDesignatedConstructor &&
        owner.reversedDesignatedConstructor->id == constructorId) {
      constructor = &*owner.reversedDesignatedConstructor;
      break;
    }
    const auto found =
        std::find_if(owner.constructors.begin(), owner.constructors.end(),
                     [constructorId](const ConstructorInfo &candidate) {
                       return candidate.id == constructorId;
                     });
    if (found != owner.constructors.end()) {
      constructor = &*found;
      break;
    }
  }
  InstanceAnalysisScope scope(*this);
  if (constructor == nullptr || constructor->declaration == nullptr) {
    return scope.finish();
  }

  prepareInstanceAnalysis();
  instanceAnalysisActive = true;
  instanceBaseModel = &scope.baseModel;
  seedExternalLambdaTypes(constructorTypeArguments, scope.baseModel);
  if (!prepareClassInstanceContext(constructor->owner, classTypeArguments,
                                   classValueArguments)) {
    return scope.finish();
  }
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : constructor->genericParameters) {
    if (parameter.pack) {
      std::vector<SemanticType> elements(
          constructorTypeArguments.begin() +
              static_cast<std::ptrdiff_t>(typeIndex),
          constructorTypeArguments.end());
      instanceTypeSubstitution.insert_or_assign(
          parameter.id,
          SemanticType::concreteTypePack(parameter.id, std::move(elements)));
      typeIndex = constructorTypeArguments.size();
    } else if (parameter.value) {
      if (valueIndex >= constructorValueArguments.size()) {
        finishClassInstanceContext();
        return scope.finish();
      }
      instanceValueSubstitution.insert_or_assign(
          parameter.id, constructorValueArguments[valueIndex++]);
    } else {
      if (typeIndex >= constructorTypeArguments.size()) {
        finishClassInstanceContext();
        return scope.finish();
      }
      instanceTypeSubstitution.insert_or_assign(
          parameter.id, constructorTypeArguments[typeIndex++]);
    }
  }
  constructor->declaration->accept(*this);
  validateDefaultArgumentCycles();
  semanticModel.finalizeCallableArguments();
  finishClassInstanceContext();
  return scope.finish();
}

auto SemanticVisitor::Impl::analyzeDestructorInstance(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments)
    -> SemanticInstanceAnalysis {
  const ClassTypeInfo *classType = semanticModel.findClassType(classId);
  const ClassLifecycleInfo *lifecycle =
      classType == nullptr || classType->declaration == nullptr
          ? nullptr
          : semanticModel.findClassLifecycle(*classType->declaration);
  InstanceAnalysisScope scope(*this);
  if (lifecycle == nullptr || !lifecycle->declaredDestructor ||
      lifecycle->declaredDestructor->declaration == nullptr) {
    return scope.finish();
  }

  prepareInstanceAnalysis();
  if (!prepareClassInstanceContext(classId, classTypeArguments,
                                   classValueArguments)) {
    return scope.finish();
  }
  lifecycle->declaredDestructor->declaration->accept(*this);
  finishClassInstanceContext();
  return scope.finish();
}

auto SemanticVisitor::Impl::analyzeClassFieldInitializers(
    ClassId classId, const std::vector<SemanticType> &classTypeArguments,
    const std::vector<CompileTimeValue> &classValueArguments)
    -> SemanticInstanceAnalysis {
  const ClassTypeInfo *classType = semanticModel.findClassType(classId);
  InstanceAnalysisScope scope(*this);
  if (classType == nullptr || classType->declaration == nullptr) {
    return scope.finish();
  }

  prepareInstanceAnalysis();
  if (!prepareClassInstanceContext(classId, classTypeArguments,
                                   classValueArguments)) {
    return scope.finish();
  }
  analyzeConcreteClassStaticAssertions(classType->declaration->members());
  if (const auto *primary =
          semanticModel.findClassLifecycle(*classType->declaration)) {
    auto concrete = *primary;
    const SemanticType concreteType = SemanticType::classType(
        classId, classTypeArguments, classValueArguments);
    concrete.traits = typeTraits(concreteType);
    if (concrete.defaultConstructor == SpecialMemberStatus::Generated)
      concrete.defaultConstructor =
          classCanUseStructuralDefaultConstruction(concreteType)
              ? SpecialMemberStatus::Generated
              : SpecialMemberStatus::Deleted;
    // Absence of an open-primary operation is not an explicit '= delete'.
    // The ordinary concrete structural trait query owns availability.
    concrete.copyConstructor = concrete.traits.copyable
                                   ? SpecialMemberStatus::Generated
                                   : SpecialMemberStatus::Deleted;
    concrete.moveConstructor = concrete.traits.movable
                                   ? SpecialMemberStatus::Generated
                                   : SpecialMemberStatus::Deleted;
    concrete.copyAssignment = concrete.traits.copyAssignable
                                  ? SpecialMemberStatus::Generated
                                  : SpecialMemberStatus::Deleted;
    concrete.moveAssignment = concrete.traits.moveAssignable
                                  ? SpecialMemberStatus::Generated
                                  : SpecialMemberStatus::Deleted;
    semanticModel.record(*classType->declaration, std::move(concrete));
  }
  // Substitution can make aliases and class-owned method packs ill-formed
  // even when no method is called. Validate their types in the concrete
  // owner scope rather than deferring object-position errors to C++ emission.
  const auto validateMemberTypes = [&](const auto &self,
                                       const StmtList &members) -> void {
    for (const StmtPtr &member : members) {
      if (const auto *conditional = stmtAs<ConditionalStmt>(member.get())) {
        if (const StmtList *branch = conditional->activeBranch(target)) {
          self(self, *branch);
        }
      } else if (const auto *alias = stmtAs<TypeAliasDecl>(member.get())) {
        validateType(alias->target());
        if (const TypeAliasInfo *primary =
                semanticModel.findTypeAlias(*alias)) {
          TypeAliasInfo concrete = *primary;
          concrete.type = typeOf(alias->target());
          semanticModel.record(*alias, std::move(concrete));
        }
      } else if (const auto *function = stmtAs<FunctionDecl>(member.get());
                 function != nullptr && !function->parameters().empty() &&
                 function->parameters().back().pack) {
        const auto &parameters = genericParametersFor(*function);
        beginTypeParameterScope(parameters);
        validateFunctionPacks(*function, parameters);
        endTypeParameterScope();
      }
    }
  };
  validateMemberTypes(validateMemberTypes, classType->declaration->members());
  for (const ClassFieldTypeInfo &field : classType->fields) {
    if (field.declaration != nullptr) {
      field.declaration->accept(*this);
      if (requestSession.collectingRawResult &&
          requestSession.collectingRawResult->defaultOwner == classId &&
          field.declaration->initializer()) {
        auto fields = rawPointerFields(field.declaration->initializer().get());
        const auto *member =
            findMember(openClassType(classId), field.declaration->name());
        if (member) {
          for (auto &entry : fields)
            entry.projections.insert(
                entry.projections.begin(),
                {.kind = PlaceProjectionKind::Field,
                 .field = toolingSymbolFor(member->symbol)});
          collectRawResultFields(fields, {}, {}, true);
        }
      }
    }
  }
  for (const ClassFieldTypeInfo &field : classType->staticFields) {
    if (field.declaration != nullptr) {
      field.declaration->accept(*this);
    }
  }
  finishClassInstanceContext();
  return scope.finish();
}

auto SemanticVisitor::Impl::visitAccessSpecifierDecl(
    const AccessSpecifierDecl &) -> void {}

auto SemanticVisitor::Impl::visitEnumDecl(const EnumDecl &stmt) -> void {
  const EnumTypeInfo *info = semanticModel.findEnumType(stmt);
  if (info != nullptr) {
    beginTypeParameterScope(info->genericParameters);
  }
  const SemanticType type = info == nullptr ? SemanticType::Unknown
                                            : SemanticType::enumType(info->id);
  const SymbolId symbol = recordToolingSymbol(
      stmt.name(), SymbolKind::Enum,
      info == nullptr ? stmt.name().lexeme : info->qualifiedName, type);
  attachDeclarationDocumentation(symbol, stmt);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.name()),
       .kind = SemanticOccurrenceKind::EnumType,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = stmt.name().lexeme,
       .type = type,
       .enumType = &stmt});
  for (std::size_t index = 0; index < stmt.enumerators().size(); ++index) {
    const EnumeratorDecl &enumerator = stmt.enumerators()[index];
    const SymbolId enumeratorSymbol = recordToolingSymbol(
        enumerator.name, SymbolKind::Enumerator,
        (info == nullptr ? stmt.name().lexeme : info->qualifiedName) +
            "::" + enumerator.name.lexeme,
        type);
    attachTokenDocumentation(enumeratorSymbol, enumerator.name);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(enumerator.name),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = enumeratorSymbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = enumerator.name.lexeme,
         .type = type});
    for (std::size_t payloadIndex = 0; payloadIndex < enumerator.payload.size();
         ++payloadIndex) {
      const Parameter &parameter = enumerator.payload[payloadIndex];
      recordTypeUse(parameter.type);
      if (parameter.defaultArgument) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2077", DiagnosticPhase::Semantics,
            parameter.defaultArgument->equal,
            "Default arguments are not supported on payload-enum fields.");
        diagnostic.hints.emplace_back(
            "Construct each payload variant with every field explicitly.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
      if (parameter.name.lexeme.empty()) {
        continue;
      }
      const SemanticType fieldType =
          info != nullptr && index < info->enumerators.size() &&
                  payloadIndex < info->enumerators[index].payloadTypes.size()
              ? info->enumerators[index].payloadTypes[payloadIndex]
              : SemanticType::Unknown;
      const SymbolId fieldSymbol = recordToolingSymbol(
          parameter.name, SymbolKind::Field,
          (info == nullptr ? stmt.name().lexeme : info->qualifiedName) +
              "::" + enumerator.name.lexeme + "::" + parameter.name.lexeme,
          fieldType);
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(parameter.name),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = fieldSymbol,
           .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
           .name = parameter.name.lexeme,
           .type = fieldType});
    }
  }
  if (info != nullptr) {
    endTypeParameterScope();
  }
}

auto SemanticVisitor::Impl::enterUnsafeRegion(const Token &keyword,
                                              bool expression) -> void {
  ++unsafeDepth;
  requestSession.activeUnsafeBlocks.push_back(&keyword);
  // A generic body can acquire a required operation only after concrete
  // selection. Do not suggest removing its permission from symbolic evidence.
  const bool genericFunction =
      currentFunctionDeclaration != nullptr &&
      !currentFunctionDeclaration->genericParameters().empty();
  const bool genericClass =
      currentClass && !classInfo(*currentClass).genericParameters.empty();
  if (!instanceAnalysisActive && !genericFunction && !genericClass &&
      !isCompilerTrustedRequester(keyword)) {
    requestSession.unsafeBlockCandidates.push_back(
        {.keyword = &keyword,
         .expression = expression,
         .owner = currentProgramEffectOwner});
  }
}

auto SemanticVisitor::Impl::leaveUnsafeRegion() -> void {
  requestSession.activeUnsafeBlocks.pop_back();
  --unsafeDepth;
}

auto SemanticVisitor::Impl::visitBlockStmt(const BlockStmt &stmt) -> void {
  if (stmt.isUnsafe())
    enterUnsafeRegion(*stmt.unsafeKeyword(), false);
  beginScope();
  analyze(stmt.statements());
  endScope();
  if (stmt.isUnsafe())
    leaveUnsafeRegion();
}

// The work visitClassDecl does before its members (tooling symbol, base and
// field validation, generic scope), shared with the resumable lexical pass.
// The caller has set currentClass; this leaves the class's type-parameter
// scope open.
auto SemanticVisitor::Impl::beginClassBody(const ClassDecl &stmt,
                                           const ClassInfo &info,
                                           const SemanticType &type) -> void {
  if (stmt.exactSpecialization() &&
      stmt.exactSpecialization()->name.segments.size() > 1) {
    const NamePath namespacePath(std::vector<Token>(
        stmt.exactSpecialization()->name.segments.begin(),
        stmt.exactSpecialization()->name.segments.end() - 1));
    recordQualifiedPathUses(namespacePath, true);
  }
  const SymbolId symbol = recordToolingSymbol(
      stmt.name(),
      stmt.kind() == ClassKind::Struct ? SymbolKind::Struct : SymbolKind::Class,
      stmt.isExactSpecialization()
          ? typeSpelling(type)
          : qualifiedName(info.namespaceScope, stmt.name().lexeme),
      type, false, !stmt.isForwardDeclaration());
  attachDeclarationDocumentation(symbol, stmt);
  OccurrenceRole roles = OccurrenceRole::Declaration;
  if (!stmt.isForwardDeclaration()) {
    roles |= OccurrenceRole::Definition;
  }
  semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                  .span = tokenSpan(stmt.name()),
                                  .kind = SemanticOccurrenceKind::ClassType,
                                  .symbol = symbol,
                                  .roles = roles,
                                  .name = stmt.name().lexeme,
                                  .type = type,
                                  .traits = typeTraits(type),
                                  .classType = &stmt});
  currentNamespace = info.namespaceScope;
  beginTypeParameterScope(info.genericParameters);
  for (const BaseSpecifier &base : stmt.bases()) {
    validateType(base.type);
    validateReferencePlacement(base.type, false, "base type");
  }
  if (info.constructors.empty() && !info.designatedConstructor &&
      !info.cAbiRecord && !info.cOpaqueHandle &&
      info.kind != ClassKind::Union) {
    for (const FieldInfo &field : info.fields) {
      const auto member = info.members.find(field.declaration->name().lexeme);
      const bool storedReference =
          member != info.members.end() &&
          member->second.symbol.type.kind == SemanticType::Reference;
      const bool nominalDefault =
          member != info.members.end() &&
          member->second.symbol.type.kind == SemanticType::Class &&
          isDefaultInitializable(member->second.symbol.type, true);
      if (!field.declaration->initializer() && !storedReference &&
          !nominalDefault) {
        report(field.declaration->name(),
               "Class and struct fields must have an initializer or be "
               "initialized by a constructor.");
      }
    }
  }
}

// The work visitClassDecl does after its members.
auto SemanticVisitor::Impl::endClassBody(const ClassInfo &info) -> void {
  for (const std::optional<ConstructorInfo> *generated :
       {&info.designatedConstructor, &info.reversedDesignatedConstructor}) {
    if (!*generated) {
      continue;
    }
    // Reserve declaration identities for the lowered inventory. Analyze the
    // generated body only when concrete HIR selects it, like an instance.
    const auto &constructor = **generated;
    for (std::size_t i = 0; i < constructor.parameterTypes.size(); ++i) {
      const Parameter &parameter = constructor.declaration->parameters()[i];
      const SemanticType &type = constructor.parameterTypes[i];
      BindingInfo binding = bindingInfo(type, AccessMode::ReadOnly);
      binding.symbol = recordParameterBinding(parameter, type);
      semanticModel.record(parameter, std::move(binding));
    }
  }
}

auto SemanticVisitor::Impl::visitClassDecl(const ClassDecl &stmt) -> void {
  const DeclarationOrderScope declarationOrder{*this, stmt};
  const auto registered = classDeclIds.find(&stmt);
  if (registered == classDeclIds.end()) {
    return;
  }
  const std::optional<ClassId> enclosingClass = currentClass;
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  currentClass = registered->second;
  const ClassInfo &info = classInfo(*currentClass);
  const SemanticType type = openClassType(info.id);
  beginClassBody(stmt, info, type);
  beginScope();
  for (const auto &[name, member] : info.members) {
    scopes.back().emplace(name, member.symbol);
  }
  analyze(stmt.members());
  endClassBody(info);
  endScope();
  endTypeParameterScope();
  currentNamespace = enclosingNamespace;
  currentClass = enclosingClass;
}

// constexpr for ( [mut] type IDENTIFIER : sequence ) statement
//
// The sequence is a value whose class type carries exactly one type pack; its
// elements are the pack's types. Each element gets its own copy of
// `{ binding; body }`, analysed in order in a scope of its own with the
// binding initialized to a default-constructed element. A sequence type that
// depends on an open generic parameter defers the expansion to concrete
// reanalysis, exactly like a dependent `if constexpr` condition.
auto SemanticVisitor::Impl::visitConstexprForStmt(const ConstexprForStmt &stmt)
    -> void {
  const SemanticType sequenceType = analyze(stmt.sequence());
  if (sequenceType == SemanticType::Unknown) {
    semanticModel.recordConstexprForExpansion(stmt, std::nullopt);
    return;
  }
  if (containsOpenTypeParameter(sequenceType)) {
    semanticModel.recordConstexprForExpansion(stmt, std::nullopt);
    loanFlow.deferredConstexprSelection = true;
    requestSession.deferredFlowOwners.insert(currentProgramEffectOwner);
    return;
  }
  const std::optional<std::vector<SemanticType>> elements =
      compileTimeSequenceElements(sequenceType);
  if (!elements) {
    report(stmt.colon(),
           "A 'constexpr for' sequence must be a value of a class whose "
           "generic arguments are one type pack; '" +
               typeSpelling(sequenceType) + "' is not.",
           "GTI-S2107");
    semanticModel.recordConstexprForExpansion(stmt, std::nullopt);
    return;
  }
  if (elements->size() > kConstexprForElementLimit ||
      constexprForExpandedBodies + elements->size() > kConstexprForBodyLimit) {
    report(stmt.forKeyword(),
           "'constexpr for' expansion exceeds the compile-time budget (" +
               std::to_string(kConstexprForElementLimit) +
               " elements per statement, " +
               std::to_string(kConstexprForBodyLimit) +
               " expanded bodies per declaration).",
           "GTI-S2108");
    semanticModel.recordConstexprForExpansion(stmt, std::nullopt);
    return;
  }
  constexprForExpandedBodies += elements->size();

  ConstexprForExpansion expanded;
  struct EmittedDiagnostic {
    std::string code;
    SourceSpan span;
    std::string message;
  };
  std::vector<EmittedDiagnostic> emitted;
  for (std::size_t index = 0; index < elements->size(); ++index) {
    const SemanticType &element = (*elements)[index];
    // Every copy gets an expansion id stamped on its tokens, so declarations
    // inside it have identities of their own (see Token::expansion).
    const std::uint32_t expansion = ++nextExpansion;
    // The element binding: `[constexpr] auto name = <element>()`. It is
    // constexpr when the element is a constant the evaluator can produce, so
    // `if constexpr` and `static_assert` may read it; a plain value otherwise.
    ExprPtr initializer = cloneExpr(*stmt.elementInitializer(), expansion);
    const Call *initializerCopy = exprAs<Call>(initializer.get());
    const bool constantElement =
        stmt.bindingMutability() != Mutability::Mutable &&
        isConstexprDefaultConstructible(element);
    const Token &anchor = stmt.constexprKeyword();
    Token bindingName = stmt.bindingName();
    bindingName.expansion = expansion;
    AstCloneResult binding;
    binding.statement = std::make_shared<VariableDecl>(
        stmt.bindingMutability(), stmt.bindingType(), bindingName,
        std::move(initializer), std::nullopt, false,
        constantElement
            ? std::optional<Token>{Token(
                  TokenKind::CONSTEXPR, "constexpr", std::monostate{},
                  anchor.position, anchor.line, anchor.source, false, true)}
            : std::nullopt,
        false, false,
        stmt.bindingMutability() == Mutability::Mutable
            ? std::optional<Token>{Token(
                  TokenKind::MUT, "mut", std::monostate{}, anchor.position,
                  anchor.line, anchor.source, false, true)}
            : std::nullopt);
    AstCloneResult body = cloneStmt(*stmt.body(), expansion);
    if (!binding.statement || !body.statement || initializerCopy == nullptr) {
      report(stmt.forKeyword(),
             "Function, class, enum, concept, alias and namespace "
             "declarations are not supported inside a 'constexpr for' body.",
             "GTI-S2109");
      semanticModel.recordConstexprForExpansion(stmt, std::nullopt);
      return;
    }
    StmtList statements;
    statements.push_back(std::move(binding.statement));
    statements.push_back(std::move(body.statement));
    auto block = std::make_shared<BlockStmt>(std::move(statements));
    if (stmt.body()->extent()) {
      block->setExtent(*stmt.body()->extent());
    }
    const std::size_t diagnosticsBefore = diagnostics.size();
    constexprForFrames.push_back(
        {.initializer = initializerCopy, .element = element});
    analyze(block);
    constexprForFrames.pop_back();
    // Name the element in every diagnostic the copy produced, and drop the
    // ones an earlier element already produced word for word.
    const std::string context = "while expanding 'constexpr for' for element " +
                                std::to_string(index) + " of type '" +
                                typeSpelling(element) + "'";
    for (std::size_t position = diagnosticsBefore;
         position < diagnostics.size();) {
      Diagnostic &diagnostic = diagnostics[position];
      const bool duplicate = std::any_of(
          emitted.begin(), emitted.end(), [&](const EmittedDiagnostic &seen) {
            return seen.code == diagnostic.code &&
                   seen.span == diagnostic.primary &&
                   seen.message == diagnostic.message;
          });
      if (duplicate) {
        diagnostics.erase(diagnostics.begin() +
                          static_cast<std::ptrdiff_t>(position));
        continue;
      }
      emitted.push_back({.code = diagnostic.code,
                         .span = diagnostic.primary,
                         .message = diagnostic.message});
      diagnostic.related.push_back(
          {.span = tokenSpan(stmt.bindingName()), .message = context});
      ++position;
    }
    expansionArena.push_back(block);
    expanded.iterations.push_back(
        {.element = element, .block = std::move(block)});
  }
  semanticModel.recordConstexprForExpansion(stmt, std::move(expanded));
}

// field.get(owner) / field.get_mut(owner) on a std::meta::field<T, I>
// metaobject: the call denotes the projected place `owner.<field>` and is
// analysed as exactly that expression, synthesized here and aliased from the
// call, so access control, borrowing, mutability and lowering are the
// ordinary field-access rules at this call site.
auto SemanticVisitor::Impl::analyzeMetaFieldAccess(
    const Call &expr, const Get &member, const SemanticType &fieldObject,
    bool mutableAccess) -> void {
  const IntrinsicKind intrinsic = mutableAccess ? IntrinsicKind::MetaFieldGetMut
                                                : IntrinsicKind::MetaFieldGet;
  if (expr.arguments().size() != 1 || !expr.typeArguments().empty() ||
      fieldObject.arguments.size() != 1 ||
      fieldObject.valueArguments.size() != 1) {
    report(expr.paren(),
           "Reflected field access takes exactly one owner argument.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType &owner = fieldObject.arguments.front();
  const CompileTimeValue &index = fieldObject.valueArguments.front();
  const AccessMode access =
      mutableAccess ? AccessMode::Mutable : AccessMode::ReadOnly;
  if (containsOpenTypeParameter(owner) ||
      index.kind != CompileTimeValue::UInt64) {
    // Symbolic: the exact field type is the computed compiler type, which
    // concrete reanalysis rewrites together with the access below.
    const ClassId computed = compilerTypeClass(CompilerTypeKind::MetaFieldType);
    currentType = computed == 0
                      ? SemanticType::Unknown
                      : SemanticType::classType(computed, {owner}, {index});
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .typeArguments = {owner},
                               .valueArguments = {index},
                               .intrinsic = intrinsic,
                               .borrowOrigin = BorrowOriginKind::Argument,
                               .borrowArgument = 0,
                               .borrowAccess = access});
    return;
  }
  const std::optional<MetaFieldFacts> facts =
      metaFieldFacts(owner, index.value);
  if (!facts) {
    report(expr.paren(),
           "Field index " + std::to_string(index.value) +
               " is out of range for '" + typeSpelling(owner) +
               "', which has " + std::to_string(metaFieldCount(owner)) +
               " instance field(s).",
           "GTI-S2111");
    currentType = SemanticType::Unknown;
    return;
  }
  // The owner argument must satisfy the declared `T&` parameter: the
  // descriptor names a field of `T`, and a same-named field of another type
  // is not it (GitHub #5).
  if (const SemanticType *argumentType =
          semanticModel.findType(*expr.arguments().front())) {
    SemanticType actual = *argumentType;
    if (actual.kind == SemanticType::Reference &&
        actual.arguments.size() == 1) {
      actual = actual.arguments.front();
    }
    if (actual != SemanticType::Unknown && !containsOpenTypeParameter(actual) &&
        !isSameOrDerivedClass(actual, owner)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2003", DiagnosticPhase::Semantics,
          expressionToken(*expr.arguments().front()),
          "Argument 1 has type '" + typeSpelling(actual) +
              "' but the field describes '" + typeSpelling(owner) +
              "'; reflected access takes the described owner.");
      diagnostic.related.push_back(
          {tokenSpan(facts->declaration->name()), "Field declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }
  }
  const Token &anchor = member.name();
  const auto generated = [&](TokenKind kind, std::string lexeme) {
    return Token(kind, std::move(lexeme), std::monostate{}, anchor.position,
                 anchor.line, anchor.source, false, true);
  };
  ExprPtr projected = std::make_unique<Get>(
      cloneExpr(*expr.arguments().front()), generated(TokenKind::DOT, "."),
      generated(TokenKind::IDENTIFIER, facts->name));
  // A receiver with effects (a call producing the metaobject) still runs,
  // exactly once and before the owner, as the left operand of a comma whose
  // value and place are the projection (GitHub #4).
  if (!isEffectFreeMetaReceiver(*member.object())) {
    // Grouped like the source form `(receiver, owner.field)`: a grouping is a
    // place expression for lowering, so the projection stays a place.
    projected = std::make_unique<Grouping>(std::make_unique<Binary>(
        cloneExpr(*member.object()), generated(TokenKind::COMMA, ","),
        std::move(projected)));
  }
  std::shared_ptr<const Expr> projection = std::move(projected);
  synthesizedExpressions.push_back(projection);
  const Expr &alias = semanticModel.recordExpressionAlias(expr, projection);
  const bool enclosingReflectedAccess = analyzingReflectedFieldAccess;
  analyzingReflectedFieldAccess = true;
  const SemanticType result = analyze(alias);
  analyzingReflectedFieldAccess = enclosingReflectedAccess;
  const ExpressionInfo *info = semanticModel.findExpression(alias);
  if (mutableAccess && result != SemanticType::Unknown &&
      (info == nullptr || info->category != ValueCategory::Place ||
       info->access != AccessMode::Mutable)) {
    report(expr.paren(),
           "'get_mut' requires a writable field place; '" + facts->name +
               "' is not writable here (the field is immutable or the owner "
               "is read-only).",
           "GTI-S2112");
  }
  currentType = result;
  // No borrow origin: the call *is* the place its alias denotes, and every
  // place-aware pass reads it through the alias.
  semanticModel.record(expr, ResolvedCallInfo{.returnType = result,
                                              .typeArguments = {owner},
                                              .valueArguments = {index},
                                              .intrinsic = intrinsic,
                                              .borrowAccess = access});
  if (const PlaceKey *place = semanticModel.findPlace(alias)) {
    semanticModel.recordPlace(expr, *place);
  }
  semanticModel.recordPlaceSelection(expr, nextPlaceSelectionId++);
}

// `field.attribute<A>()` / `type.attribute<A>()`: the constant object that the
// declaration's `[[A(...)]]` application denotes. The call carries that
// constant for compile-time readers and is aliased to a designated
// construction of `A` from the application's own arguments, so a run-time
// reader builds the same value through ordinary struct construction.
auto SemanticVisitor::Impl::analyzeMetaAttributeAccess(
    const Call &expr, const Get &member, const SemanticType &metaObject,
    bool typeSubject) -> void {
  const IntrinsicKind intrinsic = typeSubject
                                      ? IntrinsicKind::MetaTypeAttribute
                                      : IntrinsicKind::MetaFieldAttribute;
  if (!expr.arguments().empty() || expr.typeArguments().size() != 1 ||
      metaObject.arguments.size() != 1 ||
      metaObject.valueArguments.size() != (typeSubject ? 0U : 1U)) {
    report(expr.paren(),
           "'attribute' takes the attribute type as its one generic argument "
           "and no call arguments.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  validateType(expr.typeArguments().front());
  const SemanticType attribute = typeOf(expr.typeArguments().front());
  if (attribute == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType &subject = metaObject.arguments.front();
  const std::optional<CompileTimeValue> index =
      typeSubject ? std::nullopt
                  : std::optional(metaObject.valueArguments.front());
  const ClassInfo *attributeClass =
      attribute.kind == SemanticType::Class ? classInfo(attribute) : nullptr;
  if (!containsOpenTypeParameter(attribute) &&
      (attributeClass == nullptr || !attributeClass->attributeType)) {
    report(expr.typeArguments().front().name.last(),
           "'" + typeSpelling(attribute) + "' is not an attribute type.",
           "GTI-S2113");
    currentType = SemanticType::Unknown;
    return;
  }
  currentType = attribute;
  ResolvedCallInfo resolved{.returnType = attribute,
                            .typeArguments = {subject, attribute},
                            .intrinsic = intrinsic};
  if (index) {
    resolved.valueArguments = {*index};
  }
  if (containsOpenTypeParameter(subject) ||
      containsOpenTypeParameter(attribute) ||
      (index && index->kind != CompileTimeValue::UInt64)) {
    semanticModel.record(expr, resolved);
    return;
  }
  const std::vector<ResolvedDeclarationAttribute> *attributes =
      declarationAttributesOf(subject, index ? std::optional(index->value)
                                             : std::nullopt);
  if (attributes == nullptr) {
    if (index) {
      report(expr.paren(),
             "Field index " + std::to_string(index->value) +
                 " is out of range for '" + typeSpelling(subject) +
                 "', which has " + std::to_string(metaFieldCount(subject)) +
                 " instance field(s).",
             "GTI-S2111");
    } else {
      report(expr.paren(),
             "'" + typeSpelling(subject) + "' has no declaration attributes.",
             "GTI-S2116");
    }
    currentType = SemanticType::Unknown;
    return;
  }
  const auto found =
      std::find_if(attributes->begin(), attributes->end(),
                   [&](const ResolvedDeclarationAttribute &candidate) {
                     return candidate.type == attribute.classId;
                   });
  if (found == attributes->end() || found->syntax == nullptr) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2116", DiagnosticPhase::Semantics, expr.paren(),
        (index ? "Field " + std::to_string(index->value) + " of '" : "Type '") +
            typeSpelling(subject) + "' has no attribute '" +
            typeSpelling(attribute) + "'.");
    diagnostic.hints.emplace_back(
        "Test for the attribute with has_attribute<A>() before reading it.");
    diagnostics.emplace_back(std::move(diagnostic));
    currentType = SemanticType::Unknown;
    return;
  }
  semanticModel.record(expr, resolved);
  semanticModel.recordConstant(expr, found->value);

  // The run-time alias: `A{.field = argument, ...}` over clones of the
  // application's arguments; omitted fields take their declaration defaults.
  const Token &anchor = member.name();
  const auto generated = [&](TokenKind kind, std::string lexeme) {
    return Token(kind, std::move(lexeme), std::monostate{}, anchor.position,
                 anchor.line, anchor.source, false, true);
  };
  const DeclarationAttribute &syntax = *found->syntax;
  ExprList arguments;
  std::vector<Token> designators;
  arguments.reserve(syntax.arguments.size());
  designators.reserve(syntax.arguments.size());
  for (std::size_t position = 0; position < syntax.arguments.size();
       ++position) {
    arguments.push_back(cloneExpr(*syntax.arguments[position], 0));
    if (!syntax.designators.empty()) {
      designators.push_back(syntax.designators[position]);
    } else if (position < attributeClass->fields.size() &&
               attributeClass->fields[position].declaration != nullptr) {
      designators.push_back(generated(
          TokenKind::IDENTIFIER,
          attributeClass->fields[position].declaration->name().lexeme));
    }
  }
  auto construction = std::make_shared<DirectInitializer>(
      generated(TokenKind::LEFT_BRACE, "{"), std::move(arguments),
      generated(TokenKind::RIGHT_BRACE, "}"), std::move(designators));
  synthesizedExpressions.push_back(construction);
  const Expr &alias = semanticModel.recordExpressionAlias(expr, construction);
  const std::optional<SemanticType> enclosingType = contextualInitializerType;
  const Expr *enclosingExpression = contextualInitializerExpression;
  contextualInitializerType = attribute;
  contextualInitializerExpression = &alias;
  const SemanticType constructed = analyze(alias);
  contextualInitializerType = enclosingType;
  contextualInitializerExpression = enclosingExpression;
  currentType = constructed == SemanticType::Unknown ? constructed : attribute;
}

// `field<T, I>().default_value()`: the value of the field's declared default
// initializer, evaluated where the call is. Semantics aliases the call to a
// clone of that initializer, analysed under the field's type like the
// declaration itself; a field without an initializer is rejected.
auto SemanticVisitor::Impl::analyzeMetaFieldDefault(
    const Call &expr, const Get &member, const SemanticType &fieldObject)
    -> void {
  if (!expr.arguments().empty() || !expr.typeArguments().empty() ||
      fieldObject.arguments.size() != 1 ||
      fieldObject.valueArguments.size() != 1) {
    report(expr.paren(), "'default_value' takes no arguments.", "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType &owner = fieldObject.arguments.front();
  const CompileTimeValue &index = fieldObject.valueArguments.front();
  if (containsOpenTypeParameter(owner) ||
      index.kind != CompileTimeValue::UInt64) {
    // Symbolic: the field type is the computed compiler type, rewritten by
    // concrete reanalysis together with the alias below.
    const ClassId computed = compilerTypeClass(CompilerTypeKind::MetaFieldType);
    currentType = computed == 0
                      ? SemanticType::Unknown
                      : SemanticType::classType(computed, {owner}, {index});
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = currentType,
                               .typeArguments = {owner},
                               .valueArguments = {index},
                               .intrinsic = IntrinsicKind::MetaFieldDefault});
    return;
  }
  const std::optional<MetaFieldFacts> facts =
      metaFieldFacts(owner, index.value);
  if (!facts) {
    report(expr.paren(),
           "Field index " + std::to_string(index.value) +
               " is out of range for '" + typeSpelling(owner) +
               "', which has " + std::to_string(metaFieldCount(owner)) +
               " instance field(s).",
           "GTI-S2111");
    currentType = SemanticType::Unknown;
    return;
  }
  if (facts->declaration == nullptr || !facts->declaration->initializer()) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2119", DiagnosticPhase::Semantics, member.name(),
        "Field '" + facts->name + "' of '" + typeSpelling(owner) +
            "' declares no default initializer.");
    diagnostic.hints.emplace_back(
        "Test for a default with has_default() before reading it.");
    diagnostics.emplace_back(std::move(diagnostic));
    currentType = SemanticType::Unknown;
    return;
  }
  semanticModel.record(
      expr, ResolvedCallInfo{.returnType = facts->type,
                             .typeArguments = {owner},
                             .valueArguments = {index},
                             .intrinsic = IntrinsicKind::MetaFieldDefault});
  std::shared_ptr<const Expr> clone =
      cloneExpr(*facts->declaration->initializer(), 0);
  synthesizedExpressions.push_back(clone);
  const Expr &alias = semanticModel.recordExpressionAlias(expr, clone);
  // The initializer's names resolve where the field was declared, not where
  // `default_value` is called: analyse the clone in the owner's namespace,
  // source unit and class, as the declaration itself is analysed.
  const ClassInfo &ownerInfo = classInfo(owner.classId);
  const auto enclosingNamespace = currentNamespace;
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const std::optional<ClassId> enclosingClass = currentClass;
  const bool enclosingFieldInitializer = analyzingFieldInitializer;
  const std::optional<SemanticType> enclosingType = contextualInitializerType;
  const Expr *enclosingExpression = contextualInitializerExpression;
  currentNamespace = ownerInfo.namespaceScope;
  currentSourceUnit = ownerInfo.sourceUnit;
  currentClass = owner.classId;
  analyzingFieldInitializer = true;
  contextualInitializerType = facts->type;
  contextualInitializerExpression = &alias;
  const SemanticType produced = analyze(alias);
  contextualInitializerType = enclosingType;
  contextualInitializerExpression = enclosingExpression;
  analyzingFieldInitializer = enclosingFieldInitializer;
  currentClass = enclosingClass;
  currentSourceUnit = enclosingUnit;
  currentNamespace = enclosingNamespace;
  currentType = produced == SemanticType::Unknown ? produced : facts->type;
}

auto SemanticVisitor::Impl::isEffectFreeMetaReceiver(
    const Expr &expression) const -> bool {
  switch (expression.exprKind()) {
  case ExprKind::Variable:
  case ExprKind::This:
  case ExprKind::QualifiedName:
  case ExprKind::LiteralExpr:
    return true;
  case ExprKind::Grouping:
    return isEffectFreeMetaReceiver(
        *static_cast<const Grouping &>(expression).expression());
  case ExprKind::Get:
    return isEffectFreeMetaReceiver(
        *static_cast<const Get &>(expression).object());
  case ExprKind::Call: {
    // Default construction of an empty metaobject has no effects.
    const auto &call = static_cast<const Call &>(expression);
    const ResolvedConstructionInfo *construction =
        semanticModel.findConstruction(call);
    const SemanticType *type = semanticModel.findType(call);
    return construction != nullptr && call.arguments().empty() &&
           type != nullptr && type->kind == SemanticType::Class &&
           type->classId != 0 && type->classId <= classes.size() &&
           classInfo(type->classId).compilerType != CompilerTypeKind::None;
  }
  default:
    return false;
  }
}

// `gti_internal::meta_construct<T>(values...)`: `T` built through its
// designated constructor from one value per instance field, in declaration
// order, under ordinary construction, ownership transfer and rollback.
auto SemanticVisitor::Impl::analyzeMetaConstructCall(const Call &expr) -> void {
  if (expr.typeArguments().size() != 1) {
    report(expr.paren(),
           "std::meta::construct takes the constructed type as its one "
           "generic argument.",
           "GTI-S2019");
    currentType = SemanticType::Unknown;
    return;
  }
  validateType(expr.typeArguments().front());
  const SemanticType subject =
      selectExactClassSpecialization(typeOf(expr.typeArguments().front()));
  std::vector<SemanticType> argumentTypes;
  bool open = containsOpenTypeParameter(subject);
  for (const ExprPtr &argument : expr.arguments()) {
    argumentTypes.push_back(analyze(argument));
    const SemanticType &argumentType = argumentTypes.back();
    if (argumentType.kind == SemanticType::TypePack) {
      // A concrete pack lists its element types; only an unexpanded pack
      // parameter is still symbolic.
      open = open || !argumentType.concretePack ||
             std::any_of(argumentType.arguments.begin(),
                         argumentType.arguments.end(),
                         [&](const SemanticType &element) {
                           return containsOpenTypeParameter(element);
                         });
    } else {
      open = open || containsOpenTypeParameter(argumentType);
    }
  }
  if (subject == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (open) {
    // Symbolic: concrete reanalysis selects the construction.
    currentType = subject;
    semanticModel.record(
        expr, ResolvedCallInfo{.returnType = subject,
                               .typeArguments = {subject},
                               .intrinsic = IntrinsicKind::MetaConstruct});
    return;
  }
  const ClassInfo *owner =
      subject.kind == SemanticType::Class ? classInfo(subject) : nullptr;
  if (owner == nullptr || owner->kind != ClassKind::Struct ||
      !owner->reversedDesignatedConstructor) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2118", DiagnosticPhase::Semantics, expr.paren(),
        "std::meta::construct<" + typeSpelling(subject) +
            "> requires a struct eligible for designated construction: public "
            "value fields, no declared constructors or bases.");
    diagnostic.hints.emplace_back(
        "Construct other types through their own constructors; structural "
        "construction never bypasses them.");
    diagnostics.emplace_back(std::move(diagnostic));
    currentType = SemanticType::Unknown;
    return;
  }
  selectingDesignatedConstructor = true;
  analyzeConstructorCall(expr, subject.classId, subject.arguments,
                         subject.valueArguments, argumentTypes,
                         expr.arguments(), expr.paren());
  selectingDesignatedConstructor = false;
}

auto SemanticVisitor::Impl::builtinTypeToken(const SemanticType &type,
                                             const Token &anchor)
    -> std::optional<Token> {
  const auto generated = [&](TokenKind kind, std::string lexeme) {
    return Token(kind, std::move(lexeme), std::monostate{}, anchor.position,
                 anchor.line, anchor.source, false, true);
  };
  switch (type.kind) {
  case SemanticType::Int8:
    return generated(TokenKind::INT8, "int8_t");
  case SemanticType::Int16:
    return generated(TokenKind::INT16, "int16_t");
  case SemanticType::Int32:
    return generated(TokenKind::INT32, "int32_t");
  case SemanticType::Int64:
    return generated(TokenKind::INT64, "int64_t");
  case SemanticType::UInt8:
    return generated(TokenKind::UINT8, "uint8_t");
  case SemanticType::UInt16:
    return generated(TokenKind::UINT16, "uint16_t");
  case SemanticType::UInt32:
    return generated(TokenKind::UINT32, "uint32_t");
  case SemanticType::UInt64:
    return generated(TokenKind::UINT64, "uint64_t");
  case SemanticType::Float:
    return generated(TokenKind::FLOAT, "float");
  case SemanticType::Double:
    return generated(TokenKind::DOUBLE, "double");
  case SemanticType::Bool:
    return generated(TokenKind::BOOL, "bool");
  case SemanticType::Char:
    return generated(TokenKind::CHAR, "char");
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::constantLiteralExpression(
    const ConstantValue &value, const SemanticType &type,
    const Token &anchor) const -> ExprPtr {
  const auto generated = [&](TokenKind kind, std::string lexeme,
                             Literal literal = std::monostate{}) {
    return Token(kind, std::move(lexeme), std::move(literal), anchor.position,
                 anchor.line, anchor.source, false, true);
  };
  const auto literal = [&](TokenKind kind, std::string lexeme,
                           Literal content) -> ExprPtr {
    Literal copy = content;
    return std::make_unique<LiteralExpr>(
        generated(kind, std::move(lexeme), std::move(copy)),
        std::move(content));
  };
  const auto builtinToken = [&]() -> std::optional<Token> {
    return isInteger(type) ? builtinTypeToken(type, anchor) : std::nullopt;
  };
  if (const auto *text = std::get_if<std::string>(&value)) {
    if (type.kind != SemanticType::StringView &&
        type.kind != SemanticType::CString) {
      return nullptr;
    }
    return literal(TokenKind::STRING_LITERAL, "\"" + *text + "\"",
                   Literal{*text});
  }
  if (const auto *flag = std::get_if<bool>(&value)) {
    if (type != SemanticType::Bool) {
      return nullptr;
    }
    return literal(*flag ? TokenKind::TRUE : TokenKind::FALSE,
                   *flag ? "true" : "false", Literal{*flag});
  }
  if (const auto *character = std::get_if<CharacterLiteral>(&value)) {
    if (type != SemanticType::Char) {
      return nullptr;
    }
    return literal(TokenKind::CHARACTER_LITERAL,
                   std::string("'") + static_cast<char>(character->value) + "'",
                   Literal{*character});
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&value)) {
    if ((type != SemanticType::Float && type != SemanticType::Double) ||
        (floating->format == BinaryFloatFormat::Binary32) !=
            (type == SemanticType::Float)) {
      return nullptr;
    }
    return literal(TokenKind::FLOAT_LITERAL, "0.0", Literal{*floating});
  }
  if (const auto *integer = std::get_if<ConstantInteger>(&value)) {
    const std::optional<Token> target = builtinToken();
    if (!target) {
      return nullptr;
    }
    ExprPtr magnitude =
        literal(TokenKind::INT_LITERAL, std::to_string(integer->magnitude),
                Literal{integer->magnitude});
    if (integer->negative) {
      magnitude = std::make_unique<Unary>(generated(TokenKind::MINUS, "-"),
                                          std::move(magnitude));
    }
    return std::make_unique<Conversion>(TypeRef(*target),
                                        generated(TokenKind::LEFT_PAREN, "("),
                                        std::move(magnitude));
  }
  if (const auto *enumerator = std::get_if<ConstantEnum>(&value)) {
    if (type.kind != SemanticType::Enum || enumerator->enumId == 0 ||
        enumerator->enumId > enums.size() ||
        type.enumId != enumerator->enumId) {
      return nullptr;
    }
    const EnumInfo &info = enums[enumerator->enumId - 1];
    const auto found = std::find_if(
        info.enumerators.begin(), info.enumerators.end(),
        [&](const auto &entry) {
          return entry.second.value.negative == enumerator->value.negative &&
                 entry.second.value.magnitude == enumerator->value.magnitude;
        });
    if (found == info.enumerators.end()) {
      return nullptr;
    }
    std::vector<Token> segments;
    for (const std::string &scope : info.namespaceScope) {
      segments.push_back(generated(TokenKind::IDENTIFIER, scope));
    }
    segments.push_back(generated(TokenKind::IDENTIFIER, info.name.lexeme));
    segments.push_back(generated(TokenKind::IDENTIFIER, found->first));
    return std::make_unique<QualifiedName>(NamePath(std::move(segments)));
  }
  return nullptr;
}

auto SemanticVisitor::Impl::foldAttributeMemberRead(
    const Get &expr, const SemanticType &objectType) -> bool {
  const auto *call = exprAs<Call>(expr.object().get());
  if (call == nullptr || objectType.kind != SemanticType::Class ||
      expr.name().completion) {
    return false;
  }
  const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
  if (resolved == nullptr ||
      (resolved->intrinsic != IntrinsicKind::MetaFieldAttribute &&
       resolved->intrinsic != IntrinsicKind::MetaTypeAttribute)) {
    return false;
  }
  const std::optional<ConstantValue> constant =
      semanticModel.findConstant(*call);
  const auto *aggregate =
      constant ? std::get_if<ConstantAggregateValue>(&*constant) : nullptr;
  if (aggregate == nullptr || aggregate->value == nullptr) {
    return false;
  }
  const MemberInfo *member = findMember(objectType, expr.name());
  if (member == nullptr || member->access != AccessModifier::Public ||
      member->symbol.bindingKind != SemanticBindingKind::Field) {
    return false;
  }
  const SymbolId symbol = toolingSymbolFor(member->symbol);
  const auto field = std::find_if(aggregate->value->fields.begin(),
                                  aggregate->value->fields.end(),
                                  [&](const ConstantAggregateField &candidate) {
                                    return candidate.symbol == symbol;
                                  });
  if (field == aggregate->value->fields.end()) {
    return false;
  }
  ExprPtr spelled =
      constantLiteralExpression(field->value, member->symbol.type, expr.name());
  if (spelled == nullptr) {
    return false;
  }
  std::shared_ptr<const Expr> alias = std::move(spelled);
  synthesizedExpressions.push_back(alias);
  const Expr &target = semanticModel.recordExpressionAlias(expr, alias);
  currentType = analyze(target);
  semanticModel.recordConstant(expr, field->value);
  semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                  .span = tokenSpan(expr.name()),
                                  .kind = SemanticOccurrenceKind::Expression,
                                  .symbol = symbol,
                                  .roles = OccurrenceRole::Reference,
                                  .name = expr.name().lexeme,
                                  .type = member->symbol.type});
  return true;
}

auto SemanticVisitor::Impl::visitConditionalStmt(const ConditionalStmt &stmt)
    -> void {
  if (const StmtList *branch = stmt.activeBranch(target)) {
    analyze(*branch);
  }
}

auto SemanticVisitor::Impl::visitCompileErrorDirective(
    const CompileErrorDirective &stmt) -> void {
  const std::string message = stmt.message().empty()
                                  ? "Compile-time configuration error."
                                  : stmt.message();
  diagnostics.push_back(makeDiagnostic("GTI-S2047", DiagnosticPhase::Semantics,
                                       stmt.directive(), message));
}

auto SemanticVisitor::Impl::visitStaticAssertDecl(const StaticAssertDecl &stmt)
    -> void {
  const DeclarationOrderScope declarationOrder{*this, stmt};
  const std::size_t diagnosticsBefore = diagnostics.size();
  const bool enclosingStaticAssert = analyzingDependentConstantCondition;
  const bool enclosingDependency = constantConditionGenericDependency;
  analyzingDependentConstantCondition = true;
  constantConditionGenericDependency = false;
  const SemanticType conditionType = analyze(stmt.condition());
  const bool genericDependent = constantConditionGenericDependency;
  analyzingDependentConstantCondition = enclosingStaticAssert;
  constantConditionGenericDependency = enclosingDependency;

  if (diagnostics.size() != diagnosticsBefore ||
      conditionType == SemanticType::Unknown) {
    semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Failed);
    return;
  }
  if (conditionType != SemanticType::Bool) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2080", DiagnosticPhase::Semantics,
        staticAssertConditionSpan(stmt),
        "Static assertion condition must have exact type 'bool', but found "
        "'" +
            typeSpelling(conditionType) + "'.");
    appendStaticAssertSubstitutions(diagnostic);
    diagnostics.emplace_back(std::move(diagnostic));
    semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Failed);
    return;
  }
  if (genericDependent) {
    semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Deferred);
    return;
  }

  ConstexprInterpreter context = makeConstexprInterpreter();
  ConstexprEvaluation evaluated =
      evaluateConstexprExpression(stmt.condition(), context);
  const bool *value =
      evaluated ? std::get_if<bool>(&*evaluated.value) : nullptr;
  if (value == nullptr) {
    if (evaluated) {
      evaluated.failure = ConstantEvaluationFailure::InvalidOperands;
      evaluated.value.reset();
    }
    reporter.reportStaticAssertEvaluationFailure(
        stmt, evaluated.failure, evaluated.detail, evaluated.location);
    appendStaticAssertSubstitutions(diagnostics.back());
    semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Failed);
    return;
  }
  if (!*value) {
    std::string message = "Static assertion failed.";
    if (!stmt.message().empty()) {
      message += " " + stmt.message();
    }
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2079", DiagnosticPhase::Semantics,
                       staticAssertConditionSpan(stmt), std::move(message));
    appendStaticAssertSubstitutions(diagnostic);
    diagnostics.emplace_back(std::move(diagnostic));
    semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Failed);
    return;
  }
  semanticModel.recordStaticAssertResult(stmt, StaticAssertResult::Passed);
}

auto SemanticVisitor::Impl::visitConstructorDecl(const ConstructorDecl &stmt)
    -> void {
  const DeclarationOrderScope declarationOrder{*this, stmt};
  const std::size_t constructorDiagnosticStart = diagnostics.size();
  if (!currentClass) {
    report(stmt.name(),
           "Constructors can only be declared in a class or struct.");
    return;
  }

  const ClassInfo &owner = classInfo(*currentClass);
  const ConstructorInfo *constructorInfo = semanticModel.findConstructor(stmt);
  const bool directOwningPayloadTransport =
      stmt.body() != nullptr && constructorInfo != nullptr &&
      constructorInfo->kind == ConstructorKind::Ordinary && !owner.polymorphic;
  bool requirementsValid = true;
  if (constructorInfo != nullptr) {
    beginTypeParameterScope(constructorInfo->genericParameters);
    requirementsValid = beginRequirementScope(constructorInfo);
  }
  const SemanticType ownerType = SemanticType::classType(owner.id);
  const SymbolId symbol = recordToolingSymbol(
      stmt.name(), SymbolKind::Constructor,
      qualifiedName(owner.namespaceScope,
                    owner.name.lexeme + "::" + stmt.name().lexeme),
      ownerType, false, true, constructorAccess(owner, &stmt));
  attachDeclarationDocumentation(symbol, stmt);
  if (!stmt.name().generated) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.name()),
         .kind = SemanticOccurrenceKind::Constructor,
         .symbol = symbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = stmt.name().lexeme,
         .type = ownerType,
         .constructor = &stmt});
  }
  for (const Parameter &parameter : stmt.parameters()) {
    const bool previousAllowPack = allowPackTypeReference;
    allowPackTypeReference = true;
    validateType(parameter.type);
    allowPackTypeReference = previousAllowPack;
    const SemanticType declaredType = typeOf(parameter);
    const SemanticType parameterType =
        parameter.pack && declaredType.kind == SemanticType::TypeParameter
            ? SemanticType::typePack(declaredType.genericParameterId)
            : declaredType;
    validateOwningPayloadEnumByValueBoundary(
        parameterType, parameter.type.name.last(), "constructor parameter",
        directOwningPayloadTransport);
    validateReferencePlacement(parameter.type, true, "constructor parameter",
                               constructorInfo != nullptr &&
                                   constructorInfo->kind ==
                                       ConstructorKind::Move,
                               false, true);
    const bool trustedGuardSpecialMember =
        isDefaultLibraryMutexGuardCarrier(owner) &&
        constructorInfo != nullptr &&
        (constructorInfo->kind == ConstructorKind::Copy ||
         constructorInfo->kind == ConstructorKind::Move);
    if (nestsBorrowedState(parameterType) && !trustedGuardSpecialMember &&
        !isBorrowedCarrierReference(parameterType)) {
      report(parameter.sourceAnchor(),
             "Constructor parameters cannot nest borrowed state in the "
             "current lifetime model.",
             "GTI-S2045");
    }
    const SymbolId parameterSymbol =
        recordParameterBinding(parameter, parameterType);
    BindingInfo parameterInfo =
        bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                       ? AccessMode::Mutable
                                       : AccessMode::ReadOnly);
    if (parameterType.kind == SemanticType::StringView ||
        parameterType.kind == SemanticType::CString) {
      parameterInfo.traits = ownerDependentTextInfo(parameterType).traits;
    }
    parameterInfo.symbol = parameterSymbol;
    semanticModel.record(parameter, std::move(parameterInfo));
    if (parameterType == SemanticType::Void) {
      report(parameter.type.name.last(),
             "Constructor parameters cannot have type void.");
    }
  }

  std::vector<SemanticType> parameterTypes;
  parameterTypes.reserve(stmt.parameters().size());
  for (const Parameter &parameter : stmt.parameters()) {
    parameterTypes.push_back(typeOf(parameter));
  }
  const bool ordinary = constructorInfo != nullptr &&
                        constructorInfo->kind == ConstructorKind::Ordinary;
  const bool hasPack = std::any_of(
      stmt.parameters().begin(), stmt.parameters().end(),
      [](const Parameter &parameter) { return parameter.pack.has_value(); });
  analyzeParameterDefaults(
      stmt.parameters(), parameterTypes,
      constructorInfo == nullptr
          ? std::span<const std::size_t>{}
          : std::span<const std::size_t>(
                constructorInfo->defaultArgumentEffectOwners),
      DefaultArgumentOwnerKind::Constructor, ordinary,
      ordinary ? "this constructor declaration"
               : "copy or move constructor policies",
      hasPack, !instanceClassContextActive);
  const bool constexprDeclarationValid = validateConstexprConstructor(
      stmt, constructorInfo, openClassType(owner.id));

  if (constructorInfo == nullptr ||
      constructorInfo->kind != ConstructorKind::Ordinary || !stmt.body() ||
      !requirementsValid) {
    if (constructorInfo != nullptr) {
      endRequirementScope();
      endTypeParameterScope();
    }
    return;
  }

  const SemanticType enclosingReturnType = currentReturnType;
  const ReceiverMutability enclosingReceiverMutability =
      currentReceiverMutability;
  const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
  LoanFlowState enclosingLoanFlow = std::move(loanFlow);
  std::vector<ValueControlFlowContext> enclosingValueControlFlow =
      std::move(valueControlFlow);
  auto enclosingRawPointerLoops = std::move(valueFlow.rawPointerLoops);
  currentReturnType = SemanticType::Void;
  currentReceiverMutability = ReceiverMutability::Mutable;
  loanFlow = {};
  valueControlFlow.clear();
  ++functionDepth;
  ++constructorDepth;
  currentProgramEffectOwner = {.kind = ProgramEffectOwnerKind::Constructor,
                               .id = constructorInfo->id};
  beginScope();
  const bool collectingRawFields =
      requestSession.collectingRawResult &&
      requestSession.collectingRawResult->constructor == constructorInfo->id;
  auto enclosingRawProgramStorage = saveRawProgramStorage();
  if (collectingRawFields)
    declareReceiverState();

  for (const Parameter &parameter : stmt.parameters()) {
    if (!parameter.name.lexeme.empty()) {
      const SemanticType declaredType = typeOf(parameter);
      const SemanticType parameterType =
          parameter.pack && declaredType.kind == SemanticType::TypeParameter
              ? SemanticType::typePack(declaredType.genericParameterId)
              : declaredType;
      recordProgramCleanupEffects(parameterType, tokenSpan(parameter.name));
      declare(parameter.name, parameterType,
              parameter.mutability == Mutability::Mutable,
              SemanticBindingKind::Parameter, nullptr, &parameter);
      recordParameterEntryLoan(parameter);
    }
  }

  if (collectingRawFields) {
    for (const auto &field : owner.fields) {
      const auto &declaration = *field.declaration;
      if (!declaration.initializer() ||
          std::any_of(stmt.initializers().begin(), stmt.initializers().end(),
                      [&](const auto &initializer) {
                        return initializer.target.name.last().lexeme ==
                               declaration.name().lexeme;
                      }))
        continue;
      const auto member = owner.members.find(declaration.name().lexeme);
      if (member == owner.members.end())
        continue;
      const auto fieldType =
          substituteSymbol(member->second.symbol, openClassType(owner.id)).type;
      analyzeInitializer(declaration.initializer(), fieldType);
      assignRawPointerFields(
          {.root = receiverStateSymbol(),
           .projections = {{.kind = PlaceProjectionKind::Field,
                            .field = toolingSymbolFor(member->second.symbol)}},
           .receiver = true},
          declaration.initializer().get());
    }
  }

  std::unordered_set<std::string> initializedFields;
  std::optional<std::size_t> previousFieldIndex;
  const ClassBaseTypeInfo *base = concreteBase(owner);
  bool initializedBase = false;
  bool sawFieldInitializer = false;
  for (const ConstructorInitializer &initializer : stmt.initializers()) {
    semanticModel.recordFullExpression(initializer);
    const Token &target = initializer.target.name.last();
    std::optional<std::size_t> fieldIndex;
    const VariableDecl *field = nullptr;
    if (initializer.target.name.segments.size() == 1 &&
        initializer.target.arguments.empty()) {
      for (std::size_t index = 0; index < owner.fields.size(); ++index) {
        if (owner.fields[index].declaration->name().lexeme == target.lexeme) {
          fieldIndex = index;
          field = owner.fields[index].declaration;
          break;
        }
      }
    }

    if (field == nullptr && base != nullptr &&
        typeOf(initializer.target, owner.namespaceScope) == base->type) {
      validateType(initializer.target);
      if (initializedBase) {
        report(target,
               "Base '" + typeSpelling(base->type) +
                   "' is initialized more than once.",
               "GTI-S2040");
      }
      if (sawFieldInitializer) {
        report(target,
               "The state-bearing base initializer must precede field "
               "initializers.",
               "GTI-S2040");
      }
      initializedBase = true;
      analyzeBaseConstructorInitializer(initializer, base->type);
      continue;
    }

    sawFieldInitializer = true;
    const auto inserted = initializedFields.insert(target.lexeme);
    if (!inserted.second) {
      report(target, "Constructor field '" + target.lexeme +
                         "' is initialized more than once.");
    }
    SymbolId fieldSymbol = 0;
    if (field != nullptr) {
      const auto member = owner.members.find(target.lexeme);
      if (member != owner.members.end()) {
        fieldSymbol = toolingSymbolFor(member->second.symbol);
        semanticModel.recordOccurrence(
            {.sourceUnit = currentSourceUnit,
             .span = tokenSpan(target),
             .kind = SemanticOccurrenceKind::Symbol,
             .symbol = fieldSymbol,
             .roles = OccurrenceRole::Reference | OccurrenceRole::Write,
             .name = target.lexeme,
             .type = member->second.symbol.type,
             .traits = typeTraits(member->second.symbol.type),
             .access = member->second.symbol.assignable ? AccessMode::Mutable
                                                        : AccessMode::ReadOnly,
             .mutableBinding = member->second.symbol.assignable,
             .bindingKind = SemanticBindingKind::Field});
        semanticModel.record(
            initializer,
            ResolvedConstructorInitializerInfo{
                .kind = ConstructorInitializerTargetKind::Field,
                .targetType = member->second.symbol.type,
                .field = fieldSymbol,
                .storesReference =
                    member->second.symbol.type.kind == SemanticType::Reference,
                .borrowAccess =
                    isDefaultLibraryMutexGuardCarrier(owner)
                        ? AccessMode::Mutable
                        : member->second.symbol.type.referenceAccess});
      }
    }
    if (field == nullptr) {
      report(target,
             "Unknown constructor initializer target '" +
                 typeRefSpelling(initializer.target) +
                 "'; expected an immediate field" +
                 (base == nullptr ? std::string(".")
                                  : " or the direct base '" +
                                        typeSpelling(base->type) + "'."));
    } else {
      if (previousFieldIndex && *fieldIndex < *previousFieldIndex) {
        report(target, "Constructor initializers must follow field declaration "
                       "order.");
      }
      previousFieldIndex = fieldIndex;
    }

    const bool enclosingConstructorInitializer =
        analyzingConstructorInitializer;
    analyzingConstructorInitializer = true;
    const SemanticType fieldType =
        field == nullptr ? SemanticType::Unknown
                         : typeOf(field->type(),
                                  field->isMutable() ? Mutability::Mutable
                                                     : Mutability::Immutable,
                                  owner.namespaceScope);
    SemanticType valueType = SemanticType::Unknown;
    if (initializer.arguments.size() != 1) {
      report(target,
             "A field constructor initializer requires exactly one "
             "argument.",
             "GTI-S2012");
      for (const ExprPtr &argument : initializer.arguments) {
        valueType = analyze(argument);
      }
    } else {
      const SemanticType initializerTarget =
          fieldType.kind == SemanticType::Reference &&
                  fieldType.arguments.size() == 1
              ? fieldType.arguments.front()
              : fieldType;
      valueType = field == nullptr
                      ? analyze(initializer.arguments.front())
                      : analyzeInitializer(initializer.arguments.front(),
                                           initializerTarget, true);
    }
    analyzingConstructorInitializer = enclosingConstructorInitializer;
    if (field != nullptr && initializer.arguments.size() == 1 &&
        fieldType.kind == SemanticType::Reference) {
      forceProgramStorageBoundaryAccess(initializer.arguments.front());
      validateReferenceBinding(fieldType, valueType,
                               initializer.arguments.front());
    } else if (field != nullptr && initializer.arguments.size() == 1 &&
               !recursiveLayoutFields.contains(field) &&
               !isOwnershipAssignable(fieldType, valueType,
                                      initializer.arguments.front())) {
      if (fieldType == valueType && isMoveOnlyOwnerType(fieldType)) {
        const auto *source =
            exprAs<Variable>(initializer.arguments.front().get());
        const std::string sourceName =
            source == nullptr ? "value" : source->name().lexeme;
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2003", DiagnosticPhase::Semantics,
                           expressionToken(initializer.arguments.front()),
                           "Cannot initialize field '" + target.lexeme +
                               "' by copying move-only type '" +
                               typeSpelling(fieldType) + "'.");
        diagnostic.hints.emplace_back(
            "Transfer ownership explicitly with std::move(" + sourceName +
            "), or initialize the field from a fresh value.");
        diagnostics.emplace_back(std::move(diagnostic));
      } else {
        report(expressionToken(initializer.arguments.front()),
               "Cannot initialize field '" + target.lexeme + "' of type '" +
                   typeSpelling(fieldType) + "' with a value of type '" +
                   typeSpelling(valueType) + "'.",
               "GTI-S2003");
      }
    } else if (field != nullptr && initializer.arguments.size() == 1 &&
               fieldType != valueType && isInteger(fieldType) &&
               isInteger(valueType)) {
      // The accepted constant widening re-types the literal itself: the
      // constructor-stage lowering demands the field type on its staged
      // operand, and a range-checked constant needs no conversion
      // machinery. Only the direct (possibly grouped) literal re-types;
      // isAssignable already proved the value fits the field.
      const Expr *argument = initializer.arguments.front().get();
      std::vector<const Expr *> chain;
      while (const auto *grouping = exprAs<Grouping>(argument)) {
        chain.push_back(argument);
        argument = grouping->expression().get();
      }
      if (exprAs<LiteralExpr>(argument) != nullptr) {
        chain.push_back(argument);
        for (const Expr *node : chain) {
          if (const ExpressionInfo *info =
                  semanticModel.findExpression(*node)) {
            ExpressionInfo widened = *info;
            widened.type = fieldType;
            semanticModel.record(*node, widened);
          }
        }
      }
    }
    if (field != nullptr && fieldSymbol != 0 &&
        initializer.arguments.size() == 1) {
      if (collectingRawFields)
        assignRawPointerFields(
            {.root = receiverStateSymbol(),
             .projections = {{.kind = PlaceProjectionKind::Field,
                              .field = fieldSymbol}},
             .receiver = true},
            initializer.arguments.front().get());
      std::optional<std::size_t> ownedParameter;
      if (const ExprPtr *moved =
              explicitMoveSource(initializer.arguments.front())) {
        const auto *variable = exprAs<Variable>(moved->get());
        const Symbol *source =
            variable == nullptr ? nullptr : resolve(variable->name());
        if (source != nullptr && source->parameterDeclaration != nullptr &&
            source->type == fieldType) {
          const auto parameter =
              std::find_if(stmt.parameters().begin(), stmt.parameters().end(),
                           [&](const Parameter &candidate) {
                             return &candidate == source->parameterDeclaration;
                           });
          if (parameter != stmt.parameters().end() &&
              !parameter->type.reference && !parameter->pack) {
            ownedParameter = static_cast<std::size_t>(
                std::distance(stmt.parameters().begin(), parameter));
          }
        }
      }
      semanticModel.record(
          initializer,
          ResolvedConstructorInitializerInfo{
              .kind = ConstructorInitializerTargetKind::Field,
              .targetType = fieldType,
              .field = fieldSymbol,
              .storesReference = fieldType.kind == SemanticType::Reference,
              .borrowAccess = isDefaultLibraryMutexGuardCarrier(owner)
                                  ? AccessMode::Mutable
                                  : borrowAccess(fieldType),
              .ownedParameter = ownedParameter});
    }
  }

  if (base != nullptr && !initializedBase) {
    const ClassInfo *baseOwner = classInfo(base->type);
    if (baseOwner != nullptr &&
        defaultConstructorCandidateCount(*baseOwner) > 1) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2013", DiagnosticPhase::Semantics, stmt.name(),
          "Implicit construction of base '" + typeSpelling(base->type) +
              "' is ambiguous; multiple constructors accept zero explicit "
              "arguments.");
      for (const ConstructorInfo &candidate : baseOwner->constructors) {
        if (candidate.requiredParameterCount == 0 &&
            candidate.declaration != nullptr) {
          diagnostic.related.push_back(
              {tokenSpan(candidate.declaration->name()),
               "Candidate: " + constructorSignatureSpelling(candidate)});
        }
      }
      diagnostic.hints.emplace_back(
          "Write an explicit base initializer with arguments that select "
          "one constructor.");
      diagnostics.emplace_back(std::move(diagnostic));
    } else if (!baseHasAccessibleDefaultConstructor(*base, owner.id)) {
      report(stmt.name(),
             "Constructor must explicitly initialize base '" +
                 typeSpelling(base->type) +
                 "' because it has no accessible default constructor.",
             "GTI-S2040");
    } else {
      recordProgramDefaultConstructionEffects(base->type,
                                              tokenSpan(stmt.name()));
    }
  }

  for (const FieldInfo &field : owner.fields) {
    const SemanticType fieldType = typeOf(field.declaration->type());
    const bool nominalDefault =
        semanticModel.implicitFieldInitializer(*field.declaration) &&
        fieldType.kind == SemanticType::Class &&
        isDefaultInitializable(fieldType, true);
    if (!field.declaration->initializer() && !nominalDefault &&
        !initializedFields.contains(field.declaration->name().lexeme)) {
      report(field.declaration->name(),
             "Field must be initialized by this constructor.");
    }
  }

  analyze(stmt.body()->statements());
  if (collectingRawFields)
    recordRawConstructorResult(stmt);
  finalizeLoanFlow();
  endScope();
  restoreRawProgramStorage(std::move(enclosingRawProgramStorage));
  if (stmt.isConstexpr() && constexprDeclarationValid &&
      !hasErrorDiagnostics(
          std::span(diagnostics).subspan(constructorDiagnosticStart))) {
    constexprConstructorsAnalyzed.insert(&stmt);
  }
  --constructorDepth;
  --functionDepth;
  currentReceiverMutability = enclosingReceiverMutability;
  loanFlow = std::move(enclosingLoanFlow);
  valueControlFlow = std::move(enclosingValueControlFlow);
  valueFlow.rawPointerLoops = std::move(enclosingRawPointerLoops);
  currentReturnType = enclosingReturnType;
  currentProgramEffectOwner = enclosingEffectOwner;
  endRequirementScope();
  endTypeParameterScope();
}

auto SemanticVisitor::Impl::visitDestructorDecl(const DestructorDecl &stmt)
    -> void {
  const DeclarationOrderScope declarationOrder{*this, stmt};
  if (!currentClass) {
    report(stmt.tilde(),
           "Destructors can only be declared in a class or struct.",
           "GTI-S2021");
    return;
  }

  const ClassInfo &owner = classInfo(*currentClass);
  const SymbolId symbol = recordToolingSymbol(
      stmt.name(), SymbolKind::Destructor,
      qualifiedName(owner.namespaceScope,
                    owner.name.lexeme + "::~" + stmt.name().lexeme),
      SemanticType::classType(*currentClass), false, true,
      AccessModifier::Public);
  attachDeclarationDocumentation(symbol, stmt);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.name()),
       .kind = SemanticOccurrenceKind::Destructor,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = stmt.name().lexeme,
       .type = SemanticType::classType(*currentClass),
       .destructor = &stmt});

  // A defaulted destructor selects the compiler-generated structural
  // operation. It has no source body, cleanup effect owner, or active-drop
  // state of its own.
  if (stmt.specifier() || !stmt.body()) {
    return;
  }

  const SemanticType enclosingReturnType = currentReturnType;
  const ReceiverMutability enclosingReceiverMutability =
      currentReceiverMutability;
  const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
  LoanFlowState enclosingLoanFlow = std::move(loanFlow);
  std::vector<ValueControlFlowContext> enclosingValueControlFlow =
      std::move(valueControlFlow);
  currentReturnType = SemanticType::Void;
  auto enclosingRawPointerLoops = std::move(valueFlow.rawPointerLoops);
  currentReceiverMutability = ReceiverMutability::Mutable;
  loanFlow = {};
  valueControlFlow.clear();
  ++functionDepth;
  ++destructorDepth;
  currentProgramEffectOwner = {.kind = ProgramEffectOwnerKind::Destructor,
                               .id = owner.id};
  auto enclosingRawProgramStorage = saveRawProgramStorage();
  beginScope();
  declareReceiverState();
  if (Symbol *receiver = receiverStateSymbol();
      receiver != nullptr && isDirectStoredReferenceType(receiver->type))
    recordStoredEntryDependency(*receiver, 0, receiver->type, true, &stmt);

  analyze(stmt.body()->statements());
  finalizeLoanFlow();
  recordProgramCleanupEffects(SemanticType::classType(owner.id),
                              tokenSpan(stmt.tilde()), false);

  endScope();
  --destructorDepth;
  --functionDepth;
  restoreRawProgramStorage(std::move(enclosingRawProgramStorage));
  currentReceiverMutability = enclosingReceiverMutability;
  loanFlow = std::move(enclosingLoanFlow);
  valueControlFlow = std::move(enclosingValueControlFlow);
  valueFlow.rawPointerLoops = std::move(enclosingRawPointerLoops);
  currentReturnType = enclosingReturnType;
  currentProgramEffectOwner = enclosingEffectOwner;
}

auto SemanticVisitor::Impl::visitDoWhileStmt(const DoWhileStmt &stmt) -> void {
  noteUnsupportedNestedLoanControlFlow();
  beginLoanFlowLoop(stmt);
  beginRawPointerLoop();
  const ScopeStack beforeLoop = scopes;
  ++loopDepth;
  beginValueControlFlow(ValueControlFlowKind::Loop, stmt);
  analyzeLoopBody(stmt.body());
  const ScopeStack bodyScopes = scopes;
  ValueControlFlowContext valueFlow = endValueControlFlow();
  const FlowSummary bodyFlow = summarizeFlow(stmt.body().get());
  std::vector<ScopeStack> conditionInputs;
  if (bodyFlow.canFallThrough) {
    conditionInputs.push_back(bodyScopes);
  }
  for (ScopeStack &continueState : valueFlow.continueStates) {
    conditionInputs.push_back(std::move(continueState));
  }

  semanticModel.recordFullExpression(stmt, stmt.condition());
  SemanticType conditionType = SemanticType::Unknown;
  ScopeStack afterCondition = beforeLoop;
  if (!conditionInputs.empty()) {
    scopes = mergedValueStates(beforeLoop, conditionInputs);
    conditionType =
        analyzeExpectedCallableResult(stmt.condition(), SemanticType::Bool);
    afterCondition = scopes;
  } else {
    scopes = beforeLoop;
    conditionType = analyzeExpectedCallableResultMoveUnreachable(
        stmt.condition(), SemanticType::Bool);
  }
  requireBool(stmt.condition(), conditionType,
              expressionToken(stmt.condition()),
              "Do-while condition must be bool.");

  const std::optional<bool> condition = constantBoolean(stmt.condition());
  if (!conditionInputs.empty() && condition != false) {
    validateLoopBackedge(beforeLoop, afterCondition,
                         expressionToken(stmt.condition()));
  }

  const ScopeStack loanScopes =
      conditionInputs.empty() ? bodyScopes : afterCondition;
  std::vector<ScopeStack> exitStates;
  if (!conditionInputs.empty() && condition != true) {
    exitStates.push_back(afterCondition);
  }
  for (ScopeStack &breakState : valueFlow.breakStates) {
    exitStates.push_back(std::move(breakState));
  }
  const ScopeStack moveScopes = exitStates.empty()
                                    ? beforeLoop
                                    : mergedValueStates(beforeLoop, exitStates);
  scopes = loanScopes;
  applyMoveStates(moveScopes);
  --loopDepth;
  endRawPointerLoop(afterCondition,
                    !conditionInputs.empty() && condition != false);
  endLoanFlowLoop();
}

auto SemanticVisitor::Impl::visitEmptyStmt(const EmptyStmt &) -> void {}

auto SemanticVisitor::Impl::visitExternCDecl(const ExternCDecl &stmt) -> void {
  analyze(stmt.declarations());
}

auto SemanticVisitor::Impl::visitExpressionStmt(const ExpressionStmt &stmt)
    -> void {
  semanticModel.recordFullExpression(stmt, stmt.expression());
  const SemanticType resultType = analyze(stmt.expression());
  const Call *call = directCall(stmt.expression());

  if (stmt.discardAttribute()) {
    if (call == nullptr) {
      report(*stmt.discardAttribute(),
             "'[[discard]]' can only be applied to a function call.");
    } else if (resultType == SemanticType::Void) {
      report(*stmt.discardAttribute(),
             "'[[discard]]' cannot be applied to a void function call.");
    }
    return;
  }

  const auto *selected =
      call == nullptr ? nullptr : semanticModel.findCall(*call);
  const auto *callOperator =
      call == nullptr ? nullptr : semanticModel.findOperator(*call);
  const auto *target =
      semanticModel.findFunction(selected       ? selected->function
                                 : callOperator ? callOperator->function
                                                : 0);
  if (call != nullptr && !(target && target->discardableResult) &&
      resultType != SemanticType::Void && resultType != SemanticType::Unknown &&
      !expressionNeverReturns(stmt.expression().get())) {
    Diagnostic diagnostic =
        makeDiagnostic(DiagnosticId::IgnoredReturnValue, call->paren());
    SourceSpan insertion = tokenSpan(stmt.expressionStart());
    insertion.end = insertion.start;
    diagnostic.fixes.push_back(
        {.span = std::move(insertion),
         .replacement = "[[discard]] ",
         .message = "Explicitly discard the return value",
         .preferred = false});
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::visitForStmt(const ForStmt &stmt) -> void {
  noteUnsupportedNestedLoanControlFlow();
  beginLoanFlowLoop(stmt);
  beginScope();
  analyze(stmt.initializer());
  beginRawPointerLoop();
  const ScopeStack beforeCondition = scopes;
  if (stmt.condition()) {
    semanticModel.recordFullExpression(stmt, stmt.condition());
    const SemanticType conditionType =
        analyzeExpectedCallableResult(stmt.condition(), SemanticType::Bool);
    requireBool(stmt.condition(), conditionType,
                expressionToken(stmt.condition()),
                "For-loop condition must be bool.");
  }
  const ScopeStack beforeLoop = scopes;
  const std::optional<bool> condition = stmt.condition()
                                            ? constantBoolean(stmt.condition())
                                            : std::optional<bool>(true);
  ++loopDepth;
  beginValueControlFlow(ValueControlFlowKind::Loop, stmt);
  if (condition == false) {
    analyzeMoveUnreachable(stmt.body());
  } else {
    analyzeLoopBody(stmt.body());
  }
  const ScopeStack bodyScopes = scopes;
  ValueControlFlowContext valueFlow = endValueControlFlow();
  const FlowSummary bodyFlow = summarizeFlow(stmt.body().get());
  std::vector<ScopeStack> incrementInputs;
  if (condition != false && bodyFlow.canFallThrough) {
    incrementInputs.push_back(bodyScopes);
  }
  if (condition != false) {
    for (ScopeStack &continueState : valueFlow.continueStates) {
      incrementInputs.push_back(std::move(continueState));
    }
  }

  ScopeStack afterIteration = beforeLoop;
  semanticModel.recordFullExpression(stmt, stmt.increment());
  if (!incrementInputs.empty()) {
    scopes = mergedValueStates(beforeLoop, incrementInputs);
    analyze(stmt.increment());
    afterIteration = scopes;
    validateLoopBackedge(beforeCondition, afterIteration,
                         expressionToken(stmt.condition() ? stmt.condition()
                                                          : stmt.increment()));
  } else {
    scopes = beforeLoop;
    analyzeMoveUnreachable(stmt.increment());
  }
  --loopDepth;

  const ScopeStack loanScopes =
      mergedValueStates(beforeLoop, {beforeLoop, bodyScopes, afterIteration});
  std::vector<ScopeStack> exitStates;
  if (condition != true) {
    exitStates.push_back(beforeLoop);
    if (!incrementInputs.empty() && condition != false) {
      exitStates.push_back(afterIteration);
    }
  }
  for (ScopeStack &breakState : valueFlow.breakStates) {
    exitStates.push_back(std::move(breakState));
  }
  const ScopeStack moveScopes = exitStates.empty()
                                    ? beforeLoop
                                    : mergedValueStates(beforeLoop, exitStates);
  scopes = loanScopes;
  applyMoveStates(moveScopes);
  endRawPointerLoop(afterIteration,
                    !incrementInputs.empty() && condition != false);
  endScope();
  endLoanFlowLoop();
}

auto SemanticVisitor::Impl::visitRangeForStmt(const RangeForStmt &stmt)
    -> void {
  analyze(stmt.lowered());

  const bool mutableElementRequested =
      stmt.bindingMutability() == Mutability::Mutable &&
      stmt.bindingType().reference.has_value();
  bool reportedMutableRangeContract = false;
  if (mutableElementRequested && stmt.ownerBinding() != nullptr) {
    const ExprPtr &source = stmt.ownerBinding()->initializer();
    const ExpressionInfo *sourceInfo =
        source ? semanticModel.findExpression(*source) : nullptr;
    if (sourceInfo != nullptr && sourceInfo->category == ValueCategory::Place &&
        sourceInfo->access != AccessMode::Mutable) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2083", DiagnosticPhase::Semantics, stmt.bindingName(),
          "Mutable range iteration requires a mutable owner or mutable "
          "range reference.");
      diagnostic.hints.emplace_back(
          "Declare the range owner mutable, or use a read-only element "
          "reference without 'mut'.");
      diagnostics.emplace_back(std::move(diagnostic));
      reportedMutableRangeContract = true;
    }
  }

  const BindingInfo *owner =
      stmt.ownerBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.ownerBinding());
  const BindingInfo *range =
      stmt.rangeBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.rangeBinding());
  const BindingInfo *iterator =
      stmt.iteratorBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.iteratorBinding());
  const BindingInfo *sentinel =
      stmt.sentinelBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.sentinelBinding());
  const BindingInfo *element =
      stmt.elementBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.elementBinding());
  const BindingInfo *elementOwner =
      stmt.elementOwnerBinding() == nullptr
          ? nullptr
          : semanticModel.findBinding(*stmt.elementOwnerBinding());
  const VariableDecl *accessBinding = stmt.elementOwnerBinding() == nullptr
                                          ? stmt.elementBinding()
                                          : stmt.elementOwnerBinding();

  SemanticType rangeType =
      range == nullptr ? SemanticType::Unknown : range->type;
  if (rangeType.kind == SemanticType::Reference &&
      rangeType.arguments.size() == 1) {
    SemanticType referent = rangeType.arguments.front();
    rangeType = std::move(referent);
  }
  const bool symbolicReadRange =
      symbolicRangeMemberReturnType(rangeType, "begin").has_value();
  const bool symbolicMutableRange =
      symbolicRangeMemberReturnType(rangeType, "begin_mut").has_value();
  if (rangeType != SemanticType::Unknown &&
      rangeType.kind != SemanticType::Array &&
      !(rangeType.kind == SemanticType::StringView &&
        textViewMemberFactory("begin").has_value() &&
        textViewMemberFactory("end").has_value()) &&
      rangeType.kind != SemanticType::Class && !symbolicReadRange) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2083", DiagnosticPhase::Semantics, stmt.colon(),
        std::string(mutableElementRequested ? "Mutable" : "Read-only") +
            " range iteration requires a fixed array or a class "
            "implementing the exact structural iterator protocol; found '" +
            typeSpelling(rangeType) + "'.");
    diagnostic.hints.emplace_back(
        mutableElementRequested
            ? "Provide exact begin_mut()/end(), comparison, mutable "
              "dereference, and prefix-increment operations, or iterate a "
              "mutable fixed array."
            : "Provide exact read-only begin()/end(), comparison, "
              "dereference, and prefix-increment operations, or iterate a "
              "fixed array.");
    diagnostics.emplace_back(std::move(diagnostic));
  }

  const auto selectedCall = [&](const ExprPtr &expression) -> FunctionId {
    const auto *call = exprAs<Call>(expression.get());
    const ResolvedCallInfo *resolved =
        call == nullptr ? nullptr : semanticModel.findCall(*call);
    return resolved == nullptr ? 0 : resolved->function;
  };
  const auto selectedOperator = [&](const ExprPtr &expression) -> FunctionId {
    const ResolvedOperatorInfo *resolved =
        expression ? semanticModel.findOperator(*expression) : nullptr;
    return resolved == nullptr ? 0 : resolved->function;
  };

  SemanticType elementType =
      element == nullptr ? SemanticType::Unknown : element->type;
  AccessMode elementAccess = AccessMode::ReadOnly;
  if (elementType.kind == SemanticType::Reference &&
      elementType.arguments.size() == 1) {
    elementAccess = elementType.referenceAccess;
    SemanticType referent = elementType.arguments.front();
    elementType = std::move(referent);
  }
  const ForStmt *core = stmt.coreLoop();
  const FunctionId beginFunction =
      stmt.iteratorBinding() == nullptr
          ? 0
          : selectedCall(stmt.iteratorBinding()->initializer());
  const FunctionId endFunction =
      stmt.sentinelBinding() == nullptr
          ? 0
          : selectedCall(stmt.sentinelBinding()->initializer());
  const auto *endpointCall =
      stmt.sentinelBinding() == nullptr
          ? nullptr
          : exprAs<Call>(stmt.sentinelBinding()->initializer().get());
  const ResolvedCallInfo *resolvedEndpoint =
      endpointCall == nullptr ? nullptr : semanticModel.findCall(*endpointCall);
  // The hidden sentinel binding can copy a reference result. Its resulting
  // value type alone does not prove that evaluating the endpoint creates no
  // borrow: preserve the selected call's authoritative return/origin facts.
  const bool nonBorrowingEndpoint =
      endFunction != 0 && resolvedEndpoint != nullptr && sentinel != nullptr &&
      resolvedEndpoint->returnType.kind != SemanticType::Reference &&
      !typeTraits(resolvedEndpoint->returnType).containsBorrowedState &&
      resolvedEndpoint->borrowOrigin == BorrowOriginKind::None &&
      resolvedEndpoint->borrowFields.empty() &&
      resolvedEndpoint->borrowArguments.empty() &&
      !resolvedEndpoint->borrowPlace;
  const FunctionId compareFunction =
      core == nullptr ? 0 : selectedOperator(core->condition());
  const FunctionId dereferenceFunction =
      accessBinding == nullptr ? 0
                               : selectedOperator(accessBinding->initializer());
  const FunctionId incrementFunction =
      core == nullptr ? 0 : selectedOperator(core->increment());
  const ResolvedOperatorInfo *dereference =
      accessBinding == nullptr || !accessBinding->initializer()
          ? nullptr
          : semanticModel.findOperator(*accessBinding->initializer());
  const bool exclusiveInlineProxy =
      mutableElementRequested && iterator != nullptr &&
      iterator->type.kind == SemanticType::Class &&
      typeTraits(iterator->type).containsBorrowedState &&
      elementType.kind == SemanticType::Class &&
      typeTraits(elementType).containsBorrowedState && dereference != nullptr &&
      dereference->borrowOrigin == BorrowOriginKind::Receiver &&
      dereference->borrowFields.empty();
  if (exclusiveInlineProxy && resolvedEndpoint != nullptr &&
      (!nonBorrowingEndpoint || !resolvedEndpoint->parameterTypes.empty() ||
       !resolvedEndpoint->defaultArguments.empty() ||
       resolvedEndpoint->neverReturns ||
       resolvedEndpoint->dispatch != CallDispatch::Static ||
       resolvedEndpoint->receiverMode != CallReceiverMode::ReadOnlyPlace)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2083", DiagnosticPhase::Semantics, stmt.colon(),
        "Mutable inline-proxy range iteration requires a statically selected "
        "read-only endpoint call that can return an independent sentinel "
        "without arguments or a borrowed result.");
    if (resolvedEndpoint->declaration != nullptr)
      diagnostic.related.push_back(
          {tokenSpan(resolvedEndpoint->declaration->name()),
           "This endpoint cannot be evaluated while the exclusive cursor is "
           "temporarily inaccessible."});
    diagnostic.hints.emplace_back(
        "Provide a read-only end() that returns an independent sentinel "
        "value without default arguments or virtual dispatch; an empty "
        "variadic endpoint is valid when its resolved call has no arguments.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const bool mutableProtocolAvailable =
      rangeType.kind == SemanticType::Array || symbolicMutableRange ||
      (beginFunction != 0 && endFunction != 0 && compareFunction != 0 &&
       dereferenceFunction != 0 && incrementFunction != 0 &&
       elementAccess == AccessMode::Mutable);
  if (mutableElementRequested && !mutableProtocolAvailable &&
      !reportedMutableRangeContract) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2083", DiagnosticPhase::Semantics, stmt.bindingName(),
        "This range does not provide mutable element loans for a mutable "
        "reference binding.");
    diagnostic.hints.emplace_back(
        "Provide begin_mut() and a mutable dereference path that returns "
        "'mut T&', or use a read-only element reference.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const auto *sourceCall =
      stmt.ownerBinding() == nullptr
          ? nullptr
          : exprAs<Call>(stmt.ownerBinding()->initializer().get());
  const ResolvedCallInfo *resolvedSource =
      sourceCall == nullptr ? nullptr : semanticModel.findCall(*sourceCall);
  const bool expectedPayloadProjection =
      resolvedSource != nullptr &&
      resolvedSource->intrinsic == IntrinsicKind::ExpectedValue &&
      resolvedSource->borrowOrigin == BorrowOriginKind::Receiver;
  semanticModel.record(
      stmt, {.strategy = rangeType.kind == SemanticType::Array
                             ? RangeIterationStrategy::FixedArray
                             : RangeIterationStrategy::Structural,
             .rangeType = rangeType,
             .iteratorType =
                 iterator == nullptr ? SemanticType::Unknown : iterator->type,
             .sentinelType =
                 sentinel == nullptr ? SemanticType::Unknown : sentinel->type,
             .elementType = elementType,
             .elementAccess = elementAccess,
             .owner = owner == nullptr ? 0 : owner->symbol,
             .rangeCarrier = range == nullptr ? 0 : range->symbol,
             .iterator = iterator == nullptr ? 0 : iterator->symbol,
             .sentinel = sentinel == nullptr ? 0 : sentinel->symbol,
             .element = element == nullptr ? 0 : element->symbol,
             .elementOwner = elementOwner == nullptr ? 0 : elementOwner->symbol,
             .rangeLoan = range == nullptr ? 0 : range->retainedLoan,
             .elementLoan = element == nullptr ? 0 : element->retainedLoan,
             .elementOwnerLoan =
                 elementOwner == nullptr ? 0 : elementOwner->retainedLoan,
             .beginFunction = beginFunction,
             .endFunction = endFunction,
             .compareFunction = compareFunction,
             .dereferenceFunction = dereferenceFunction,
             .incrementFunction = incrementFunction,
             .arrayLength = rangeType.kind == SemanticType::Array &&
                                    rangeType.arrayLengthParameterId == 0
                                ? rangeType.arrayLength
                                : 0,
             .ownsRangeTemporary = owner != nullptr &&
                                   owner->type.kind != SemanticType::Reference,
             .expectedPayloadProjection = expectedPayloadProjection,
             .nonBorrowingEndpoint = nonBorrowingEndpoint,
             .materializesElementValue =
                 elementOwner != nullptr &&
                 elementOwner->type.kind != SemanticType::Reference});
}

auto SemanticVisitor::Impl::visitFunctionDecl(const FunctionDecl &stmt)
    -> void {
  FunctionDemandScope pauseDemand(semanticModel, nullptr);
  const FunctionInfo *definitionInfo = semanticModel.findFunction(stmt);
  if (stmt.definitionOwner() && definitionInfo == nullptr)
    return; // The registration diagnostic owns an unmatched definition.
  struct DefinitionContext {
    Impl &self;
    std::vector<std::string> namespaceScope;
    SourceUnitId unit;
    std::optional<ClassId> owner;
    bool active;
    ~DefinitionContext() {
      if (!active)
        return;
      self.exitDeclarationContext(self.currentClass);
      self.currentNamespace = std::move(namespaceScope);
      self.currentSourceUnit = unit;
      self.currentClass = owner;
    }
  } context{*this, currentNamespace, currentSourceUnit, currentClass,
            stmt.definitionOwner().has_value() && !instanceAnalysisActive};
  if (context.active) {
    validateType(TypeRef(*stmt.definitionOwner()));
    enterDeclarationContext(definitionInfo->namespaceScope,
                            sourceUnitFor(stmt.name()),
                            definitionInfo->ownerClass);
  }
  const DeclarationOrderScope declarationOrder{*this, stmt};
  // A declaration analysed on demand (see ensureBodyAnalyzed) is complete;
  // the lexical pass must not process it a second time. Concrete-instance
  // reanalysis is a separate request and always runs.
  if (!instanceAnalysisActive &&
      !visitedFunctionDeclarations.insert(&stmt).second) {
    return;
  }
  const std::size_t functionDiagnosticStart = diagnostics.size();
  constexprForExpandedBodies = 0;
  const std::vector<GenericParameterInfo> &genericParameters =
      genericParametersFor(stmt);
  beginTypeParameterScope(genericParameters);
  const FunctionInfo *functionInfo = semanticModel.findFunction(stmt);
  const bool staticMethod =
      functionInfo != nullptr && functionInfo->staticMember;
  const bool requirementsValid = beginRequirementScope(functionInfo);
  const Token &functionToken =
      stmt.operatorName() ? stmt.operatorName()->symbol : stmt.name();
  const SymbolId functionSymbol = recordFunctionSymbol(stmt);
  attachDeclarationDocumentation(functionSymbol, stmt);
  OccurrenceRole functionRoles = OccurrenceRole::Declaration;
  if (stmt.body()) {
    functionRoles |= OccurrenceRole::Definition;
  }
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(functionToken),
       .kind = SemanticOccurrenceKind::Function,
       .symbol = functionSymbol,
       .roles = functionRoles,
       .name = stmt.operatorName() ? std::string(operatorSourceSpelling(
                                         stmt.operatorName()->kind))
                                   : stmt.name().lexeme,
       .type = functionInfo == nullptr ? SemanticType::Unknown
                                       : functionInfo->returnType,
       .function = &stmt});
  const bool isEntryPoint = functionInfo != nullptr && functionInfo->entryPoint;
  if (isEntryPoint && stmt.unsafeKeyword()) {
    report(*stmt.unsafeKeyword(),
           "The hosted entry point cannot require an unsafe caller.",
           "GTI-S2055");
  }
  const bool intrinsicDeclaration =
      functionInfo != nullptr && functionInfo->intrinsic != IntrinsicKind::None;
  const auto reportContract = [&](const Token &where, std::string message) {
    diagnostics.push_back(makeDiagnostic(DiagnosticId::InvalidFunctionContract,
                                         where, {std::move(message)}));
  };
  std::unordered_set<std::string> functionAttributeNames;
  for (const auto &contract : stmt.functionAttributes()) {
    const Token &attribute = contract.name;
    if (attribute.lexeme == "thread_confined_callback") {
      if (!functionAttributeNames.insert(attribute.lexeme).second) {
        reportContract(
            attribute,
            "Duplicate function attribute 'thread_confined_callback'.");
      } else if (contract.argument || functionInfo == nullptr ||
                 functionInfo->ownerClass != 0 || isEntryPoint ||
                 !functionInfo->genericParameters.empty() ||
                 functionInfo->parameterPack || functionInfo->requiresUnsafe ||
                 functionInfo->tracksCaller || stmt.hasCLinkage() ||
                 stmt.runtimeBinding() || !stmt.body() || stmt.operatorName()) {
        reportContract(attribute, "A thread_confined_callback contract "
                                  "requires a defined, non-generic, "
                                  "safe namespace-scope GTI function without "
                                  "caller tracking or attribute arguments.");
      }
      continue;
    }
    if (attribute.lexeme == "discardable") {
      if (!functionAttributeNames.insert(attribute.lexeme).second) {
        reportContract(attribute,
                       "Duplicate function attribute 'discardable'.");
      } else if (functionInfo != nullptr &&
                 functionInfo->returnType == SemanticType::Void) {
        reportContract(
            attribute,
            "A discardable contract requires a non-void return type.");
      }
      continue;
    }
    if (attribute.lexeme == "borrowed_from") {
      if (!functionAttributeNames.insert(attribute.lexeme).second) {
        reportContract(attribute,
                       "Duplicate function attribute 'borrowed_from'.");
      } else if (stmt.hasCLinkage() || stmt.runtimeBinding()) {
        reportContract(
            attribute,
            "A borrowed_from contract requires a GTI function or method.");
      } else if (functionInfo == nullptr ||
                 functionInfo->returnType.kind != SemanticType::Reference) {
        reportContract(attribute, "A borrowed_from contract requires a checked "
                                  "reference return type.");
      } else if (!contract.argument) {
        reportContract(attribute, "A borrowed_from contract requires one "
                                  "reference parameter name or 'this'.");
      } else if (functionInfo->returnBorrowOrigin == BorrowOriginKind::None) {
        reportContract(
            *contract.argument,
            "The selected borrow origin must name a reference parameter with "
            "sufficient access, or a non-static receiver.");
      }
      continue;
    }
    if (attribute.lexeme != "noreturn" && attribute.lexeme != "track_caller") {
      reportContract(attribute, "Unknown GTI function attribute '" +
                                    attribute.lexeme + "'.");
    } else if (!functionAttributeNames.insert(attribute.lexeme).second) {
      reportContract(attribute, "Duplicate function attribute '" +
                                    attribute.lexeme + "'.");
    } else if (isEntryPoint && attribute.lexeme == "track_caller") {
      reportContract(attribute,
                     "The hosted entry point has no GTI caller location and "
                     "cannot use [[track_caller]].");
    } else if (stmt.operatorName()) {
      reportContract(
          attribute,
          "Function contracts are not yet supported on operator "
          "declarations; use an ordinary named GTI method or function.");
    } else if (stmt.hasCLinkage() || stmt.runtimeBinding() ||
               stmt.isVirtual() || stmt.isOverride() || stmt.isPure() ||
               (functionInfo != nullptr && functionInfo->virtualMethod) ||
               (!stmt.body() && !intrinsicDeclaration)) {
      reportContract(attribute,
                     "This attribute requires a defined, non-polymorphic GTI "
                     "function.");
    }
  }
  if (sourceGraph != nullptr && currentSourceUnit != 0 &&
      currentSourceUnit != sourceGraph->entryUnit() &&
      currentNamespace.empty() && !currentClass && functionDepth == 0 &&
      stmt.name().lexeme == "main") {
    report(stmt.name(),
           "The main entry point can only be declared in the entry source "
           "file.",
           "GTI-S2025");
  }
  if (!genericParameters.empty() && currentNamespace.empty() && !currentClass &&
      stmt.name().lexeme == "main") {
    report(stmt.name(), "The main entry point cannot be generic.");
  }
  validateRuntimeBinding(stmt);
  const bool enclosingPackTypeReference = allowPackTypeReference;
  allowPackTypeReference = true;
  validateType(stmt.returnType());
  allowPackTypeReference = enclosingPackTypeReference;
  const bool methodDeclaration = currentClass && functionDepth == 0;
  const bool polymorphic =
      methodDeclaration &&
      (stmt.isVirtual() || stmt.isPure() || stmt.isOverride() ||
       (!staticMethod &&
        classInfo(*currentClass).kind == ClassKind::Interface));
  const bool directOwningPayloadTransport =
      stmt.body() != nullptr && !intrinsicDeclaration &&
      !stmt.runtimeBinding() && !stmt.hasCLinkage() && !stmt.operatorName() &&
      !polymorphic;
  if ((stmt.isVirtual() || stmt.isOverride() || stmt.isPure()) &&
      !methodDeclaration) {
    const Token &location =
        stmt.isVirtual() ? *stmt.virtualKeyword()
                         : (stmt.isOverride() ? *stmt.overrideKeyword()
                                              : stmt.pureSpecifier()->equal);
    report(location,
           "Polymorphic specifiers are valid only on class, struct, or "
           "interface methods.",
           "GTI-S2042");
  }
  if (stmt.isStatic() && !methodDeclaration && currentNamespace.empty() &&
      stmt.name().lexeme == "main") {
    report(*stmt.staticKeyword(),
           "The main entry point cannot have internal static linkage.",
           "GTI-S2039");
  }
  if (stmt.requiresClause()) {
    if (stmt.operatorName()) {
      report(stmt.requiresClause()->keyword,
             "Trailing requires clauses are not supported on operator "
             "declarations in the bounded requirements model.",
             "GTI-S2049");
    } else if (polymorphic) {
      report(stmt.requiresClause()->keyword,
             "Trailing requires clauses are not supported on polymorphic "
             "methods in the bounded requirements model.",
             "GTI-S2049");
    }
  }
  validateFunctionPacks(stmt, genericParameters);
  validateOperatorDeclaration(stmt, methodDeclaration);
  const bool hasReturnBorrowSummary =
      functionInfo != nullptr &&
      functionInfo->returnBorrowOrigin != BorrowOriginKind::None;
  const SemanticType declaredReturnType =
      typeOf(stmt.returnType(), stmt.returnMutability());
  validateOwningPayloadEnumByValueBoundary(
      declaredReturnType, stmt.returnType().name.last(), "function return",
      directOwningPayloadTransport);
  validateReferencePlacement(
      stmt.returnType(),
      methodDeclaration || intrinsicDeclaration || hasReturnBorrowSummary ||
          isBorrowReturningType(declaredReturnType),
      methodDeclaration ? "method return type" : "function return type");
  if (nestsBorrowedState(declaredReturnType) &&
      !isFallibleBorrowType(declaredReturnType) &&
      !isBorrowedCarrierReference(declaredReturnType)) {
    report(stmt.name(),
           "Borrowed state cannot be nested in a function return type in "
           "the current lifetime model.",
           "GTI-S2045");
  } else if (isBorrowReturningType(declaredReturnType) &&
             !intrinsicDeclaration && !hasReturnBorrowSummary &&
             functionInfo != nullptr &&
             !stmt.hasFunctionAttribute("borrowed_from")) {
    reportMissingFunctionReturnBorrowSummary(stmt, *functionInfo);
  }
  if (stmt.returnMutability() == Mutability::Mutable) {
    if (!stmt.returnType().reference) {
      report(stmt.name(), "A mutable function return must be a reference type.",
             "GTI-S2017");
    } else if (methodDeclaration && !staticMethod &&
               stmt.receiverMutability() != ReceiverMutability::Mutable) {
      report(stmt.name(),
             "A mutable reference return requires a mutable receiver.",
             "GTI-S2017");
    }
  }
  for (const Parameter &parameter : stmt.parameters()) {
    const bool enclosingParameterPackTypeReference = allowPackTypeReference;
    allowPackTypeReference = true;
    validateType(parameter.type);
    allowPackTypeReference = enclosingParameterPackTypeReference;
    if (!intrinsicDeclaration) {
      validateReferencePlacement(parameter.type, true, "function parameter",
                                 false, false, true);
    }
    const SemanticType declaredType = typeOf(parameter);
    const SemanticType parameterType =
        parameter.pack && declaredType.kind == SemanticType::TypeParameter
            ? SemanticType::typePack(declaredType.genericParameterId)
            : declaredType;
    validateOwningPayloadEnumByValueBoundary(
        parameterType, parameter.type.name.last(), "function parameter",
        directOwningPayloadTransport);
    if (nestsBorrowedState(parameterType) &&
        !isBorrowedCarrierReference(parameterType)) {
      report(parameter.sourceAnchor(),
             "Function parameters cannot nest borrowed state in the "
             "current lifetime model.",
             "GTI-S2045");
    }
    const SymbolId parameterSymbol =
        recordParameterBinding(parameter, parameterType);
    BindingInfo parameterInfo =
        bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                       ? AccessMode::Mutable
                                       : AccessMode::ReadOnly);
    if (parameterType.kind == SemanticType::StringView ||
        parameterType.kind == SemanticType::CString) {
      parameterInfo.traits = ownerDependentTextInfo(parameterType).traits;
    }
    parameterInfo.symbol = parameterSymbol;
    semanticModel.record(parameter, std::move(parameterInfo));
    if (declaredType == SemanticType::Void) {
      report(parameter.type.name.last(),
             "Function parameters cannot have type void.");
    }
  }
  const bool defaultsPermitted =
      !stmt.operatorName() && !stmt.runtimeBinding() && !stmt.hasCLinkage() &&
      !polymorphic && !isEntryPoint && !intrinsicDeclaration;
  std::string_view unsupportedContext = "this declaration";
  if (stmt.operatorName()) {
    unsupportedContext = "operator declarations";
  } else if (stmt.runtimeBinding() || intrinsicDeclaration) {
    unsupportedContext = "runtime or intrinsic declarations";
  } else if (stmt.hasCLinkage()) {
    unsupportedContext = "extern \"C\" declarations";
  } else if (polymorphic) {
    unsupportedContext = "virtual, override, or interface methods";
  } else if (isEntryPoint) {
    unsupportedContext = "the main entry point";
  }
  std::vector<SemanticType> concreteParameterTypes;
  concreteParameterTypes.reserve(stmt.parameters().size());
  for (const Parameter &parameter : stmt.parameters()) {
    const SemanticType declared = typeOf(parameter);
    concreteParameterTypes.push_back(
        parameter.pack && declared.kind == SemanticType::TypeParameter
            ? SemanticType::typePack(declared.genericParameterId)
            : declared);
  }
  analyzeParameterDefaults(
      stmt.parameters(), concreteParameterTypes,
      functionInfo == nullptr ? std::span<const std::size_t>{}
                              : std::span<const std::size_t>(
                                    functionInfo->defaultArgumentEffectOwners),
      DefaultArgumentOwnerKind::Function, defaultsPermitted, unsupportedContext,
      functionInfo != nullptr && functionInfo->parameterPack,
      !instanceAnalysisActive);
  const bool constexprDeclarationValid =
      validateConstexprFunction(stmt, methodDeclaration, intrinsicDeclaration,
                                declaredReturnType, isEntryPoint);
  validateNativeCArrayAttribute(stmt, declaredReturnType);
  validateNativeLinkNameAttribute(stmt);
  validateExternCFunction(stmt, methodDeclaration, declaredReturnType);
  if (isEntryPoint) {
    const SemanticType returnType =
        typeOf(stmt.returnType(), stmt.returnMutability());
    const bool invalidReturnType = returnType != SemanticType::Unknown &&
                                   returnType != SemanticType::Int32;
    ProgramEntryKind entryKind = ProgramEntryKind::None;
    FunctionId appendFunction = 0;
    bool invalidParameters = false;
    if (stmt.parameters().empty()) {
      entryKind = ProgramEntryKind::NoArguments;
    } else if (const std::optional<FunctionId> append =
                   ownedProgramEntryAppendFunction(stmt, *functionInfo)) {
      entryKind = ProgramEntryKind::OwnedArguments;
      appendFunction = *append;
    } else {
      invalidParameters = true;
    }
    if (invalidReturnType || invalidParameters || !stmt.body()) {
      const Token &location =
          invalidReturnType ? stmt.returnType().name.last()
                            : (invalidParameters && !stmt.parameters().empty()
                                   ? stmt.parameters().front().type.name.last()
                                   : stmt.name());
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2032", DiagnosticPhase::Semantics, location,
          "The main entry point requires either 'int main()' or "
          "'int main(int argc, std::vector<std::string> argv)'.");
      diagnostic.hints.emplace_back(
          "Include <std/string> and <std/vector> for the owned argument "
          "form; parameter names and ordinary 'mut' bindings may vary.");
      diagnostics.emplace_back(std::move(diagnostic));
      entryKind = ProgramEntryKind::None;
      appendFunction = 0;
    }
    FunctionInfo updated = *functionInfo;
    updated.entryKind = entryKind;
    updated.entryArgumentAppendFunction = appendFunction;
    semanticModel.record(stmt, std::move(updated));
  }
  if (stmt.receiverMutability() == ReceiverMutability::Consuming) {
    const SemanticType result =
        typeOf(stmt.returnType(), stmt.returnMutability());
    if (result.kind == SemanticType::Reference ||
        typeTraits(result).containsBorrowedState ||
        (functionInfo != nullptr &&
         functionInfo->returnBorrowOrigin != BorrowOriginKind::None)) {
      report(stmt.name(),
             std::string("A consuming ") +
                 (stmt.operatorName() ? "operator" : "method") +
                 " cannot return a reference or tracked borrowed state.",
             "GTI-S2022");
    }
  }
  if (!stmt.body() || !requirementsValid) {
    endRequirementScope();
    endTypeParameterScope();
    return;
  }
  const bool declarationErrors = hasErrorDiagnostics(
      std::span(diagnostics).subspan(functionDiagnosticStart));
  if (!instanceAnalysisActive && analyzedBodies.contains(&stmt)) {
    endRequirementScope();
    endTypeParameterScope();
    return;
  }
  if (deferTrustedBody(stmt)) {
    deferredBodyIndex.emplace(&stmt, deferredBodies.size());
    deferredBodies.push_back(
        {.declaration = &stmt,
         .function = functionInfo == nullptr ? 0 : functionInfo->id,
         .namespaceScope = currentNamespace,
         .sourceUnit = currentSourceUnit,
         .owner = currentClass,
         .declarationErrors = declarationErrors,
         .constexprDeclarationValid = constexprDeclarationValid,
         .declarationOrder = currentDeclarationOrder});
    endRequirementScope();
    endTypeParameterScope();
    return;
  }
  analyzeFunctionBody(stmt, functionInfo, declarationErrors,
                      constexprDeclarationValid);
  endRequirementScope();
  endTypeParameterScope();
}

// The declaration half of visitFunctionDecl has run (symbols, occurrences,
// contracts, signature validation); this is the body half. A trusted body
// deferred by the lexical pass re-enters here later with its declaration's
// namespace, class and generic scopes re-established, so the analysis it
// performs is the one it would have performed in place.
auto SemanticVisitor::Impl::analyzeFunctionBody(
    const FunctionDecl &stmt, const FunctionInfo *functionInfo,
    bool declarationErrors, bool constexprDeclarationValid) -> void {
  // While a preamble is built, a body that bounded analysis would defer is
  // analysed now but reported only once an analysis resumed from the
  // preamble reaches it: its diagnostics and the functions it consulted are
  // bucketed at the end (see reachPreambleBodies).
  const bool bucketForPreamble = buildingPreamble && !instanceAnalysisActive &&
                                 !onDemandBodyAnalysis && functionDepth == 0 &&
                                 lambdaDepth == 0 && reachableDeferrable(stmt);
  std::unordered_set<FunctionId> preambleDemands;
  FunctionDemandScope demandScope(
      semanticModel, bucketForPreamble ? &preambleDemands
                     : trustedBodies == TrustedBodyAnalysis::Reachable
                         ? &demandedFunctions
                         : nullptr);
  if (!instanceAnalysisActive) {
    analyzedBodies.insert(&stmt);
    semanticModel.recordAnalyzedFunctionBody(stmt);
    bodiesInProgress.insert(&stmt);
    if (lateAnalyzedBodies != nullptr)
      lateAnalyzedBodies->push_back(&stmt);
  }
  struct InProgressGuard {
    Impl &impl;
    const FunctionDecl &stmt;
    ~InProgressGuard() { impl.bodiesInProgress.erase(&stmt); }
  } inProgressGuard{*this, stmt};
  const std::size_t bodyDiagnosticStart = diagnostics.size();
  const Token &functionToken =
      stmt.operatorName() ? stmt.operatorName()->symbol : stmt.name();
  const bool isEntryPoint = functionInfo != nullptr && functionInfo->entryPoint;
  const bool methodDeclaration = currentClass && functionDepth == 0;
  const bool staticMethod =
      functionInfo != nullptr && functionInfo->staticMember;
  const auto reportContract = [&](const Token &where, std::string message) {
    diagnostics.push_back(makeDiagnostic(DiagnosticId::InvalidFunctionContract,
                                         where, {std::move(message)}));
  };
  // Errors anywhere in this function so far: the declaration half plus the
  // body analysed here.
  const auto functionErrors = [&] {
    return declarationErrors ||
           hasErrorDiagnostics(
               std::span(diagnostics).subspan(bodyDiagnosticStart));
  };
  const SemanticType enclosingReturnType = currentReturnType;
  const ReceiverMutability enclosingReceiverMutability =
      currentReceiverMutability;
  LoanFlowState enclosingLoanFlow = std::move(loanFlow);
  std::vector<ValueControlFlowContext> enclosingValueControlFlow =
      std::move(valueControlFlow);
  const bool enclosingStaticMemberFunction = currentStaticMemberFunction;
  auto enclosingRawPointerLoops = std::move(valueFlow.rawPointerLoops);
  const FunctionDecl *enclosingFunctionDeclaration = currentFunctionDeclaration;
  const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
  auto enclosingRawProgramStorage = saveRawProgramStorage();
  if (currentClass && functionDepth == 0) {
    currentReceiverMutability = stmt.receiverMutability();
    currentStaticMemberFunction = staticMethod;
  }
  loanFlow = {};
  valueControlFlow.clear();
  currentReturnType = typeOf(stmt.returnType(), stmt.returnMutability());
  currentFunctionDeclaration = &stmt;
  if (functionInfo != nullptr && functionInfo->id != 0) {
    currentProgramEffectOwner = {.kind = ProgramEffectOwnerKind::Function,
                                 .id = functionInfo->id};
  }
  ++functionDepth;
  beginScope();
  if (methodDeclaration && !staticMethod) {
    declareReceiverState();
    if (Symbol *receiver = receiverStateSymbol();
        receiver != nullptr && isDirectStoredReferenceType(receiver->type)) {
      recordStoredEntryDependency(*receiver, 0, receiver->type, true);
    }
  }

  for (const Parameter &parameter : stmt.parameters()) {
    if (!parameter.name.lexeme.empty()) {
      const SemanticType declaredType = typeOf(parameter);
      recordProgramCleanupEffects(declaredType, tokenSpan(parameter.name));
      declare(parameter.name,
              parameter.pack && declaredType.kind == SemanticType::TypeParameter
                  ? SemanticType::typePack(declaredType.genericParameterId)
                  : declaredType,
              parameter.mutability == Mutability::Mutable,
              SemanticBindingKind::Parameter, nullptr, &parameter);
      recordParameterEntryLoan(parameter);
    }
  }

  // A function body owns the parameter scope, so do not add another scope.
  if (functionInfo && functionInfo->explicitReturnBorrowOrigin &&
      functionInfo->returnBorrowOrigin == BorrowOriginKind::Argument) {
    const auto *attribute = stmt.findFunctionAttribute("borrowed_from");
    const auto index = functionInfo->returnBorrowParameter;
    if (attribute && attribute->argument && index < stmt.parameters().size()) {
      const auto *binding = semanticModel.findBinding(stmt.parameters()[index]);
      if (binding)
        semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                        .span = tokenSpan(*attribute->argument),
                                        .kind = SemanticOccurrenceKind::Symbol,
                                        .symbol = binding->symbol,
                                        .roles = OccurrenceRole::Reference,
                                        .name = attribute->argument->lexeme,
                                        .type = binding->type});
    }
  }
  analyze(stmt.body()->statements());
  const FlowSummary functionFlow = summarizeFlow(stmt.body()->statements());
  const bool canFallThrough = functionFlow.canFallThrough;
  if (functionInfo != nullptr && functionInfo->neverReturns &&
      !loanFlow.deferredConstexprSelection && !functionErrors() &&
      (canFallThrough || functionFlow.normalReturn != nullptr)) {
    reportContract(
        functionFlow.normalReturn != nullptr
            ? functionFlow.normalReturn->keyword()
            : functionToken,
        "A [[noreturn]] function cannot return normally or reach the end "
        "of its body.");
  }
  if (currentReturnType != SemanticType::Void &&
      currentReturnType != SemanticType::Unknown && !isEntryPoint &&
      canFallThrough && !loanFlow.deferredConstexprSelection &&
      (functionInfo == nullptr || !functionInfo->neverReturns)) {
    const Token &location =
        stmt.operatorName() ? stmt.operatorName()->keyword : stmt.name();
    report(location,
           "Non-void function can reach the end without returning a value "
           "of type '" +
               typeSpelling(currentReturnType) + "'.",
           "GTI-S2031");
  }
  if (methodDeclaration && !staticMethod && canFallThrough) {
    requireInitializedReceiver(functionToken);
  }
  finalizeLoanFlow();
  endScope();
  if (stmt.isConstexpr() && constexprDeclarationValid && !functionErrors()) {
    constexprDefinitionsAnalyzed.insert(&stmt);
  }
  --functionDepth;
  currentReceiverMutability = enclosingReceiverMutability;
  loanFlow = std::move(enclosingLoanFlow);
  valueControlFlow = std::move(enclosingValueControlFlow);
  valueFlow.rawPointerLoops = std::move(enclosingRawPointerLoops);
  currentStaticMemberFunction = enclosingStaticMemberFunction;
  currentReturnType = enclosingReturnType;
  currentFunctionDeclaration = enclosingFunctionDeclaration;
  currentProgramEffectOwner = enclosingEffectOwner;
  restoreRawProgramStorage(std::move(enclosingRawProgramStorage));
  if (bucketForPreamble) {
    RequestSessionState::PreambleBody &bucket =
        requestSession.preambleBodies[&stmt];
    bucket.demands.assign(preambleDemands.begin(), preambleDemands.end());
    std::sort(bucket.demands.begin(), bucket.demands.end());
    bucket.diagnostics.assign(
        std::make_move_iterator(
            diagnostics.begin() +
            static_cast<std::ptrdiff_t>(bodyDiagnosticStart)),
        std::make_move_iterator(diagnostics.end()));
    diagnostics.resize(bodyDiagnosticStart);
    requestSession.preambleBodyOrder.push_back(&stmt);
  }
}

// A trusted (prelude or standard-library) body is deferred when the policy
// asks for it. Polymorphic members are reached through dispatch rather than a
// resolved declaration, so they are always analysed in place; the entry unit
// is the file being checked and is never deferred.
auto SemanticVisitor::Impl::deferTrustedBody(const FunctionDecl &stmt) const
    -> bool {
  if (trustedBodies == TrustedBodyAnalysis::Complete ||
      requiredFunctionBodies.contains(&stmt) || instanceAnalysisActive ||
      onDemandBodyAnalysis || functionDepth != 0 || lambdaDepth != 0) {
    return false;
  }
  return reachableDeferrable(stmt);
}

auto SemanticVisitor::Impl::reachableDeferrable(const FunctionDecl &stmt) const
    -> bool {
  if (sourceGraph == nullptr || currentSourceUnit == 0 ||
      currentSourceUnit == sourceGraph->entryUnit() ||
      !sourceGraph->isCompilerTrusted(currentSourceUnit)) {
    return false;
  }
  if (stmt.isVirtual() || stmt.isOverride() || stmt.isPure()) {
    return false;
  }
  if (currentClass && classInfo(*currentClass).kind == ClassKind::Interface &&
      !stmt.isStatic()) {
    return false;
  }
  // The language requires a constexpr definition to be analysed before a
  // constant-evaluated call to it; deferral would break that rule.
  if (stmt.isConstexpr()) {
    return false;
  }
  return true;
}

auto SemanticVisitor::Impl::enterDeclarationContext(
    const std::vector<std::string> &namespaceScope, SourceUnitId sourceUnit,
    std::optional<ClassId> owner) -> void {
  currentNamespace = namespaceScope;
  currentSourceUnit = sourceUnit;
  currentClass = owner;
  if (owner) {
    const ClassInfo &info = classInfo(*owner);
    beginTypeParameterScope(info.genericParameters);
    beginScope();
    for (const auto &[name, member] : info.members) {
      scopes.back().emplace(name, member.symbol);
    }
    addEnclosingClassStaticMembers(*owner);
  }
}

auto SemanticVisitor::Impl::addEnclosingClassStaticMembers(ClassId owner)
    -> void {
  // Detached and concrete body analysis must see the same lexical static
  // members as the declaration walk, without capturing an outer receiver.
  for (ClassId parent = classInfo(owner).enclosingClass; parent != 0;
       parent = classInfo(parent).enclosingClass) {
    for (const auto &[name, member] : classInfo(parent).members) {
      if (member.symbol.staticMember)
        scopes.back().emplace(name, member.symbol);
    }
  }
}

auto SemanticVisitor::Impl::exitDeclarationContext(std::optional<ClassId> owner)
    -> void {
  if (owner) {
    endScope();
    endTypeParameterScope();
  }
}

auto SemanticVisitor::Impl::captureBodyLocalState() -> BodyLocalState {
  BodyLocalState state{
      .scopes = std::move(scopes),
      .typeParameterScopes = std::move(typeParameterScopes),
      .valueParameterScopes = std::move(valueParameterScopes),
      .typePackScopes = std::move(typePackScopes),
      .requirementScopes = std::move(requirementScopes),
      .currentNamespace = std::move(currentNamespace),
      .currentSourceUnit = currentSourceUnit,
      .currentClass = currentClass,
      .currentFunctionDeclaration = currentFunctionDeclaration,
      .analyzingFieldInitializer = analyzingFieldInitializer,
      .analyzingConstructorInitializer = analyzingConstructorInitializer,
      .analyzingDefaultArgument = analyzingDefaultArgument,
      .analyzingDependentConstantCondition =
          analyzingDependentConstantCondition,
      .constantConditionGenericDependency = constantConditionGenericDependency,
      .currentDefaultArgumentRoot = currentDefaultArgumentRoot,
      .currentDefaultParameterNames = std::move(currentDefaultParameterNames),
      .analyzingCallCallee = analyzingCallCallee,
      .currentStaticMemberFunction = currentStaticMemberFunction,
      .allowPackTypeReference = allowPackTypeReference,
      .loanFlow = std::move(loanFlow),
      .valueControlFlow = std::move(valueControlFlow),
      .contextualInitializerType = contextualInitializerType,
      .contextualInitializerExpression = contextualInitializerExpression,
      .contextualOperatorIntegerType = contextualOperatorIntegerType,
      .contextualCallableResult = contextualCallableResult,
      .currentReceiverMutability = currentReceiverMutability,
      .suppressedProjectedPlaceReads = std::move(suppressedProjectedPlaceReads),
      .constructorDepth = constructorDepth,
      .destructorDepth = destructorDepth,
      .functionDepth = functionDepth,
      .loopDepth = loopDepth,
      .switchDepth = switchDepth,
      .lambdaDepth = lambdaDepth,
      .lambdaUncapturedLocals = std::move(lambdaUncapturedLocals),
      .structuredBindingElements = std::move(structuredBindingElements),
      .currentType = currentType,
      .currentReturnType = currentReturnType,
      .currentProgramEffectOwner = currentProgramEffectOwner,
      .rawPointerLoops = std::move(valueFlow.rawPointerLoops),
      .rawProgramStorage = saveRawProgramStorage(),
      .measureTopLevelStatements = measureTopLevelStatements,
      .onDemandBodyAnalysis = onDemandBodyAnalysis,
      .constexprForFrames = std::move(constexprForFrames),
      .constexprForExpandedBodies = constexprForExpandedBodies};
  // The clean state a declaration-level body starts from; keep in step with
  // prepareInstanceAnalysis.
  scopes.clear();
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  requirementScopes.clear();
  currentNamespace.clear();
  currentSourceUnit = 0;
  currentClass.reset();
  currentFunctionDeclaration = nullptr;
  analyzingFieldInitializer = false;
  analyzingConstructorInitializer = false;
  analyzingDefaultArgument = false;
  analyzingDependentConstantCondition = false;
  constantConditionGenericDependency = false;
  currentDefaultArgumentRoot = nullptr;
  currentDefaultParameterNames.clear();
  analyzingCallCallee = false;
  currentStaticMemberFunction = false;
  allowPackTypeReference = false;
  loanFlow = {};
  valueControlFlow.clear();
  contextualInitializerType.reset();
  contextualInitializerExpression = nullptr;
  contextualOperatorIntegerType.reset();
  contextualCallableResult.reset();
  currentReceiverMutability = ReceiverMutability::ReadOnly;
  suppressedProjectedPlaceReads.clear();
  constructorDepth = 0;
  destructorDepth = 0;
  functionDepth = 0;
  loopDepth = 0;
  switchDepth = 0;
  lambdaDepth = 0;
  lambdaUncapturedLocals.clear();
  structuredBindingElements.clear();
  currentType = SemanticType::Unknown;
  currentReturnType = SemanticType::Unknown;
  currentProgramEffectOwner = {};
  valueFlow.rawPointerLoops = {};
  measureTopLevelStatements = false;
  constexprForFrames.clear();
  constexprForExpandedBodies = 0;
  return state;
}

auto SemanticVisitor::Impl::restoreBodyLocalState(BodyLocalState state)
    -> void {
  scopes = std::move(state.scopes);
  typeParameterScopes = std::move(state.typeParameterScopes);
  valueParameterScopes = std::move(state.valueParameterScopes);
  typePackScopes = std::move(state.typePackScopes);
  requirementScopes = std::move(state.requirementScopes);
  currentNamespace = std::move(state.currentNamespace);
  currentSourceUnit = state.currentSourceUnit;
  currentClass = state.currentClass;
  currentFunctionDeclaration = state.currentFunctionDeclaration;
  analyzingFieldInitializer = state.analyzingFieldInitializer;
  analyzingConstructorInitializer = state.analyzingConstructorInitializer;
  analyzingDefaultArgument = state.analyzingDefaultArgument;
  analyzingDependentConstantCondition =
      state.analyzingDependentConstantCondition;
  constantConditionGenericDependency = state.constantConditionGenericDependency;
  currentDefaultArgumentRoot = state.currentDefaultArgumentRoot;
  currentDefaultParameterNames = std::move(state.currentDefaultParameterNames);
  analyzingCallCallee = state.analyzingCallCallee;
  currentStaticMemberFunction = state.currentStaticMemberFunction;
  allowPackTypeReference = state.allowPackTypeReference;
  loanFlow = std::move(state.loanFlow);
  valueControlFlow = std::move(state.valueControlFlow);
  contextualInitializerType = state.contextualInitializerType;
  contextualInitializerExpression = state.contextualInitializerExpression;
  contextualOperatorIntegerType = state.contextualOperatorIntegerType;
  contextualCallableResult = state.contextualCallableResult;
  currentReceiverMutability = state.currentReceiverMutability;
  suppressedProjectedPlaceReads =
      std::move(state.suppressedProjectedPlaceReads);
  constructorDepth = state.constructorDepth;
  destructorDepth = state.destructorDepth;
  functionDepth = state.functionDepth;
  loopDepth = state.loopDepth;
  switchDepth = state.switchDepth;
  lambdaDepth = state.lambdaDepth;
  lambdaUncapturedLocals = std::move(state.lambdaUncapturedLocals);
  structuredBindingElements = std::move(state.structuredBindingElements);
  currentType = state.currentType;
  currentReturnType = state.currentReturnType;
  currentProgramEffectOwner = state.currentProgramEffectOwner;
  valueFlow.rawPointerLoops = std::move(state.rawPointerLoops);
  restoreRawProgramStorage(std::move(state.rawProgramStorage));
  measureTopLevelStatements = state.measureTopLevelStatements;
  onDemandBodyAnalysis = state.onDemandBodyAnalysis;
  constexprForFrames = std::move(state.constexprForFrames);
  constexprForExpandedBodies = state.constexprForExpandedBodies;
}

// A caller may need a fact that only the callee's body analysis produces
// (today: whether the body returns a moved parameter, which decides what a
// by-value borrow-carrier result borrows from). Lexical order used to decide
// whether that fact existed yet. Instead the consumer asks for the body:
//
// - a trusted body the lexical pass deferred is analysed now, in its own
//   declaration context and under its own lexical order, so it sees exactly
//   the globals and constexpr definitions that precede it;
// - a declaration the lexical pass has not reached yet is reached now: the
//   caller's body-local state is set aside, the top-level walk advances
//   through every intervening declaration exactly once, in order, analyses
//   the requested declaration in its own position, and the caller resumes.
//
// Recursion (the body is in progress), concrete-instance reanalysis, the
// registration phase, demands from outside a function body, and declarations
// the walk cannot reach (inactive branches, local functions) leave the fact
// unavailable, exactly as before.
auto SemanticVisitor::Impl::ensureBodyAnalyzed(const FunctionInfo &function)
    -> void {
  const FunctionDecl *declaration = function.bodyDeclaration();
  if (!bodyPhaseActive || instanceAnalysisActive || declaration == nullptr ||
      declaration->body() == nullptr || analyzedBodies.contains(declaration) ||
      bodiesInProgress.contains(declaration)) {
    return;
  }
  const auto deferred = std::find_if(
      deferredBodies.begin(), deferredBodies.end(),
      [declaration](const RequestSessionState::DeferredBody &body) {
        return body.declaration == declaration;
      });
  if (deferred != deferredBodies.end()) {
    const RequestSessionState::DeferredBody body = *deferred;
    BodyLocalState enclosing = captureBodyLocalState();
    const std::size_t enclosingOrder = currentDeclarationOrder;
    enterDeclarationContext(body.namespaceScope, body.sourceUnit, body.owner);
    const FunctionInfo *info = semanticModel.findFunction(*declaration);
    beginTypeParameterScope(genericParametersFor(*declaration));
    if (beginRequirementScope(info)) {
      currentDeclarationOrder = body.declarationOrder;
      analyzeFunctionBody(*declaration, info, body.declarationErrors,
                          body.constexprDeclarationValid);
    }
    endRequirementScope();
    endTypeParameterScope();
    exitDeclarationContext(body.owner);
    restoreBodyLocalState(std::move(enclosing));
    currentDeclarationOrder = enclosingOrder;
    return;
  }
  if (visitedFunctionDeclarations.contains(declaration) ||
      !lexicallyReachableFunctions.contains(declaration) ||
      !lexicalAdvanceAllowed()) {
    return;
  }
  BodyLocalState enclosing = captureBodyLocalState();
  const std::size_t enclosingOrder = currentDeclarationOrder;
  const std::size_t diagnosticsBefore = diagnostics.size();
  rebuildLexicalEnvironment();
  advanceLexicalPass(declaration);
  restoreBodyLocalState(std::move(enclosing));
  currentDeclarationOrder = enclosingOrder;
  if (!heldDiagnostics.empty() && diagnostics.size() > diagnosticsBefore) {
    // Keep the demanding leaf's diagnostics first; the advanced
    // declarations' diagnostics follow it once it completes.
    std::vector<Diagnostic> &held = heldDiagnostics.back();
    held.insert(
        held.end(),
        std::make_move_iterator(diagnostics.begin() +
                                static_cast<std::ptrdiff_t>(diagnosticsBefore)),
        std::make_move_iterator(diagnostics.end()));
    diagnostics.erase(diagnostics.begin() +
                          static_cast<std::ptrdiff_t>(diagnosticsBefore),
                      diagnostics.end());
  }
  // The walk may have deferred the body it reached; analyse it now.
  if (!analyzedBodies.contains(declaration)) {
    ensureBodyAnalyzed(function);
  }
}

// ---- The resumable lexical pass -------------------------------------------
//
// The body phase walks the program's declarations in source order, exactly as
// `analyze(program.declarations())` did, but keeps its position in explicit
// frames (the program list, each open namespace body, each open class member
// list, each active conditional branch) so that `ensureBodyAnalyzed` can
// advance the walk from inside a body and resume it afterwards. Frames hold
// no analysis state of their own: the declaration-level environment (program
// scope, namespace path, open class scopes, declaration-level loan flow) is
// rebuilt from the frames whenever they change under a paused leaf. Every
// declaration is processed exactly once, in source order.

auto SemanticVisitor::Impl::indexLexicallyReachableFunctions(
    const StmtList &declarations) -> void {
  for (const StmtPtr &statement : declarations) {
    if (!statement) {
      continue;
    }
    if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      indexLexicallyReachableFunctions(space->declarations());
    } else if (const auto *conditional =
                   stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        indexLexicallyReachableFunctions(*branch);
      }
    } else if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      indexLexicallyReachableFunctions(classDecl->members());
    } else if (const auto *function = stmtAs<FunctionDecl>(statement.get())) {
      lexicallyReachableFunctions.insert(function);
    }
  }
}

auto SemanticVisitor::Impl::runLexicalPass(const StmtList &declarations)
    -> void {
  lexicalFrames.clear();
  lexicalFrames.push_back({.statements = &declarations,
                           .region = loanFlow.nextRegion++,
                           .sourceUnit = currentSourceUnit});
  ++lexicalFrameVersion;
  lexicalLoanFlow = loanFlow;
  advanceLexicalPass(nullptr);
  loanFlow = lexicalLoanFlow;
}

auto SemanticVisitor::Impl::rebuildLexicalEnvironment() -> void {
  scopes.clear();
  typeParameterScopes.clear();
  valueParameterScopes.clear();
  typePackScopes.clear();
  requirementScopes.clear();
  currentNamespace.clear();
  currentClass.reset();
  currentSourceUnit = 0;
  loanFlow = lexicalLoanFlow;
  beginScope();
  ClassId innermostClass = 0;
  for (const auto &frame : lexicalFrames) {
    if (frame.owner != nullptr)
      innermostClass = frame.ownerId;
  }
  for (const RequestSessionState::LexicalFrame &frame : lexicalFrames) {
    currentSourceUnit = frame.sourceUnit;
    if (frame.space != nullptr) {
      currentNamespace.emplace_back(frame.space->name().lexeme);
    }
    if (frame.owner != nullptr) {
      const ClassInfo &info = classInfo(frame.ownerId);
      currentClass = frame.ownerId;
      currentNamespace = info.namespaceScope;
      beginTypeParameterScope(info.genericParameters);
      beginScope();
      for (const auto &[name, member] : info.members) {
        if (frame.ownerId != innermostClass && !member.symbol.staticMember)
          continue;
        scopes.back().emplace(name, member.symbol);
      }
    }
  }
}

auto SemanticVisitor::Impl::enterLexicalNamespace(const NamespaceDecl &space,
                                                  SourceUnitId unit) -> void {
  currentSourceUnit = unit;
  const SymbolId symbol =
      recordToolingSymbol(space.name(), SymbolKind::Namespace,
                          qualifiedName(currentNamespace, space.name().lexeme),
                          SemanticType::Unknown);
  attachDeclarationDocumentation(symbol, space);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(space.name()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = space.name().lexeme});
  currentNamespace.emplace_back(space.name().lexeme);
  lexicalFrames.push_back({.statements = &space.declarations(),
                           .region = loanFlow.nextRegion++,
                           .space = &space,
                           .sourceUnit = unit});
  ++lexicalFrameVersion;
}

auto SemanticVisitor::Impl::enterLexicalClass(const ClassDecl &classDecl,
                                              SourceUnitId unit) -> bool {
  const auto registered = classDeclIds.find(&classDecl);
  if (registered == classDeclIds.end()) {
    return false;
  }
  const DeclarationOrderScope declarationOrder{*this, classDecl};
  currentSourceUnit = unit;
  currentClass = registered->second;
  const ClassInfo &info = classInfo(*currentClass);
  const SemanticType type = openClassType(info.id);
  beginClassBody(classDecl, info, type);
  lexicalFrames.push_back({.statements = &classDecl.members(),
                           .region = loanFlow.nextRegion++,
                           .owner = &classDecl,
                           .ownerId = info.id,
                           .sourceUnit = unit});
  ++lexicalFrameVersion;
  rebuildLexicalEnvironment();
  return true;
}

auto SemanticVisitor::Impl::exitLexicalFrame() -> void {
  const RequestSessionState::LexicalFrame frame = lexicalFrames.back();
  if (frame.owner != nullptr) {
    endClassBody(classInfo(frame.ownerId));
  }
  lexicalFrames.pop_back();
  ++lexicalFrameVersion;
  lexicalLoanFlow = loanFlow;
  rebuildLexicalEnvironment();
}

auto SemanticVisitor::Impl::processLexicalLeaf(const StmtPtr &statement,
                                               bool measure) -> void {
  const std::size_t version = lexicalFrameVersion;
  const std::size_t depth = lexicalFrames.size();
  const StmtList *list = lexicalFrames.back().statements;
  const std::size_t index = lexicalFrames.back().index;
  const auto start = measure ? std::chrono::steady_clock::now()
                             : std::chrono::steady_clock::time_point{};
  measureTopLevelStatements = false;
  heldDiagnostics.emplace_back();
  analyze(statement);
  {
    std::vector<Diagnostic> held = std::move(heldDiagnostics.back());
    heldDiagnostics.pop_back();
    diagnostics.insert(diagnostics.end(), std::make_move_iterator(held.begin()),
                       std::make_move_iterator(held.end()));
  }
  measureTopLevelStatements = measure;
  if (measure && workMetrics != nullptr) {
    const std::size_t elapsed = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    const SourceUnitId unit = statementSourceUnit(*statement);
    if (sourceGraph != nullptr && sourceGraph->isCompilerTrusted(unit)) {
      workMetrics->semanticTrustedBodyMicroseconds += elapsed;
    } else {
      workMetrics->semanticApplicationBodyMicroseconds += elapsed;
    }
  }
  const bool advanced = lexicalFrameVersion != version ||
                        lexicalFrames.size() != depth ||
                        lexicalFrames.back().statements != list ||
                        lexicalFrames.back().index != index;
  if (advanced) {
    // A demand advanced the walk while this leaf ran. The walk's own state
    // (frames, declaration-level loan flow, reachability) is already current;
    // only the environment this leaf ran in must be rebuilt from it.
    rebuildLexicalEnvironment();
    return;
  }
  lexicalLoanFlow = loanFlow;
  lexicalFrames.back().reachable =
      summarizeFlow(statement.get()).canFallThrough;
}

auto SemanticVisitor::Impl::advanceLexicalPass(const FunctionDecl *until)
    -> void {
  while (!lexicalFrames.empty()) {
    if (until != nullptr && visitedFunctionDeclarations.contains(until)) {
      return;
    }
    RequestSessionState::LexicalFrame &frame = lexicalFrames.back();
    if (frame.index >= frame.statements->size()) {
      exitLexicalFrame();
      continue;
    }
    const StmtPtr &statement = (*frame.statements)[frame.index++];
    if (!statement) {
      continue;
    }
    loanFlow.currentRegion = frame.region;
    loanFlow.currentOrder = frame.index;
    loanFlow.currentStatement = statement.get();
    if (!frame.reachable && !frame.reportedUnreachable) {
      frame.reportedUnreachable = true;
      if (!instanceAnalysisActive && statement->extent()) {
        Diagnostic diagnostic = makeDiagnostic(
            DiagnosticId::UnreachableStatement, *statement->extent());
        diagnostic.tags.push_back(DiagnosticTag::Unnecessary);
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
    const SourceUnitId statementUnit = statementSourceUnit(*statement);
    const SourceUnitId unit =
        statementUnit != 0 ? statementUnit : frame.sourceUnit;
    if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      enterLexicalNamespace(*space, unit);
      continue;
    }
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target)) {
        lexicalFrames.push_back({.statements = branch,
                                 .region = loanFlow.nextRegion++,
                                 .sourceUnit = unit});
        ++lexicalFrameVersion;
      }
      continue;
    }
    if (const auto *classDecl = stmtAs<ClassDecl>(statement.get())) {
      (void)enterLexicalClass(*classDecl, unit);
      continue;
    }
    processLexicalLeaf(statement, measureTopLevelStatements);
  }
}

// Complete deferral drains in reverse order; reachable deferral drains only
// the transitive demand set. Each
// body is analysed exactly as in place: its namespace, source unit, owning
// class scope with members, generic parameters and requirement scope are
// re-established around the shared body half.
auto SemanticVisitor::Impl::analyzeDeferredBodies() -> void {
  if (deferredBodies.empty()) {
    return;
  }
  const auto start = std::chrono::steady_clock::now();
  if (trustedBodies == TrustedBodyAnalysis::Deferred) {
    for (std::size_t index = deferredBodies.size(); index > 0; --index)
      analyzeDeferredBody(deferredBodies[index - 1]);
  } else {
    drainDemandedDeferredBodies();
  }
  if (workMetrics != nullptr) {
    workMetrics->semanticTrustedBodyMicroseconds += static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    workMetrics->semanticDeferredBodies += deferredBodies.size();
    workMetrics->semanticAnalyzedDeferredBodies += std::count_if(
        deferredBodies.begin(), deferredBodies.end(), [&](const auto &body) {
          return analyzedBodies.contains(body.declaration);
        });
  }
}

auto SemanticVisitor::Impl::analyzeDeferredBody(
    const RequestSessionState::DeferredBody body) -> void {
  if (analyzedBodies.contains(body.declaration)) {
    return; // Analysed on demand by a caller meanwhile.
  }
  const std::vector<std::string> enclosingNamespace = currentNamespace;
  const SourceUnitId enclosingUnit = currentSourceUnit;
  const std::optional<ClassId> enclosingClass = currentClass;
  enterDeclarationContext(body.namespaceScope, body.sourceUnit, body.owner);
  const FunctionInfo *functionInfo =
      semanticModel.findFunction(*body.declaration);
  beginTypeParameterScope(genericParametersFor(*body.declaration));
  const bool requirementsValid = beginRequirementScope(functionInfo);
  if (requirementsValid) {
    const std::size_t enclosingOrder = currentDeclarationOrder;
    currentDeclarationOrder = body.declarationOrder;
    analyzeFunctionBody(*body.declaration, functionInfo, body.declarationErrors,
                        body.constexprDeclarationValid);
    currentDeclarationOrder = enclosingOrder;
  }
  endRequirementScope();
  endTypeParameterScope();
  exitDeclarationContext(body.owner);
  currentNamespace = enclosingNamespace;
  currentSourceUnit = enclosingUnit;
  currentClass = enclosingClass;
}

// Conservative fixed point: selected callees, callable values and other
// consulted records can demand further bodies. Every body runs at most once
// and retains its original lexical visibility and safety checks.
auto SemanticVisitor::Impl::drainDemandedDeferredBodies() -> void {
  std::unordered_set<const FunctionDecl *> processed;
  bool progress = true;
  while (progress) {
    progress = false;
    for (std::size_t index = 0; index < deferredBodies.size(); ++index) {
      const auto body = deferredBodies[index];
      if (!demandedFunctions.contains(body.function) ||
          !processed.insert(body.declaration).second)
        continue;
      progress = true;
      analyzeDeferredBody(body);
    }
  }
}

auto SemanticVisitor::Impl::pendingDeferredBody(
    const FunctionDecl *declaration) const -> bool {
  return declaration != nullptr && deferredBodyIndex.contains(declaration) &&
         !analyzedBodies.contains(declaration);
}

auto SemanticVisitor::Impl::finalizesFunction(
    const FunctionInfo &function) const -> bool {
  if (lateFinalization != nullptr)
    return function.bodyDeclaration() != nullptr &&
           lateFinalization->functions->contains(function.bodyDeclaration());
  // A pending body has no analysed facts yet. Inferring its callable
  // contracts from the bare declaration would pre-empt the ones its body
  // establishes if a late demand analyses it; no analysed body calls it.
  return !pendingDeferredBody(function.bodyDeclaration());
}

auto SemanticVisitor::Impl::finalizesCall(const Call *call) const -> bool {
  return lateFinalization == nullptr || lateFinalization->calls->contains(call);
}

// Late demand. The request is complete: the lexical pass, the reachable
// drain and request finalization have run, and concrete discovery is reading
// the model. A late batch is equivalent to the drain having analysed the
// bodies when:
// - each body is pending (deferred, never analysed) and its facts were
//   consumed by nothing already kept: no analysed body calls it (it would
//   have been demanded), callers' concrete instances re-run after the batch
//   (withLateDemand), and finalization skipped pending bodies;
// - the batch adds no error (invalid primary analysis never reaches HIR);
// - no initializer's program-effect closure visited one of the bodies, so the
//   program initialization plan already validated is unaffected;
// - identities stay disjoint from existing instance deltas.
// Raw-pointer audits keep complete analysis. A condition that fails before
// analysing refuses (the caller records an ordinary semantic request); one
// that fails afterwards marks the request failed, and the frontend repeats
// the analysis with every late body required.
auto SemanticVisitor::Impl::analyzeLateDemand(
    std::span<const FunctionDecl *const> roots) -> bool {
  if (!lateDemandReady || lateDemandFailed || bodyPhaseActive ||
      instanceAnalysisActive || rawAuditEnabled() || hadError())
    return false;
  // A preamble body is analysed already; reaching it reports what it held
  // back. An error among that is exactly what a fresh late analysis would
  // have found, so the request fails the same way and the repeated analysis
  // reports it in the pass.
  if (adoptedPreamble != nullptr && reachPreambleBodies(roots, false)) {
    lateDemandFailed = true;
    for (const FunctionDecl *root : roots)
      if (root != nullptr && requestSession.preambleBodies.contains(root))
        lateDemandedBodies.push_back(root);
    return false;
  }
  std::vector<const FunctionDecl *> pending;
  for (const FunctionDecl *root : roots)
    if (pendingDeferredBody(root))
      pending.push_back(root);
  if (pending.empty())
    return std::all_of(
        roots.begin(), roots.end(), [&](const FunctionDecl *root) {
          return root != nullptr && analyzedBodies.contains(root);
        });
  if (instanceSymbolCeiling > semanticModel.database().symbolIdentityCeiling())
    semanticModel.reserveSymbolIdentities(instanceSymbolCeiling);
  nextLambdaId = std::max(nextLambdaId, instanceLambdaCeiling);
  const LambdaId firstLambda = nextLambdaId;

  const auto start = std::chrono::steady_clock::now();
  const std::size_t diagnosticStart = diagnostics.size();
  std::unordered_set<const Call *> calls;
  std::vector<const FunctionDecl *> analyzed;
  LateFinalizationScope scope{
      .calls = &calls,
      .forwardings = semanticModel.pendingCallableForwardings.size(),
      .cThreadCallUses = requestSession.cThreadCallUses.size(),
      .unsafeBlockCandidates = requestSession.unsafeBlockCandidates.size(),
      .mutableLocalCandidates = requestSession.mutableLocalCandidates.size(),
      .genericDefaultConstantUses = genericDefaultConstantUses.size(),
      .occurrenceCounts = semanticModel.occurrenceCounts()};
  auto *const enclosingJournal = semanticModel.setCallJournal(&calls);
  lateAnalyzedBodies = &analyzed;
  bodyPhaseActive = true;
  rebuildLexicalEnvironment();
  {
    const FunctionDemandScope demandScope(semanticModel, &demandedFunctions);
    for (const FunctionDecl *declaration : pending)
      if (const FunctionInfo *function =
              semanticModel.findFunction(*declaration))
        demandedFunctions.insert(function->id);
    drainDemandedDeferredBodies();
  }
  endScope();
  bodyPhaseActive = false;
  lateAnalyzedBodies = nullptr;
  semanticModel.setCallJournal(enclosingJournal);

  const std::unordered_set<const FunctionDecl *> functions(analyzed.begin(),
                                                           analyzed.end());
  scope.functions = &functions;
  scope.firstLambda = firstLambda;
  for (const auto &[unit, count] : semanticModel.occurrenceCounts()) {
    const auto before = scope.occurrenceCounts.find(unit);
    if (before == scope.occurrenceCounts.end() || before->second != count)
      scope.units.insert(unit);
  }
  bool equivalent = std::all_of(
      pending.begin(), pending.end(),
      [&](const FunctionDecl *root) { return functions.contains(root); });
  for (const FunctionDecl *declaration : analyzed) {
    const FunctionInfo *function = semanticModel.findFunction(*declaration);
    if (function != nullptr)
      scope.functionIds.insert(function->id);
    if (function != nullptr &&
        initializerEffectOwners.contains(
            {.kind = ProgramEffectOwnerKind::Function, .id = function->id}))
      equivalent = false;
  }
  finalizeLateDemand(scope);
  if (hasErrorDiagnostics(std::span(diagnostics).subspan(diagnosticStart)))
    equivalent = false;
  lateDemandedBodies.insert(lateDemandedBodies.end(), analyzed.begin(),
                            analyzed.end());
  if (!equivalent) {
    // Concrete discovery must not lower these bodies from this model; the
    // repeated analysis analyses them in the pass instead.
    lateDemandFailed = true;
    for (const FunctionDecl *declaration : analyzed)
      semanticModel.forgetAnalyzedFunctionBody(*declaration);
  }
  if (workMetrics != nullptr) {
    const auto elapsed = static_cast<std::size_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start)
            .count());
    workMetrics->semanticLateDemandMicroseconds += elapsed;
    workMetrics->semanticTrustedBodyMicroseconds += elapsed;
    workMetrics->semanticLateDemandBodies += analyzed.size();
    workMetrics->semanticAnalyzedDeferredBodies += std::count_if(
        analyzed.begin(), analyzed.end(), [&](const FunctionDecl *body) {
          return deferredBodyIndex.contains(body);
        });
  }
  return equivalent;
}

// Request finalization restricted to one late batch: the declaration-fact
// passes and diagnostics cover only the bodies the batch analysed and the
// calls, forwardings and candidates it recorded; idempotent whole-model
// passes simply run again. Pass order is the order of check().
auto SemanticVisitor::Impl::finalizeLateDemand(
    const LateFinalizationScope &scope) -> void {
  lateFinalization = &scope;
  recordGenericDefaultConstantUses();
  validateDefaultArgumentCycles();
  discoverOwnedCallableContracts();
  semanticModel.finalizeCallableForwardings();
  discoverOwnedCallableValueParameters();
  validateOnceCallableForwardings();
  validateOwnedCallableArguments();
  validateProvisionalCallableArguments();
  validateConfinedCallableContracts();
  validateConfinedCallableUses();
  semanticModel.finalizeCallableArguments(*scope.calls, scope.units);
  finalizeCThreadRequirements();
  finalizeDeferredProgramConstantSubstitutions(
      semanticModel.programInitializationPlan());
  semanticModel.finalizeOccurrences(scope.units);
  finalizeAdvisoryDiagnostics();
  catalog.assertUnchangedSinceSeal();
  lateFinalization = nullptr;
}

auto SemanticVisitor::Impl::visitIfStmt(const IfStmt &stmt) -> void {
  if (!stmt.isConstexpr()) {
    semanticModel.recordFullExpression(stmt, stmt.condition());
  }
  if (stmt.isConstexpr()) {
    const bool enclosingCondition = analyzingDependentConstantCondition;
    const bool enclosingDependency = constantConditionGenericDependency;
    analyzingDependentConstantCondition = true;
    constantConditionGenericDependency = false;
    const SemanticType conditionType =
        analyzeExpectedCallableResult(stmt.condition(), SemanticType::Bool);
    const bool dependent = constantConditionGenericDependency;
    analyzingDependentConstantCondition = enclosingCondition;
    constantConditionGenericDependency = enclosingDependency;
    requireBool(stmt.condition(), conditionType,
                expressionToken(stmt.condition()),
                "If constexpr condition must be bool.");
    if (conditionType != SemanticType::Bool) {
      return;
    }

    if (dependent && !instanceAnalysisActive) {
      semanticModel.recordConstexprBranch(stmt, std::nullopt);
      loanFlow.deferredConstexprSelection = true;
      requestSession.deferredFlowOwners.insert(currentProgramEffectOwner);
      return;
    }

    ConstexprInterpreter context = makeConstexprInterpreter();
    ConstexprEvaluation evaluated =
        evaluateConstexprExpression(stmt.condition(), context);
    const bool *selected =
        evaluated ? std::get_if<bool>(&*evaluated.value) : nullptr;
    if (selected == nullptr) {
      if (evaluated) {
        evaluated.failure = ConstantEvaluationFailure::InvalidOperands;
        evaluated.value.reset();
      }
      reportConstexprConditionFailure(stmt, evaluated);
      return;
    }

    semanticModel.recordConstexprBranch(stmt, *selected);
    analyzeConditionalArm(*selected ? stmt.thenBranch() : stmt.elseBranch());
    return;
  }

  const std::size_t conditionalIndex = loanFlow.conditionals.size();
  if (!loanFlow.activeConditionalArms.empty()) {
    const ActiveLoanFlowConditionalArm parent =
        loanFlow.activeConditionalArms.back();
    if (parent.conditional < loanFlow.conditionals.size()) {
      LoanFlowConditional &parentConditional =
          loanFlow.conditionals[parent.conditional];
      LoanFlowConditionalArm &parentArm = parent.thenBranch
                                              ? parentConditional.thenArm
                                              : parentConditional.elseArm;
      parentArm.nestedConditionals.push_back(conditionalIndex);
    }
  }
  loanFlow.conditionals.push_back(
      {.statement = &stmt,
       .useBegin = loanFlow.uses.size(),
       .conflictBegin = loanFlow.conflicts.size(),
       .parentPath = loanFlow.activeConditionalArms,
       .parentSwitchPath = loanFlow.activeSwitchArms});
  loanFlow.conditionalBoundaries.push_back({.region = loanFlow.currentRegion,
                                            .order = loanFlow.currentOrder,
                                            .statement = &stmt});
  const SemanticType conditionType =
      analyzeExpectedCallableResult(stmt.condition(), SemanticType::Bool);
  requireBool(stmt.condition(), conditionType,
              expressionToken(stmt.condition()), "If condition must be bool.");
  const ScopeStack beforeBranches = scopes;
  const std::vector<ValueControlFlowContext> beforeValueControlFlow =
      valueControlFlow;

  loanFlow.conditionals[conditionalIndex].thenArm.useBegin =
      loanFlow.uses.size();
  loanFlow.conditionals[conditionalIndex].thenArm.conflictBegin =
      loanFlow.conflicts.size();
  loanFlow.activeConditionalArms.push_back(
      {.conditional = conditionalIndex, .thenBranch = true});
  analyzeConditionalArm(stmt.thenBranch());
  loanFlow.activeConditionalArms.pop_back();
  loanFlow.conditionals[conditionalIndex].thenArm.useEnd = loanFlow.uses.size();
  loanFlow.conditionals[conditionalIndex].thenArm.conflictEnd =
      loanFlow.conflicts.size();
  const ScopeStack thenScopes = scopes;
  const FlowSummary thenFlow = summarizeFlow(stmt.thenBranch().get());
  const std::vector<ValueControlFlowContext> thenValueControlFlow =
      valueControlFlow;

  scopes = beforeBranches;
  valueControlFlow = beforeValueControlFlow;
  loanFlow.conditionals[conditionalIndex].elseArm.useBegin =
      loanFlow.uses.size();
  loanFlow.conditionals[conditionalIndex].elseArm.conflictBegin =
      loanFlow.conflicts.size();
  if (stmt.elseBranch()) {
    loanFlow.activeConditionalArms.push_back(
        {.conditional = conditionalIndex, .thenBranch = false});
    analyzeConditionalArm(stmt.elseBranch());
    loanFlow.activeConditionalArms.pop_back();
  }
  loanFlow.conditionals[conditionalIndex].elseArm.useEnd = loanFlow.uses.size();
  loanFlow.conditionals[conditionalIndex].elseArm.conflictEnd =
      loanFlow.conflicts.size();
  loanFlow.conditionals[conditionalIndex].useEnd = loanFlow.uses.size();
  loanFlow.conditionals[conditionalIndex].conflictEnd =
      loanFlow.conflicts.size();
  loanFlow.conditionals[conditionalIndex].endSequence = loanFlow.nextSequence++;
  const ScopeStack elseScopes = scopes;
  const FlowSummary elseFlow = summarizeFlow(stmt.elseBranch().get());
  const std::vector<ValueControlFlowContext> elseValueControlFlow =
      valueControlFlow;

  scopes = beforeBranches;
  mergeValueStates(beforeBranches, thenScopes, elseScopes);
  const ScopeStack loanScopes = scopes;
  ScopeStack moveScopes = beforeBranches;
  if (const std::optional<bool> condition = constantBoolean(stmt.condition())) {
    moveScopes = *condition ? thenScopes : elseScopes;
    valueControlFlow = *condition ? thenValueControlFlow : elseValueControlFlow;
  } else if (thenFlow.canFallThrough && elseFlow.canFallThrough) {
    moveScopes = mergedValueStates(beforeBranches, {thenScopes, elseScopes});
    valueControlFlow = mergedValueControlFlow(
        beforeValueControlFlow, thenValueControlFlow, elseValueControlFlow);
  } else if (thenFlow.canFallThrough) {
    moveScopes = thenScopes;
    valueControlFlow = mergedValueControlFlow(
        beforeValueControlFlow, thenValueControlFlow, elseValueControlFlow);
  } else if (elseFlow.canFallThrough) {
    moveScopes = elseScopes;
    valueControlFlow = mergedValueControlFlow(
        beforeValueControlFlow, thenValueControlFlow, elseValueControlFlow);
  } else {
    valueControlFlow = mergedValueControlFlow(
        beforeValueControlFlow, thenValueControlFlow, elseValueControlFlow);
  }
  scopes = loanScopes;
  applyMoveStates(moveScopes);
  loanFlow.conditionalBoundaries.pop_back();
}

auto SemanticVisitor::Impl::visitLoopControlStmt(const LoopControlStmt &stmt)
    -> void {
  const bool isBreak = stmt.keyword().kind == TokenKind::BREAK;
  const bool valid = (isBreak && (loopDepth != 0 || switchDepth != 0)) ||
                     (!isBreak && loopDepth != 0);
  if (!valid) {
    report(stmt.keyword(),
           isBreak ? "'break' can only be used inside a loop or switch."
                   : "'continue' can only be used inside a loop.",
           "GTI-S2010");
    return;
  }
  if (isBreak) {
    recordLoanFlowBreak(stmt);
  }
  recordValueControlFlowExit(isBreak);
}

auto SemanticVisitor::Impl::visitMutableFieldGroupDecl(
    const MutableFieldGroupDecl &stmt) -> void {
  analyze(stmt.members());
}

auto SemanticVisitor::Impl::visitNamespaceAliasDecl(
    const NamespaceAliasDecl &stmt) -> void {
  if (compilerPrivatePathDenied(stmt.target())) {
    return;
  }
  recordQualifiedPathUses(stmt.target(), true);
  const SymbolId symbol =
      recordToolingSymbol(stmt.name(), SymbolKind::NamespaceAlias,
                          qualifiedName(currentNamespace, stmt.name().lexeme),
                          SemanticType::Unknown);
  attachDeclarationDocumentation(symbol, stmt);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.name()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = stmt.name().lexeme});
}

auto SemanticVisitor::Impl::visitNamespaceDecl(const NamespaceDecl &stmt)
    -> void {
  const SymbolId symbol =
      recordToolingSymbol(stmt.name(), SymbolKind::Namespace,
                          qualifiedName(currentNamespace, stmt.name().lexeme),
                          SemanticType::Unknown);
  attachDeclarationDocumentation(symbol, stmt);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.name()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = stmt.name().lexeme});
  currentNamespace.emplace_back(stmt.name().lexeme);
  analyze(stmt.declarations());
  currentNamespace.pop_back();
}

auto SemanticVisitor::Impl::visitReturnStmt(const ReturnStmt &stmt) -> void {
  semanticModel.recordFullExpression(stmt, stmt.value());
  if (functionDepth == 0) {
    report(stmt.keyword(), "Cannot return from outside a function.");
    return;
  }
  if (constructorDepth > 0) {
    if (stmt.value()) {
      analyze(stmt.value());
    }
    report(stmt.keyword(), "Constructors cannot contain return statements.");
    return;
  }
  if (destructorDepth > 0) {
    if (stmt.value()) {
      analyze(stmt.value());
    }
    report(stmt.keyword(), "Destructors cannot contain return statements.",
           "GTI-S2021");
    return;
  }
  if (currentReturnType == SemanticType::Void) {
    if (stmt.value()) {
      const SemanticType valueType = analyze(stmt.value());
      if (valueType != SemanticType::Void) {
        report(stmt.keyword(), "Void function cannot return a value.");
      }
    }
    requireInitializedReceiver(stmt.keyword());
    return;
  }
  if (!stmt.value()) {
    if (isExpectedVoid(currentReturnType)) {
      requireInitializedReceiver(stmt.keyword());
      return;
    }
    report(stmt.keyword(), "A value is required for this return type.");
    requireInitializedReceiver(stmt.keyword());
    return;
  }

  if (currentReturnType.kind == SemanticType::Reference) {
    if (currentReturnType.arguments.size() != 1) {
      analyze(stmt.value());
      requireInitializedReceiver(stmt.keyword());
      return;
    }
    const SemanticType valueType =
        analyzeInitializer(stmt.value(), currentReturnType);
    if (currentReturnType.referenceAccess == AccessMode::Mutable) {
      recordMutableBindingUse(stmt.value());
    }
    forceProgramStorageBoundaryAccess(stmt.value());
    validateReferenceReturn(currentReturnType, valueType, stmt.value());
    recordRawResult(stmt.value());
    requireInitializedReceiver(stmt.keyword());
    return;
  }

  const SemanticType valueType =
      analyzeInitializer(stmt.value(), currentReturnType);
  recordRawResult(stmt.value());
  if (typeTraits(currentReturnType).containsBorrowedState) {
    validateStoredBorrowReturn(currentReturnType, valueType, stmt.value());
  }
  if (currentReturnType.kind == SemanticType::StringView ||
      currentReturnType.kind == SemanticType::CString) {
    validateOwnerDependentTextReturn(valueType, stmt.value());
  }
  if (!isOwnershipAssignable(currentReturnType, valueType, stmt.value())) {
    const ImplicitReturnMoveStatus implicitMove =
        analyzeImplicitReturnMove(stmt.value(), currentReturnType, valueType);
    if (implicitMove == ImplicitReturnMoveStatus::NotEligible) {
      if (isMoveOnlyOwnerType(valueType) &&
          isAssignable(currentReturnType, valueType, stmt.value().get())) {
        const Variable *returned = movedVariable(stmt.value());
        const Symbol *returnedSymbol =
            returned == nullptr ? nullptr : resolve(returned->name());
        const bool namedLocalOwner =
            returnedSymbol != nullptr &&
            (returnedSymbol->bindingKind ==
                 SemanticBindingKind::LocalVariable ||
             returnedSymbol->bindingKind == SemanticBindingKind::Parameter);
        if (namedLocalOwner) {
          Diagnostic diagnostic =
              makeDiagnostic("GTI-S2003", DiagnosticPhase::Semantics,
                             expressionToken(stmt.value()),
                             "Cannot return '" + returned->name().lexeme +
                                 "' by copy because type '" +
                                 typeSpelling(valueType) + "' is move-only.");
          diagnostic.hints.emplace_back("Use 'return std::move(" +
                                        returned->name().lexeme +
                                        ");' to transfer ownership.");
          diagnostics.emplace_back(std::move(diagnostic));
        } else {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2003", DiagnosticPhase::Semantics,
              expressionToken(stmt.value()),
              "Returning this value would copy move-only type '" +
                  typeSpelling(valueType) + "'.");
          diagnostic.hints.emplace_back(
              "Return a fresh value or transfer an eligible owner explicitly "
              "with std::move(...).");
          diagnostics.emplace_back(std::move(diagnostic));
        }
      } else {
        report(expressionToken(stmt.value()),
               "Cannot return a value of type '" + typeSpelling(valueType) +
                   "' from a function returning '" +
                   typeSpelling(currentReturnType) + "'.",
               "GTI-S2003");
      }
    }
  }
  requireInitializedReceiver(stmt.keyword());
}

auto SemanticVisitor::Impl::visitSwitchStmt(const SwitchStmt &stmt) -> void {
  semanticModel.recordFullExpression(stmt, stmt.expression());
  noteUnsupportedNestedLoanControlFlow();
  const std::size_t loanSwitchIndex = loanFlow.switches.size();
  loanFlow.switches.push_back(
      {.statement = &stmt,
       .parentPath = loanFlow.activeConditionalArms,
       .parentSwitchPath = loanFlow.activeSwitchArms,
       .arms = std::vector<LoanFlowSwitchArm>(stmt.arms().size())});
  loanFlow.switchBoundaries.push_back({.region = loanFlow.currentRegion,
                                       .order = loanFlow.currentOrder,
                                       .statement = &stmt});
  const SemanticType subjectType = analyze(stmt.expression());
  const EnumTypeInfo *payloadEnum =
      subjectType.kind == SemanticType::Enum
          ? semanticModel.findEnumType(subjectType.enumId)
          : nullptr;
  if (payloadEnum != nullptr && !payloadEnum->payload) {
    payloadEnum = nullptr;
  }
  const bool validSubject = isInteger(subjectType) ||
                            subjectType == SemanticType::Char ||
                            subjectType.kind == SemanticType::Enum;
  if (subjectType != SemanticType::Unknown && !validSubject) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2037", DiagnosticPhase::Semantics, stmt.keyword(),
        "Switch expression must have an integer, char, or scoped enum "
        "type; found '" +
            typeSpelling(subjectType) + "'.");
    if (subjectType == SemanticType::Bool) {
      diagnostic.hints.emplace_back(
          "Use 'if' for boolean branching; switch is reserved for "
          "multi-value selection.");
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }

  const ScopeStack beforeSwitch = scopes;
  std::vector<std::pair<SwitchCaseValue, const SwitchLabel *>> seenCases;
  const SwitchLabel *firstDefault = nullptr;
  std::vector<std::pair<ScopeStack, FlowSummary>> armResults;
  std::unordered_set<std::size_t> coveredPayloadVariants;
  std::unordered_set<std::size_t> flowCoveredPayloadVariants;
  bool invalidPayloadPattern = false;
  bool hasDefault = false;

  ++switchDepth;
  beginValueControlFlow(ValueControlFlowKind::Switch, stmt);
  for (std::size_t armIndex = 0; armIndex < stmt.arms().size(); ++armIndex) {
    const SwitchArm &arm = stmt.arms()[armIndex];
    std::vector<std::pair<const Expr *, ResolvedPayloadPatternInfo>>
        payloadPatterns;
    for (const SwitchLabel &label : arm.labels) {
      if (label.isDefault()) {
        hasDefault = true;
        if (firstDefault == nullptr) {
          firstDefault = &label;
        } else {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2037", DiagnosticPhase::Semantics, label.keyword,
              "Switch may contain only one 'default' label.");
          diagnostic.related.push_back(
              {tokenSpan(firstDefault->keyword),
               "First 'default' label declared here."});
          diagnostics.emplace_back(std::move(diagnostic));
        }
        continue;
      }

      if (payloadEnum != nullptr) {
        const auto *patternCall = exprAs<Call>(label.value.get());
        const auto *variantName = exprAs<QualifiedName>(
            patternCall ? patternCall->callee().get() : label.value.get());
        if (variantName != nullptr) {
          if (const auto *variant = resolveEnumerator(variantName->name());
              variant != nullptr && variant->symbol.type == subjectType) {
            flowCoveredPayloadVariants.insert(variant->variantIndex);
          }
        }
        scopes = beforeSwitch;
        std::optional<ResolvedPayloadPatternInfo> pattern =
            analyzePayloadSwitchPattern(label.value, subjectType);
        scopes = beforeSwitch;
        if (!pattern) {
          invalidPayloadPattern = true;
          continue;
        }
        const SwitchCaseValue value{
            .kind = SwitchCaseKind::Enumerator,
            .type = subjectType,
            .value = {.magnitude = pattern->variantIndex},
            .enumOwner = subjectType.enumId};
        const auto duplicate = std::find_if(
            seenCases.begin(), seenCases.end(),
            [&](const auto &candidate) { return candidate.first == value; });
        if (duplicate != seenCases.end()) {
          Diagnostic diagnostic = makeDiagnostic(
              "GTI-S2067", DiagnosticPhase::Semantics, label.keyword,
              "Duplicate payload-enum switch variant.");
          diagnostic.related.push_back(
              {tokenSpan(duplicate->second->keyword),
               "First matching variant is handled here."});
          diagnostics.emplace_back(std::move(diagnostic));
          invalidPayloadPattern = true;
          continue;
        }
        seenCases.emplace_back(value, &label);
        coveredPayloadVariants.insert(pattern->variantIndex);
        semanticModel.recordSwitchCase(*label.value, value);
        payloadPatterns.emplace_back(label.value.get(), std::move(*pattern));
        continue;
      }

      scopes = beforeSwitch;
      const SemanticType caseType = analyze(label.value);
      scopes = beforeSwitch;
      if (subjectType == SemanticType::Unknown ||
          caseType == SemanticType::Unknown || !validSubject) {
        continue;
      }
      if (caseType != subjectType) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2037", DiagnosticPhase::Semantics,
                           expressionToken(label.value),
                           "Switch case type '" + typeSpelling(caseType) +
                               "' does not exactly match subject type '" +
                               typeSpelling(subjectType) + "'.");
        diagnostic.hints.emplace_back(
            "GTI switch labels do not perform implicit conversions; use "
            "an explicit conversion when appropriate.");
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      const std::optional<SwitchCaseValue> value =
          switchCaseConstant(label.value.get(), subjectType);
      if (!value) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2037", DiagnosticPhase::Semantics,
            expressionToken(label.value),
            "Switch case must be a compile-time integer or character "
            "constant, or a scoped enumerator.");
        diagnostic.hints.emplace_back(
            "Use a literal, an explicitly converted integer literal, or "
            "an enumerator from the switch enum.");
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }

      const auto duplicate = std::find_if(
          seenCases.begin(), seenCases.end(),
          [&](const auto &candidate) { return candidate.first == *value; });
      if (duplicate != seenCases.end()) {
        Diagnostic diagnostic =
            makeDiagnostic("GTI-S2037", DiagnosticPhase::Semantics,
                           label.keyword, "Duplicate switch case value.");
        diagnostic.related.push_back(
            {tokenSpan(duplicate->second->keyword),
             "First matching case label declared here."});
        diagnostics.emplace_back(std::move(diagnostic));
        continue;
      }
      seenCases.emplace_back(*value, &label);
      semanticModel.recordSwitchCase(*label.value, *value);
    }

    scopes = beforeSwitch;
    beginScope();
    loanFlow.activeSwitchArms.push_back(
        {.switchIndex = loanSwitchIndex, .armIndex = armIndex});
    for (auto &[source, pattern] : payloadPatterns) {
      if (!pattern.bindings.empty() && arm.labels.size() != 1) {
        report(expressionToken(*source),
               "A payload-binding switch arm must have exactly one case "
               "label.",
               "GTI-S2067");
        invalidPayloadPattern = true;
        pattern.bindings.clear();
      }
      for (PayloadBindingInfo &payload : pattern.bindings) {
        if (payload.name == nullptr) {
          continue;
        }
        const bool mutableReferent =
            payload.binding.type.kind == SemanticType::Reference &&
            payload.binding.type.referenceAccess == AccessMode::Mutable;
        const SymbolId symbol = recordBindingOccurrence(
            *payload.name, payload.binding.type, mutableReferent,
            SemanticBindingKind::LocalVariable);
        payload.binding.symbol = symbol;
        declare(*payload.name, payload.binding.type, mutableReferent,
                SemanticBindingKind::LocalVariable);
        if (!rawPointerFieldPaths(payload.binding.type).empty())
          if (auto *carrier = resolveMutable(*payload.name)) {
            const auto fields = rawPointerFields(stmt.expression().get());
            if (payload.binding.type.kind == SemanticType::RawPointer) {
              for (const auto &field : fields)
                mergeRawPointerOrigins(carrier->rawPointerOrigins,
                                       field.origins);
            } else {
              carrier->rawPointerFields = fields;
            }
          }
        if (payload.binding.type.kind == SemanticType::Reference) {
          const AccessMode access = payload.binding.type.referenceAccess;
          const ExpressionInfo *subject =
              semanticModel.findExpression(*stmt.expression());
          SemanticLoanPlace place = retainedLoanPlace(stmt.expression());
          if (subject == nullptr || subject->category != ValueCategory::Place ||
              !place.valid()) {
            report(*payload.name,
                   "A borrowed payload pattern requires a stable enum place; "
                   "bind the enum to a local variable before matching it.",
                   "GTI-S2067");
          } else if (access == AccessMode::Mutable &&
                     subject->access != AccessMode::Mutable) {
            report(
                *payload.name,
                "A mutable payload pattern requires a mutable enum receiver.",
                "GTI-S2067");
          } else if (Symbol *carrier = resolveMutable(*payload.name)) {
            if (access == AccessMode::Mutable) {
              recordMutableBindingUse(stmt.expression());
            }
            SemanticLoanId parent = 0;
            if (const auto source = retainedLoanSource(stmt.expression()))
              parent = source->loan;
            reportBorrowConflict(
                *payload.name, conflictingLoans(place, access, parent),
                "Cannot borrow this payload while an overlapping enum borrow "
                "may still be live.");
            // Variant replacement invalidates every field. The original enum
            // is the conservative alias root even when the runtime referent
            // is one exact active payload field.
            payload.binding.retainedLoan = createRetainedLoan(
                *carrier, symbol, SemanticLoanKind::Reference, payload.source,
                place, place.root, place.receiver, access, true, parent);
            // The binding is born at this arm's entry, not at the switch
            // subject's full expression. Keep its endpoint lexical until
            // arm-local NLL creation/endpoint plans are represented.
            loanFlow.records.at(payload.binding.retainedLoan).confinedToRegion =
                false;
          }
        }
        semanticModel.recordPayloadBinding(*payload.name, payload.binding);
      }
      semanticModel.recordPayloadPattern(*source, std::move(pattern));
    }
    analyze(arm.statements);
    loanFlow.activeSwitchArms.pop_back();
    endScope();
    const FlowSummary flow = summarizeFlow(arm.statements);
    armResults.emplace_back(scopes, flow);
    if (flow.canFallThrough) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2037", DiagnosticPhase::Semantics,
          arm.labels.empty() ? stmt.keyword() : arm.labels.back().keyword,
          "Switch arm can reach its boundary without an explicit "
          "terminator.");
      diagnostic.hints.emplace_back("End every switch arm with a terminal "
                                    "call, 'break', 'return', or 'continue' "
                                    "when the switch is inside a loop.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  const bool exhaustivePayload =
      payloadEnum != nullptr && !invalidPayloadPattern &&
      coveredPayloadVariants.size() == payloadEnum->enumerators.size();
  if (payloadEnum != nullptr && invalidPayloadPattern &&
      flowCoveredPayloadVariants.size() == payloadEnum->enumerators.size()) {
    requestSession.recoveredExhaustiveSwitches.insert(&stmt);
  }
  if (hasDefault || exhaustivePayload) {
    semanticModel.recordExhaustiveSwitch(stmt);
  }
  if (payloadEnum != nullptr && !hasDefault && !invalidPayloadPattern &&
      !exhaustivePayload) {
    std::string missing;
    for (const EnumeratorInfo &enumerator : payloadEnum->enumerators) {
      if (coveredPayloadVariants.contains(enumerator.variantIndex) ||
          enumerator.declaration == nullptr) {
        continue;
      }
      if (!missing.empty()) {
        missing += ", ";
      }
      missing += payloadEnum->qualifiedName +
                 "::" + enumerator.declaration->name.lexeme;
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2067", DiagnosticPhase::Semantics, stmt.keyword(),
        "Payload-enum switch is not exhaustive; missing " + missing + ".");
    diagnostic.hints.emplace_back(
        "Handle every variant explicitly or add a 'default' arm.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  ValueControlFlowContext valueFlow = endValueControlFlow();
  --switchDepth;

  std::vector<ScopeStack> loanExitStates;
  if (!hasDefault && !exhaustivePayload) {
    loanExitStates.push_back(beforeSwitch);
  }
  for (const auto &[armScopes, flow] : armResults) {
    if (flow.canFallThrough || flow.breaksEnclosingControl) {
      loanExitStates.push_back(armScopes);
    }
  }
  const ScopeStack loanScopes =
      loanExitStates.empty() ? beforeSwitch
                             : mergedValueStates(beforeSwitch, loanExitStates);

  std::vector<ScopeStack> exitStates;
  if (!hasDefault && !exhaustivePayload) {
    exitStates.push_back(beforeSwitch);
  }
  for (auto &[armScopes, flow] : armResults) {
    if (flow.canFallThrough) {
      exitStates.push_back(std::move(armScopes));
    }
  }
  for (ScopeStack &breakState : valueFlow.breakStates) {
    exitStates.push_back(std::move(breakState));
  }
  const ScopeStack moveScopes =
      exitStates.empty() ? beforeSwitch
                         : mergedValueStates(beforeSwitch, exitStates);
  scopes = loanScopes;
  applyMoveStates(moveScopes);
  loanFlow.switches[loanSwitchIndex].hasDefault =
      hasDefault || exhaustivePayload;
  loanFlow.switches[loanSwitchIndex].endSequence = loanFlow.nextSequence++;
  loanFlow.switchBoundaries.pop_back();
}

auto SemanticVisitor::Impl::visitStructuredBindingDecl(
    const StructuredBindingDecl &stmt) -> void {
  semanticModel.recordFullExpression(stmt, stmt.initializer());
  if (functionDepth == 0) {
    report(stmt.autoKeyword(),
           "Structured bindings are limited to local declarations.",
           "GTI-S2048");
  }

  const bool untypedInitializer =
      exprAs<ArrayInitializer>(stmt.initializer().get()) != nullptr;
  const SemanticType initializerType =
      analyzeExpectedCallableResult(stmt.initializer(), SemanticType::Unknown);
  SemanticType sourceType = initializerType;
  if (initializerType == SemanticType::Void ||
      initializerType == SemanticType::Function ||
      initializerType.kind == SemanticType::TypeName ||
      initializerType.kind == SemanticType::Unexpected) {
    report(stmt.autoKeyword(),
           "Structured binding source must have a complete value type.",
           "GTI-S2048");
    sourceType = SemanticType::Unknown;
  } else if (initializerType.kind == SemanticType::Reference) {
    report(stmt.autoKeyword(),
           "Structured bindings own their source value and do not infer "
           "references; copy or move the referred value explicitly.",
           "GTI-S2048");
    sourceType = SemanticType::Unknown;
  } else if (initializerType == SemanticType::Unknown && !untypedInitializer) {
    sourceType = SemanticType::Unknown;
  }

  if (sourceType != SemanticType::Unknown && stmt.initializer() &&
      !isOwnershipAssignable(sourceType, initializerType, stmt.initializer())) {
    report(expressionToken(stmt.initializer()),
           "Cannot initialize structured binding by copying move-only "
           "type '" +
               typeSpelling(sourceType) + "'.",
           "GTI-S2003");
    diagnostics.back().hints.emplace_back(
        "Transfer ownership explicitly with std::move(owner), or bind a "
        "fresh value.");
  }
  if (sourceType != SemanticType::Unknown &&
      typeTraits(sourceType).containsBorrowedState) {
    report(stmt.autoKeyword(),
           "Structured bindings cannot decompose values carrying stored "
           "references in the current lifetime model.",
           "GTI-S2048");
    diagnostics.back().hints.emplace_back(
        "Keep the value named and access its fields directly.");
  }

  StructuredBindingInfo result{
      .source = bindingInfo(sourceType, AccessMode::ReadOnly)};
  result.elements.reserve(stmt.bindings().size());

  std::vector<StructuredBindingElementInfo> components;
  bool decompositionKnown = false;
  if (sourceType.kind == SemanticType::Array &&
      sourceType.arguments.size() == 1 &&
      sourceType.arrayLengthParameterId == 0) {
    decompositionKnown = true;
    components.reserve(static_cast<std::size_t>(sourceType.arrayLength));
    for (std::uint64_t index = 0; index < sourceType.arrayLength; ++index) {
      components.push_back(
          {.binding =
               bindingInfo(sourceType.arguments.front(), AccessMode::ReadOnly),
           .projection = StructuredBindingProjectionKind::ArrayElement,
           .index = index});
    }
  } else if (sourceType.kind == SemanticType::Class) {
    const ClassInfo *owner = classInfo(sourceType);
    if (owner == nullptr) {
      report(stmt.autoKeyword(),
             "Structured binding source has an unresolved class type.",
             "GTI-S2048");
    } else if (!owner->bases.empty()) {
      report(stmt.autoKeyword(),
             "Structured bindings do not decompose inherited storage; "
             "only direct fields are supported.",
             "GTI-S2048");
    } else {
      decompositionKnown = true;
      components.reserve(owner->fields.size());
      for (const FieldInfo &field : owner->fields) {
        if (field.declaration == nullptr) {
          continue;
        }
        const auto member =
            owner->members.find(field.declaration->name().lexeme);
        if (member == owner->members.end()) {
          report(stmt.autoKeyword(),
                 "Structured binding source has unresolved field "
                 "metadata.",
                 "GTI-S2048");
          components.clear();
          decompositionKnown = false;
          break;
        }
        if (member->second.access != AccessModifier::Public) {
          report(field.declaration->name(),
                 "Structured bindings require every direct instance field "
                 "to be public; field '" +
                     field.declaration->name().lexeme + "' is private.",
                 "GTI-S2048");
          components.clear();
          decompositionKnown = false;
          break;
        }
        Symbol resolved = substituteSymbol(member->second.symbol, sourceType);
        if (!isCompilerTrustedRequester(stmt.autoKeyword()) &&
            (resolved.compilerPrivate ||
             semanticModel.isCompilerPrivateType(resolved.type))) {
          reportCompilerPrivateAccess(stmt.autoKeyword(),
                                      owner->name.lexeme + "." +
                                          field.declaration->name().lexeme);
          components.clear();
          decompositionKnown = false;
          break;
        }
        if (resolved.type.kind == SemanticType::Reference ||
            typeTraits(resolved.type).containsBorrowedState) {
          report(field.declaration->name(),
                 "Structured bindings cannot project a field carrying a "
                 "stored reference in the current lifetime model.",
                 "GTI-S2048");
          components.clear();
          decompositionKnown = false;
          break;
        }
        components.push_back(
            {.binding = bindingInfo(resolved.type, AccessMode::ReadOnly),
             .projection = StructuredBindingProjectionKind::Field,
             .field = toolingSymbolFor(resolved)});
      }
    }
  } else if (sourceType != SemanticType::Unknown) {
    report(stmt.autoKeyword(),
           "Structured bindings require a fixed array or a class/struct "
           "value with public direct instance fields; found '" +
               typeSpelling(sourceType) + "'.",
           "GTI-S2048");
  }

  if (decompositionKnown && components.size() != stmt.bindings().size()) {
    report(stmt.leftBracket(),
           "Structured binding declares " +
               std::to_string(stmt.bindings().size()) +
               " names but the source provides " +
               std::to_string(components.size()) + " components.",
           "GTI-S2048");
  }

  for (std::size_t index = 0; index < stmt.bindings().size(); ++index) {
    const VariableDecl &binding = stmt.bindings()[index];
    StructuredBindingElementInfo element =
        index < components.size()
            ? components[index]
            : StructuredBindingElementInfo{
                  .binding =
                      bindingInfo(SemanticType::Unknown, AccessMode::ReadOnly)};
    const SymbolId symbol =
        recordBindingOccurrence(binding.name(), element.binding.type, false,
                                SemanticBindingKind::LocalVariable);
    attachDeclarationDocumentation(symbol, stmt);
    element.declaration = &binding;
    element.binding.symbol = symbol;
    semanticModel.record(binding, element.binding);
    structuredBindingElements.insert(&binding);
    (void)declare(binding.name(), element.binding.type, false,
                  SemanticBindingKind::LocalVariable, &binding);
    result.elements.emplace_back(std::move(element));
  }

  semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                  .span = tokenSpan(stmt.autoKeyword()),
                                  .kind = SemanticOccurrenceKind::InferredType,
                                  .name = stmt.autoKeyword().lexeme,
                                  .type = sourceType,
                                  .traits = typeTraits(sourceType),
                                  .access = AccessMode::ReadOnly,
                                  .mutableBinding = false});
  semanticModel.record(stmt, std::move(result));
}

auto SemanticVisitor::Impl::visitTypeAliasDecl(const TypeAliasDecl &stmt)
    -> void {
  const TypeAliasInfo *info = semanticModel.findTypeAlias(stmt);
  const SemanticType type =
      info == nullptr ? SemanticType::Unknown : info->type;
  validateConcreteEnumPayloads(type, stmt.name());
  const SymbolId symbol = recordToolingSymbol(
      stmt.name(), SymbolKind::TypeAlias,
      info == nullptr ? qualifiedName(currentNamespace, stmt.name().lexeme)
                      : info->qualifiedName,
      type);
  attachDeclarationDocumentation(symbol, stmt);
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(stmt.name()),
       .kind = SemanticOccurrenceKind::TypeAlias,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = stmt.name().lexeme,
       .type = type,
       .typeAlias = &stmt});
}

auto SemanticVisitor::Impl::visitConceptDecl(const ConceptDecl &stmt) -> void {
  const auto found = conceptDeclIds.find(&stmt);
  if (found == conceptDeclIds.end() || found->second == 0 ||
      found->second > concepts.size()) {
    return;
  }
  const RegisteredConcept &conceptInfo = concepts[found->second - 1];
  attachDeclarationDocumentation(conceptInfo.symbol, stmt);
  semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                  .span = tokenSpan(stmt.name()),
                                  .kind = SemanticOccurrenceKind::Symbol,
                                  .symbol = conceptInfo.symbol,
                                  .roles = OccurrenceRole::Declaration |
                                           OccurrenceRole::Definition |
                                           OccurrenceRole::TypeUse,
                                  .name = stmt.name().lexeme,
                                  .conceptDeclaration = &stmt});
  const std::size_t count = std::min(stmt.typeParameters().size(),
                                     conceptInfo.parameterSymbols.size());
  for (std::size_t index = 0; index < count; ++index) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(stmt.typeParameters()[index]),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = conceptInfo.parameterSymbols[index],
         .roles = OccurrenceRole::Declaration | OccurrenceRole::TypeUse,
         .name = stmt.typeParameters()[index].lexeme});
  }
}
auto SemanticVisitor::Impl::visitVariableDecl(const VariableDecl &stmt)
    -> void {
  const DeclarationOrderScope declarationOrder{*this, stmt};
  const std::size_t declarationDiagnosticStart = diagnostics.size();
  if (stmt.type().name.last().kind == TokenKind::AUTO) {
    semanticModel.recordFullExpression(stmt, stmt.initializer());
    analyzeInferredVariable(stmt);
    return;
  }
  validateType(stmt.type());
  const bool localReference = functionDepth > 0;
  const bool instanceField =
      currentClass && functionDepth == 0 && !stmt.isStatic();
  const ClassInfo *fieldOwner =
      instanceField ? &classInfo(*currentClass) : nullptr;
  validateReferencePlacement(
      stmt.type(), localReference || instanceField,
      localReference ? "local binding"
                     : (instanceField ? "instance field" : "storage"));
  const SemanticType declaredType =
      typeOf(stmt.type(),
             stmt.isMutable() ? Mutability::Mutable : Mutability::Immutable);
  const bool openField = declaredType.kind == SemanticType::Class &&
                         !isConcreteExactSpecializationType(declaredType);
  const ExprPtr &initializer =
      !stmt.initializer() && instanceField && !openField &&
              declaredType.kind == SemanticType::Class &&
              isDefaultInitializable(declaredType, true)
          ? semanticModel.implicitFieldInitializer(stmt)
          : stmt.initializer();
  semanticModel.recordFullExpression(stmt, initializer);
  if (catalog.sealed)
    validateOrdinaryClassLayout(declaredType);
  if (declaredType.kind == SemanticType::Array &&
      containsOwningPayloadEnum(declaredType)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2067", DiagnosticPhase::Semantics, stmt.name(),
        "Fixed array '" + stmt.name().lexeme + "' has type '" +
            typeSpelling(declaredType) +
            "', but owning payload-enum elements are not supported in this "
            "bounded containment slice.");
    diagnostic.hints.emplace_back(
        "Keep each owning payload enum in its own local binding until "
        "aggregate element lifecycle state is represented explicitly.");
    diagnostics.emplace_back(std::move(diagnostic));
  } else if (instanceField && declaredType.kind == SemanticType::Enum &&
             containsOwningPayloadEnum(declaredType) &&
             !(fieldOwner != nullptr &&
               (fieldOwner->kind == ClassKind::Class ||
                fieldOwner->kind == ClassKind::Struct) &&
               !fieldOwner->polymorphic && !fieldOwner->cAbiRecord &&
               !fieldOwner->cOpaqueHandle && !fieldOwner->storedReference &&
               !fieldOwner->compilerPrivate &&
               fieldOwner->compilerCapability ==
                   CompilerCapabilityTypeKind::None)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2067", DiagnosticPhase::Semantics, stmt.name(),
        "Instance field '" + stmt.name().lexeme + "' has type '" +
            typeSpelling(declaredType) +
            "', which contains owning payload-enum state unsupported for "
            "this owner in the bounded direct-field slice.");
    diagnostic.hints.emplace_back(
        "A direct owning payload-enum field is supported only in an ordinary "
        "non-polymorphic GTI class or struct; keep "
        "layout-constrained and compiler-owned shapes in separate bindings.");
    diagnostics.emplace_back(std::move(diagnostic));
  } else if ((declaredType.kind == SemanticType::Expected ||
              declaredType.kind == SemanticType::Unexpected) &&
             containsOwningPayloadEnum(declaredType)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2067", DiagnosticPhase::Semantics, stmt.name(),
        "Binding '" + stmt.name().lexeme + "' has type '" +
            typeSpelling(declaredType) +
            "', but direct owning payload-enum alternatives in a result "
            "carrier are not supported in this containment slice.");
    diagnostic.hints.emplace_back(
        "Store the enum in an ordinary class or struct so the containing "
        "value has an explicit structural lifecycle.");
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const SemanticBindingKind bindingKind =
      currentClass && functionDepth == 0
          ? (stmt.isStatic() ? SemanticBindingKind::StaticField
                             : SemanticBindingKind::Field)
          : (functionDepth > 0 ? SemanticBindingKind::LocalVariable
                               : SemanticBindingKind::GlobalVariable);
  const bool internalLinkage = stmt.isStatic() && !currentClass;
  const SymbolId symbol = recordBindingOccurrence(
      stmt.name(), declaredType, stmt.isMutable(), bindingKind,
      bindingKind == SemanticBindingKind::StaticField, internalLinkage);
  if (!instanceAnalysisActive && !isCompilerTrustedRequester(stmt.name()) &&
      bindingKind == SemanticBindingKind::LocalVariable && stmt.isMutable() &&
      stmt.mutKeyword() && !stmt.isRangeBinding() &&
      declaredType.kind != SemanticType::Reference && symbol != 0) {
    requestSession.mutableLocalCandidates.push_back(
        {.symbol = symbol,
         .keyword = *stmt.mutKeyword(),
         .name = stmt.name(),
         .owner = currentProgramEffectOwner});
  }
  attachDeclarationDocumentation(symbol, stmt);
  BindingInfo info =
      bindingInfo(declaredType, stmt.isMutable() ? AccessMode::Mutable
                                                 : AccessMode::ReadOnly);
  info.symbol = symbol;
  info.staticStorage = stmt.isStatic();
  info.internalLinkage = internalLinkage;
  semanticModel.record(stmt, std::move(info));
  if (std::find(requestSession.invalidOrdinaryLayouts.begin(),
                requestSession.invalidOrdinaryLayouts.end(),
                declaredType) != requestSession.invalidOrdinaryLayouts.end()) {
    // Preserve declaration identity for recovery, but do not attempt lifecycle
    // construction of a type whose owning layout proof already failed.
    return;
  }
  if (functionDepth > 0)
    recordProgramCleanupEffects(declaredType, tokenSpan(stmt.name()));
  const bool globalStorage =
      functionDepth == 0 && (!currentClass || stmt.isStatic());
  SemanticType initializerType = SemanticType::Unknown;
  if (declaredType == SemanticType::Void) {
    report(stmt.type().name.last(), "Variables cannot have type void.");
  } else if (declaredType.kind == SemanticType::UniqueOwner && globalStorage) {
    report(stmt.name(),
           "Compiler-private unique owners can only be local bindings, "
           "function values, or class fields.",
           "GTI-S2018");
  } else if (declaredType.kind == SemanticType::UniqueOwner && !initializer) {
    report(stmt.name(),
           "Compiler-private unique owner bindings require an initializer.",
           "GTI-S2018");
  } else if (typeTraits(declaredType).ownership == OwnershipKind::Unique &&
             globalStorage) {
    report(stmt.name(),
           "Unique owners can only be local bindings, function values, or "
           "class fields.",
           "GTI-S2018");
  } else if (typeTraits(declaredType).containsBorrowedState && globalStorage) {
    report(stmt.name(),
           "Values carrying stored references cannot have global or static "
           "storage duration.",
           "GTI-S2045");
  } else if (globalStorage && requiresActiveCleanupFor(declaredType)) {
    const std::string storageKind =
        currentClass ? "Static field" : "Namespace global";
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2061", DiagnosticPhase::Semantics, stmt.name(),
        storageKind + " '" + stmt.name().lexeme + "' has type '" +
            typeSpelling(declaredType) +
            "', which requires active cleanup; GTI does not define "
            "global or static shutdown.");
    if (const std::optional<ActiveCleanupCause> cause =
            activeCleanupCause(declaredType);
        cause && cause->source != nullptr) {
      diagnostic.related.push_back({tokenSpan(*cause->source), cause->message});
    }
    diagnostic.hints.emplace_back(
        "Move the value into a local binding or remove its cleanup-owning "
        "component.");
    diagnostics.emplace_back(std::move(diagnostic));
  } else if (reportConcurrentGlobalPolicyFailure(stmt, declaredType,
                                                 globalStorage)) {
    // The helper emits the one profile-specific policy diagnostic.
  } else if (instanceField && declaredType.kind != SemanticType::Reference &&
             typeTraits(declaredType).containsBorrowedState &&
             (!currentClass ||
              storedReferencePaths(SemanticType::classType(*currentClass))
                  .empty())) {
    if (std::none_of(diagnostics.begin(), diagnostics.end(),
                     [&](const Diagnostic &diagnostic) {
                       return diagnostic.code == "GTI-S2045" &&
                              diagnostic.primary == tokenSpan(stmt.name());
                     })) {
      report(stmt.name(),
             "Borrowed field state must use checked stored-reference paths "
             "through ordinary nominal fields; fallible and other non-nominal "
             "carriers cannot be stored as fields.",
             "GTI-S2045");
    }
  } else if (declaredType.kind == SemanticType::Reference && !initializer &&
             !instanceField) {
    report(stmt.name(), "Reference bindings require an initializer.",
           "GTI-S2017");
  } else if (!initializer) {
    const bool field = currentClass && functionDepth == 0 && !stmt.isStatic();
    const bool passiveStorageField =
        field && currentClass &&
        (classInfo(*currentClass).cAbiRecord ||
         classInfo(*currentClass).kind == ClassKind::Union);
    if (stmt.isConstexpr()) {
      // The constexpr contract reports its more specific initializer error.
    } else if ((declaredType.kind == SemanticType::RawPointer ||
                declaredType.kind == SemanticType::NativeFunction) &&
               !passiveStorageField) {
      report(stmt.name(),
             declaredType.kind == SemanticType::NativeFunction
                 ? "Native function bindings and fields require an "
                   "explicit initializer; use 'nullptr' when no callback "
                   "is installed."
                 : "Raw pointer bindings and fields require an explicit "
                   "initializer; use 'nullptr' when no address is "
                   "available.",
             declaredType.kind == SemanticType::NativeFunction ? "GTI-S2076"
                                                               : "GTI-S2056");
    } else if (stmt.isStatic() && currentClass) {
      report(stmt.name(),
             "Static class and struct fields require an in-class "
             "initializer.",
             "GTI-S2039");
    } else if (!field && declaredType.kind == SemanticType::Array) {
      report(stmt.name(),
             "Fixed array variables require an initializer; use '{}' to "
             "default-initialize every element.",
             "GTI-S2015");
    } else if (!field && declaredType.kind == SemanticType::Class) {
      report(stmt.name(),
             "Class and struct variables require explicit construction.");
    } else if (!field && declaredType.kind == SemanticType::Enum) {
      report(stmt.name(),
             "Scoped enum variables require an explicit enumerator "
             "initializer.",
             "GTI-S2036");
    } else if (!field && !stmt.isMutable()) {
      report(stmt.name(), "Immutable variable must have an initializer.");
    } else if (!field && !globalStorage && !stmt.isRangeBinding() &&
               declaredType.kind != SemanticType::Unknown) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2000", DiagnosticPhase::Semantics, stmt.name(),
                         "Mutable local variable '" + stmt.name().lexeme +
                             "' requires an initializer.");
      diagnostic.hints.emplace_back(
          "Initialize the binding where it is declared. A later assignment "
          "does not initialize an otherwise uninitialized local declaration.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  if (initializer) {
    const bool enclosingFieldInitializer = analyzingFieldInitializer;
    const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
    if (globalStorage && symbol != 0) {
      currentProgramEffectOwner = {.kind = ProgramEffectOwnerKind::Initializer,
                                   .id = symbol};
    } else if (instanceField && currentClass) {
      currentProgramEffectOwner = {
          .kind = ProgramEffectOwnerKind::FieldInitializers,
          .id = *currentClass};
    }
    analyzingFieldInitializer = currentClass && functionDepth == 0;
    const bool directInitializer =
        exprAs<DirectInitializer>(initializer.get()) != nullptr;
    const bool establishesLoan =
        !directInitializer && (declaredType.kind == SemanticType::Reference ||
                               typeTraits(declaredType).containsBorrowedState);
    if (establishesLoan) {
      ++suppressLoanPlaceAccessDepth;
      ++programStoragePlaceUseDepth;
    }
    initializerType = analyzeInitializer(initializer, declaredType, true);
    if (declaredType.kind == SemanticType::Reference &&
        declaredType.referenceAccess == AccessMode::Mutable) {
      recordMutableBindingUse(initializer);
    }
    if (establishesLoan) {
      --suppressLoanPlaceAccessDepth;
      --programStoragePlaceUseDepth;
    }
    analyzingFieldInitializer = enclosingFieldInitializer;
    currentProgramEffectOwner = enclosingEffectOwner;
  }

  const ExpressionInfo *initializerInfo =
      initializer ? semanticModel.findExpression(*initializer) : nullptr;
  const bool ownerDependentText =
      (declaredType.kind == SemanticType::StringView ||
       declaredType.kind == SemanticType::CString) &&
      initializerInfo != nullptr &&
      initializerInfo->traits.containsBorrowedState;
  if (ownerDependentText) {
    const bool cString = declaredType.kind == SemanticType::CString;
    if (globalStorage) {
      report(stmt.name(),
             cString ? "An owner-dependent c_string cannot have "
                       "global or static storage duration because "
                       "its owner dependency is local."
                     : "A dynamic std::string_view cannot have "
                       "global or static storage duration because "
                       "its owner dependency is local.",
             "GTI-S2045");
    } else if (instanceField) {
      report(stmt.name(),
             cString ? "An owner-dependent c_string cannot be stored "
                       "in a field in the single-owner text-borrow "
                       "model."
                     : "A dynamic std::string_view cannot be stored "
                       "in a field in the single-owner view model.",
             "GTI-S2045");
    }
    BindingInfo updated = *semanticModel.findBinding(stmt);
    updated.traits = initializerInfo->traits;
    semanticModel.record(stmt, std::move(updated));
  }

  const bool initializerAssignable =
      !initializer ||
      isOwnershipAssignable(declaredType, initializerType, initializer);
  const std::optional<ConstantValue> constant =
      constexprBindingValue(stmt, declaredType,
                            diagnostics.size() == declarationDiagnosticStart &&
                                initializerAssignable);
  if (constant) {
    BindingInfo updated = *semanticModel.findBinding(stmt);
    updated.constant = *constant;
    semanticModel.record(stmt, std::move(updated));
  }

  const auto rawOrigins = declaredType.kind == SemanticType::RawPointer
                              ? rawPointerOrigins(initializer)
                              : std::vector<RawPointerOrigin>{};
  const auto rawFields = declaredType.kind != SemanticType::RawPointer
                             ? rawPointerFields(initializer.get())
                             : std::vector<ProjectedRawPointerOrigins>{};
  if (!predeclaredVariables.contains(&stmt)) {
    if (functionDepth == 0 && !currentClass) {
      declareNamespaceSymbol(currentNamespace, stmt.name(), declaredType,
                             stmt.isMutable(), stmt.isStatic(), symbol, &stmt,
                             constant);
    } else {
      declare(stmt.name(), declaredType, stmt.isMutable(), bindingKind, &stmt,
              nullptr, constant);
    }
  } else if (constant && currentClass) {
    if (Symbol *symbolInScope = resolveMutable(stmt.name())) {
      symbolInScope->constant = *constant;
    }
  }

  if (auto *target = resolveMutable(stmt.name())) {
    target->rawPointerOrigins = rawOrigins;
    target->rawPointerFields = rawFields;
  }
  if (initializer && declaredType.kind == SemanticType::Reference &&
      exprAs<DirectInitializer>(initializer.get()) == nullptr) {
    validateReferenceBinding(declaredType, initializerType, initializer);
    if (declaredType.arguments.size() == 1 &&
        !isDirectOwnerType(declaredType.arguments[0])) {
      recordRetainedBorrow(stmt, initializer);
    }
  } else if (initializer) {
    if (initializerType != SemanticType::Unknown &&
        (typeTraits(declaredType).containsBorrowedState ||
         ownerDependentText)) {
      validateStoredBorrowInitialization(declaredType, initializer);
      recordRetainedBorrow(stmt, initializer);
    }
    if (!initializerAssignable && declaredType != SemanticType::Unknown &&
        initializerType != SemanticType::Unknown) {
      report(expressionToken(initializer),
             "Cannot initialize '" + stmt.name().lexeme + "' of type '" +
                 typeSpelling(declaredType) + "' with a value of type '" +
                 typeSpelling(initializerType) + "'.",
             "GTI-S2003");
      if (isMoveOnlyOwnerType(declaredType) &&
          declaredType == initializerType) {
        diagnostics.back().hints.emplace_back(
            "Move-only owners cannot be copied; transfer ownership "
            "explicitly with std::move(owner).");
      }
    }
  }
}
auto SemanticVisitor::Impl::visitWhileStmt(const WhileStmt &stmt) -> void {
  semanticModel.recordFullExpression(stmt, stmt.condition());
  noteUnsupportedNestedLoanControlFlow();
  beginLoanFlowLoop(stmt);
  beginRawPointerLoop();
  const ScopeStack beforeCondition = scopes;
  const SemanticType conditionType =
      analyzeExpectedCallableResult(stmt.condition(), SemanticType::Bool);
  requireBool(stmt.condition(), conditionType,
              expressionToken(stmt.condition()),
              "While condition must be bool.");
  const ScopeStack beforeLoop = scopes;
  const std::optional<bool> condition = constantBoolean(stmt.condition());
  ++loopDepth;
  beginValueControlFlow(ValueControlFlowKind::Loop, stmt);
  if (condition == false) {
    analyzeMoveUnreachable(stmt.body());
  } else {
    analyzeLoopBody(stmt.body());
  }
  const ScopeStack bodyScopes = scopes;
  ValueControlFlowContext valueFlow = endValueControlFlow();
  --loopDepth;

  const FlowSummary bodyFlow = summarizeFlow(stmt.body().get());
  std::vector<ScopeStack> backedgeStates;
  if (condition != false && bodyFlow.canFallThrough) {
    backedgeStates.push_back(bodyScopes);
  }
  if (condition != false) {
    for (ScopeStack &continueState : valueFlow.continueStates) {
      backedgeStates.push_back(std::move(continueState));
    }
  }
  const ScopeStack afterIteration =
      mergedValueStates(beforeLoop, backedgeStates);
  if (!backedgeStates.empty()) {
    validateLoopBackedge(beforeCondition, afterIteration,
                         expressionToken(stmt.condition()));
  }

  const ScopeStack loanScopes =
      mergedValueStates(beforeLoop, {beforeLoop, bodyScopes});
  std::vector<ScopeStack> exitStates;
  if (condition != true) {
    exitStates.push_back(beforeLoop);
    if (!backedgeStates.empty() && condition != false) {
      exitStates.push_back(afterIteration);
    }
  }
  for (ScopeStack &breakState : valueFlow.breakStates) {
    exitStates.push_back(std::move(breakState));
  }
  const ScopeStack moveScopes = exitStates.empty()
                                    ? beforeLoop
                                    : mergedValueStates(beforeLoop, exitStates);
  scopes = loanScopes;
  applyMoveStates(moveScopes);
  endRawPointerLoop(afterIteration, !backedgeStates.empty());
  endLoanFlowLoop();
}

auto SemanticVisitor::Impl::visitAssignExpr(const Assign &expr) -> void {
  const bool qualified = expr.path().segments.size() > 1;
  if (qualified && reportCompilerPrivatePath(expr.path())) {
    currentType = analyze(expr.value());
    return;
  }
  if (qualified) {
    recordQualifiedPathUses(expr.path());
  }
  const Symbol *resolvedSymbol =
      qualified ? resolveQualified(expr.path()) : resolve(expr.name());
  if (resolvedSymbol == nullptr) {
    if (!qualified && resolveValueParameter(expr.name())) {
      analyze(expr.value());
      report(expr.name(),
             "Cannot assign to compile-time value parameter '" +
                 expr.name().lexeme + "'.",
             "GTI-S2026");
      currentType = SemanticType::UInt64;
      return;
    }
    if (!qualified && !reportMissingLambdaCapture(expr.name()) &&
        !reportInvisibleSymbol(expr.name(), expr.name().lexeme,
                               resolveGlobally(expr.name()))) {
      report(expr.name(), "Undefined variable '" + expr.name().lexeme + "'.",
             "GTI-S2001");
    } else if (qualified) {
      report(expr.name(),
             "Undefined qualified assignment target '" +
                 pathSpelling(expr.path()) + "'.",
             "GTI-S2001");
    }
    currentType = analyze(expr.value());
    return;
  }
  const Symbol symbol = *resolvedSymbol;
  if (qualified && !isCompilerTrustedRequester(expr.name()) &&
      symbol.compilerPrivate) {
    reportCompilerPrivateAccess(expr.name(), pathSpelling(expr.path()));
    currentType = analyze(expr.value());
    return;
  }
  if (qualified && symbol.ownerClass != 0 &&
      symbol.access == AccessModifier::Private &&
      !isWithinClass(symbol.ownerClass)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, expr.name(),
        "Static member '" + expr.name().lexeme + "' of '" +
            classInfo(symbol.ownerClass).name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(symbol.declaration), "Static member declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const SemanticType targetType = symbol.type.kind == SemanticType::Reference &&
                                          symbol.type.arguments.size() == 1
                                      ? symbol.type.arguments[0]
                                      : symbol.type;
  Symbol *placeTarget = qualified ? resolveQualifiedPlaceSymbol(expr.path())
                                  : resolvePlaceSymbol(expr.name());
  SemanticPlace targetPlace =
      placeTarget == nullptr ? SemanticPlace{} : placeForSymbol(*placeTarget);
  // The result of assignment is a value, but its destination is still an
  // exact semantic place. Preserve it independently of result category so
  // implicit receiver fields cannot become namespace/global symbol storage
  // during HIR/MIR lowering.
  if (targetPlace.root != nullptr)
    semanticModel.recordPlace(expr, placeKey(targetPlace));
  if (expr.oper().kind != TokenKind::EQUAL &&
      symbol.valueState != ValueState::Available) {
    reportUnavailableValue(expr.name(), symbol);
  }
  const bool simpleAssignment = expr.oper().kind == TokenKind::EQUAL;
  bool targetStateValid = true;
  if (!simpleAssignment) {
    if (const ProjectedValueState *state =
            unavailableProjectedState(targetPlace)) {
      reportUnavailablePlace(expr.name(), *state, &targetPlace);
      targetStateValid = false;
    }
  } else if (const ProjectedValueState *state =
                 unavailableAncestorState(targetPlace)) {
    reportUnavailablePlace(expr.name(), *state, &targetPlace);
    targetStateValid = false;
  }
  SemanticType valueType;
  bool valueAnalyzed = false;
  const std::optional<OverloadedOperator> mutationOperator =
      compoundOperator(expr.oper().kind);
  if (!simpleAssignment && mutationOperator &&
      (targetType.kind == SemanticType::Class ||
       targetType.kind == SemanticType::TypeParameter)) {
    valueType = analyze(expr.value());
    valueAnalyzed = true;
    const bool symbolicMutation =
        targetType.kind == SemanticType::TypeParameter &&
        hasSymbolicOperatorRequirement(
            *mutationOperator, targetType,
            std::span<const SemanticType>(&valueType, 1));
    if (targetType.kind == SemanticType::Class || symbolicMutation) {
      ExprPtr receiver;
      if (expr.path().segments.size() == 1) {
        receiver = std::make_unique<Variable>(expr.name());
      } else {
        receiver =
            std::make_unique<QualifiedName>(NamePath(expr.path().segments));
      }
      const ExpressionInfo receiverInfo =
          classifyExpression(*receiver, targetType);
      const std::optional<CallableCandidate> selected = resolveOperator(
          expr, *mutationOperator, receiver, targetType, expr.oper(),
          std::span<const SemanticType>(&valueType, 1),
          std::span<const ExprPtr>(&expr.value(), 1), false, std::nullopt,
          OperatorReceiverState{.place = targetPlace,
                                .mutableReceiver =
                                    receiverInfo.access == AccessMode::Mutable,
                                .info = receiverInfo});
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }
  }
  if (simpleAssignment && targetType.kind == SemanticType::Class) {
    valueType = analyze(expr.value());
    if (hasExactOperatorCandidate(OverloadedOperator::Assignment, targetType,
                                  valueType)) {
      ExprPtr receiver;
      if (expr.path().segments.size() == 1) {
        receiver = std::make_unique<Variable>(expr.name());
      } else {
        receiver =
            std::make_unique<QualifiedName>(NamePath(expr.path().segments));
      }
      const ExpressionInfo receiverInfo =
          classifyExpression(*receiver, targetType);
      const std::optional<CallableCandidate> selected = resolveOperator(
          expr, OverloadedOperator::Assignment, receiver, targetType,
          expr.oper(), std::span<const SemanticType>(&valueType, 1),
          std::span<const ExprPtr>(&expr.value(), 1), false, std::nullopt,
          OperatorReceiverState{.place = targetPlace,
                                .mutableReceiver =
                                    receiverInfo.access == AccessMode::Mutable,
                                .info = receiverInfo});
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
      return;
    }
  } else if (!valueAnalyzed) {
    valueType = simpleAssignment ? analyzeInitializer(expr.value(), targetType)
                                 : analyze(expr.value());
  }
  if (simpleAssignment &&
      (targetType.kind == SemanticType::StringView ||
       targetType.kind == SemanticType::CString) &&
      isOwnerDependentTextExpression(expr.value())) {
    reportOwnerDependentTextStorage(expr.value(),
                                    "assigned into an existing binding");
  }
  // Recursive RHS analysis can merge short-circuit branch scopes and move
  // their Symbol storage. Re-resolve the assignment place before retaining
  // or comparing any pointer-backed SemanticPlace state.
  placeTarget = qualified ? resolveQualifiedPlaceSymbol(expr.path())
                          : resolvePlaceSymbol(expr.name());
  targetPlace =
      placeTarget == nullptr ? SemanticPlace{} : placeForSymbol(*placeTarget);
  const auto *moveCall = exprAs<Call>(expr.value().get());
  const bool explicitMove =
      moveCall != nullptr &&
      resolvedIntrinsicKind(*moveCall) == IntrinsicKind::Move &&
      moveCall->arguments().size() == 1;
  const Variable *movedSource =
      explicitMove ? movedVariable(moveCall->arguments().front()) : nullptr;
  const bool directSelfMove = movedSource != nullptr &&
                              movedSource->name().lexeme == expr.name().lexeme;
  const bool directSelfPlace =
      samePlace(targetPlace, movedSourcePlace(expr.value()));
  if (directSelfMove || directSelfPlace) {
    report(expr.name(), "Cannot move a binding into itself.", "GTI-S2018");
  }
  if (!symbol.assignable) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2002", DiagnosticPhase::Semantics, expr.name(),
        symbol.lambdaCapture ? "Cannot assign to immutable lambda capture '" +
                                   expr.name().lexeme + "'."
                             : "Cannot assign to immutable binding '" +
                                   expr.name().lexeme + "'.");
    if (!symbol.declaration.lexeme.empty()) {
      diagnostic.related.push_back(
          {tokenSpan(symbol.declaration), "Binding declared here."});
    }
    diagnostic.hints.emplace_back(
        symbol.lambdaCapture
            ? "Lambda captures are value snapshots and remain immutable."
        : symbol.constant
            ? "A constant cannot be mutated; use separate mutable storage if a "
              "changing value is needed."
        : target.executionProfile == ExecutionProfile::Concurrent &&
                (symbol.bindingKind == SemanticBindingKind::GlobalVariable ||
                 symbol.bindingKind == SemanticBindingKind::StaticField)
            ? "Concurrent globals and static fields must stay immutable. Use "
              "local mutable storage or an immutable value with an approved "
              "atomic or mutex contract for synchronized mutation."
            : "Bindings are immutable by default; add 'mut' to the "
              "declaration if mutation is required.");
    diagnostics.emplace_back(std::move(diagnostic));
  } else if (symbol.ownerClass != 0 && !symbol.staticMember &&
             !receiverAllowsMutation(currentReceiverMutability)) {
    const std::optional<ResolvedFieldUse> root =
        receiverRestrictedField(expr.name(), symbol);
    if (!root || !reportReceiverRestrictedMutation(expr.name(), *root)) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
    }
  } else {
    reportLoanPlaceConflict(
        expr.name(), targetPlace, AccessMode::Mutable,
        "Cannot assign to storage while an overlapping borrow may still be "
        "live.");
  }
  const bool valueAssignable =
      !simpleAssignment ||
      isOwnershipAssignment(targetType, valueType, expr.value());
  if (simpleAssignment && !valueAssignable) {
    const bool ownerAssignment =
        isMoveOnlyOwnerType(targetType) && targetType == valueType;
    const auto *valueInfo = semanticModel.findExpression(*expr.value());
    const bool moving =
        valueInfo != nullptr && valueInfo->category == ValueCategory::Value;
    report(expressionToken(expr.value()),
           ownerAssignment
               ? (moving ? "Move assignment is unavailable for type '"
                         : "Cannot copy assign move-only type '") +
                     typeSpelling(targetType) + "'."
               : "Cannot assign a value of type '" + typeSpelling(valueType) +
                     "' to '" + expr.name().lexeme + "' of type '" +
                     typeSpelling(targetType) + "'.",
           "GTI-S2003");
    if (ownerAssignment) {
      diagnostics.back().hints.emplace_back(
          !typeTraits(targetType).moveAssignable
              ? "This type does not support move assignment; initialize a "
                "new binding from an owned value instead."
              : "Move-only owners cannot be copied; use std::move(owner) "
                "to transfer ownership.");
    }
  }
  if (!simpleAssignment) {
    if (targetType.kind == SemanticType::RawPointer &&
        (expr.oper().kind == TokenKind::PLUS_EQUAL ||
         expr.oper().kind == TokenKind::MINUS_EQUAL) &&
        isInteger(valueType) && targetType.arguments.size() == 1 &&
        targetType.arguments.front() != SemanticType::Void &&
        !isCOpaqueHandleType(targetType.arguments.front()) &&
        symbol.assignable) {
      requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                    "Raw pointer arithmetic");
    }
    validateCompoundAssignment(expr.oper(), targetType, valueType,
                               expr.value().get());
  }
  if (simpleAssignment && symbol.assignable && valueAssignable &&
      !directSelfMove && !directSelfPlace && targetStateValid) {
    if (targetType.kind == SemanticType::Class ||
        targetType.kind == SemanticType::Array)
      invalidateRawPointerOrigins(targetPlace);
    assignRawPointerFields(targetPlace, expr.value().get());
    if (!qualified) {
      if (Symbol *target = resolveMutable(expr.name())) {
        if (tracksValueState(*target)) {
          const ValueState reinitializeStateBefore =
              targetPlace.projections.empty()
                  ? target->valueState
                  : projectedValueState(*target, targetPlace.projections);
          target->valueState = ValueState::Available;
          target->moveOrigins.clear();
          semanticModel.recordOwnershipEvent(
              expr, {.kind = OwnershipEventKind::Reinitialize,
                     .place = placeKey(placeForSymbol(*target)),
                     .before = reinitializeStateBefore,
                     .after = ValueState::Available});
        }
        reinitializePlace(placeForSymbol(*target));
      }
    }
  }
  currentType = targetType;
}

auto SemanticVisitor::Impl::analyzeCollectionInitializer(
    const ArrayInitializer &expr, const SemanticType &target) -> void {
  const ClassInfo *owner = classInfo(target);
  const auto reject = [&](const Token &site, const std::string &message) {
    report(site, message, "GTI-S2092");
  };
  const auto select =
      [&](const std::string &name) -> std::optional<CallableCandidate> {
    if (owner == nullptr) {
      return std::nullopt;
    }
    const auto member = owner->members.find(name);
    if (member == owner->members.end() ||
        member->second.access != AccessModifier::Public) {
      return std::nullopt;
    }
    const Symbol symbol = substituteSymbol(member->second.symbol, target);
    if (symbol.overloads.size() != 1) {
      return std::nullopt;
    }
    const CallableCandidate &candidate = symbol.overloads.front();
    if (!candidate.staticMember || candidate.compilerPrivate ||
        candidate.declaration == nullptr ||
        candidate.bodyDeclaration()->body() == nullptr ||
        candidate.declaration->hasCLinkage() ||
        candidate.record().requiresUnsafe ||
        candidate.record().intrinsic != IntrinsicKind::None ||
        !candidate.record().genericParameters.empty() ||
        candidate.record().parameterPack ||
        candidate.requiredParameterCount != candidate.parameterTypes.size() ||
        candidate.returnType != target) {
      return std::nullopt;
    }
    return candidate;
  };
  const auto initialize = select("collection_init");
  const auto insert = select("collection_insert");
  if (!initialize || !insert ||
      initialize->parameterTypes !=
          std::vector<SemanticType>{SemanticType::UInt64} ||
      insert->parameterTypes.size() != 2 ||
      insert->parameterTypes.front() != target ||
      insert->parameterTypes.back().kind == SemanticType::Reference ||
      insert->parameterTypes.back().kind == SemanticType::Array ||
      typeTraits(target).containsBorrowedState || !typeTraits(target).movable) {
    reject(expr.brace(),
           "Collection initialization requires public, non-overloaded static "
           "functions 'C collection_init(uint64_t count)' and "
           "'C collection_insert(C collection, T element)' with owned value "
           "parameters and no method-local generics or defaults.");
    currentType = SemanticType::Unknown;
    return;
  }
  // These are ordinary statically selected calls. Substitute owner arguments
  // above, then use the same constraint authority as an explicit call; an
  // unused constrained hook must not disable explicit container construction.
  for (const CallableCandidate *hook : {&*initialize, &*insert}) {
    if (const auto failure = firstRequirementFailure(hook->requirements)) {
      reportConstraintFailure(expr.brace(), *failure);
      diagnostics.back().related.push_back(
          {tokenSpan(hook->declaration->name()),
           "This collection initialization hook requires the constraint."});
      currentType = SemanticType::Unknown;
      return;
    }
  }
  const SemanticType elementType = insert->parameterTypes.back();
  if (elementType == SemanticType::Void ||
      elementType.kind == SemanticType::Expected ||
      elementType.kind == SemanticType::Unexpected ||
      elementType.kind == SemanticType::Lambda ||
      elementType.kind == SemanticType::FunctionItem ||
      typeTraits(elementType).containsBorrowedState) {
    reject(expr.brace(), "Collection elements require scalar, class, or enum "
                         "values without stored borrow dependencies; expected "
                         "values and confined callables are not supported.");
    currentType = SemanticType::Unknown;
    return;
  }
  bool valid = true;
  for (const ExprPtr &element : expr.elements()) {
    const SemanticType actual = analyzeInitializer(element, elementType);
    if (actual == SemanticType::Unknown) {
      valid = false;
      continue;
    }
    if (actual != elementType ||
        !isOwnershipAssignable(elementType, actual, element)) {
      reject(expressionToken(element),
             "Cannot initialize collection element of type '" +
                 typeSpelling(elementType) + "' from '" + typeSpelling(actual) +
                 "'.");
      diagnostics.back().related.push_back(
          {tokenSpan(insert->declaration->parameters().back().name),
           "The collection protocol declares its element type here."});
      diagnostics.back().hints.emplace_back(
          actual == elementType && isMoveOnlyOwnerType(actual)
              ? "Transfer a named move-only element with std::move, or supply "
                "a fresh value."
              : "Elements require the exact declared type; convert explicitly "
                "when necessary.");
      valid = false;
    }
    if ((elementType.kind == SemanticType::StringView ||
         elementType.kind == SemanticType::CString) &&
        isOwnerDependentTextExpression(element)) {
      reportOwnerDependentTextStorage(element, "stored in a collection");
      valid = false;
    }
  }
  if (valid) {
    semanticModel.recordCollection(expr, {.type = target,
                                          .elementType = elementType,
                                          .initialize = initialize->id,
                                          .insert = insert->id});
  }
  currentType = valid ? target : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::visitArrayInitializerExpr(
    const ArrayInitializer &expr) -> void {
  if (contextualInitializerType &&
      contextualInitializerType->kind == SemanticType::Class) {
    analyzeCollectionInitializer(
        expr, selectExactClassSpecialization(*contextualInitializerType));
    return;
  }
  if (!contextualInitializerType ||
      contextualInitializerType->kind != SemanticType::Array ||
      contextualInitializerType->arguments.size() != 1) {
    for (const ExprPtr &element : expr.elements()) {
      analyze(element);
    }
    report(expr.brace(),
           "Array initializer requires a fixed array type from its context.",
           "GTI-S2015");
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType arrayType = *contextualInitializerType;
  const SemanticType elementType = arrayType.arguments[0];
  if (arrayType.arrayLengthParameterId == 0 && !expr.elements().empty() &&
      expr.elements().size() != arrayType.arrayLength) {
    report(expr.brace(),
           "Fixed array initializer provides " +
               std::to_string(expr.elements().size()) + " elements but '" +
               typeSpelling(arrayType) + "' requires exactly " +
               std::to_string(arrayType.arrayLength) + ".",
           "GTI-S2015");
  }
  if (expr.elements().empty() &&
      (arrayType.arrayLengthParameterId != 0 || arrayType.arrayLength != 0) &&
      !isDefaultInitializable(elementType)) {
    report(expr.brace(),
           "Empty initialization of '" + typeSpelling(arrayType) +
               "' requires a default-initializable element type.",
           "GTI-S2015");
  } else if (expr.elements().empty() && arrayType.arrayLengthParameterId == 0 &&
             arrayType.arrayLength != 0 &&
             fixedArrayClassLeafType(elementType).kind == SemanticType::Class) {
    // A multidimensional array element is itself a fixed array; the
    // construction that must be resolved is the leaf class element's.
    if (std::optional<ResolvedConstructionInfo> construction =
            resolveDefaultConstruction(fixedArrayClassLeafType(elementType))) {
      recordDefaultArgumentDependencies(construction->defaultArguments);
      semanticModel.recordArrayElementConstruction(expr,
                                                   std::move(*construction));
    } else {
      report(expr.brace(),
             "Empty initialization of '" + typeSpelling(arrayType) +
                 "' has no exact accessible default element constructor.",
             "GTI-S2015");
    }
  }

  for (const ExprPtr &element : expr.elements()) {
    const SemanticType valueType = analyzeInitializer(element, elementType);
    if ((elementType.kind == SemanticType::StringView ||
         elementType.kind == SemanticType::CString) &&
        isOwnerDependentTextExpression(element)) {
      reportOwnerDependentTextStorage(element, "nested in a fixed array");
    }
    if (!isAssignable(elementType, valueType, element.get())) {
      report(expressionToken(element),
             "Cannot initialize array element of type '" +
                 typeSpelling(elementType) + "' with a value of type '" +
                 typeSpelling(valueType) + "'.",
             "GTI-S2003");
    }
  }
  currentType = arrayType;
}

auto SemanticVisitor::Impl::visitBinaryExpr(const Binary &expr) -> void {
  const auto [leftType, rightType] = analyzeBinaryOperands(expr);
  validateSequencedTransientLoans(expr.left(), expr.right(), expr.oper());

  if ((expr.oper().kind == TokenKind::PLUS ||
       expr.oper().kind == TokenKind::MINUS) &&
      (leftType.kind == SemanticType::RawPointer ||
       rightType.kind == SemanticType::RawPointer)) {
    if ((leftType.kind == SemanticType::RawPointer &&
         rejectCOpaquePointeeOperation(leftType, expr.oper(),
                                       "raw-pointer arithmetic")) ||
        (rightType.kind == SemanticType::RawPointer &&
         rejectCOpaquePointeeOperation(rightType, expr.oper(),
                                       "raw-pointer arithmetic"))) {
      currentType = SemanticType::Unknown;
      return;
    }
    const auto validPointer = [&](const SemanticType &type) {
      return type.kind == SemanticType::RawPointer &&
             type.arguments.size() == 1 &&
             type.arguments.front() != SemanticType::Void;
    };
    if (expr.oper().kind == TokenKind::PLUS) {
      if (validPointer(leftType) && isInteger(rightType)) {
        requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                      "Raw pointer arithmetic");
        currentType = leftType;
        return;
      }
      if (isInteger(leftType) && validPointer(rightType)) {
        requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                      "Raw pointer arithmetic");
        currentType = rightType;
        return;
      }
    } else {
      if (validPointer(leftType) && isInteger(rightType)) {
        requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                      "Raw pointer arithmetic");
        currentType = leftType;
        return;
      }
      if (validPointer(leftType) && leftType == rightType) {
        requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                      "Raw pointer arithmetic");
        currentType = SemanticType::Int64;
        return;
      }
    }
    report(expr.oper(),
           "Raw pointer arithmetic requires a non-void pointer and an "
           "integer offset, or two identical pointers for subtraction.",
           "GTI-S2056");
    currentType = SemanticType::Unknown;
    return;
  }

  const std::optional<OverloadedOperator> sourceOperator =
      binaryOperator(expr.oper().kind);
  const bool constrainedOperator =
      sourceOperator && leftType.kind == SemanticType::TypeParameter &&
      hasSymbolicOperatorRequirement(
          *sourceOperator, leftType,
          std::span<const SemanticType>(&rightType, 1));
  if ((leftType.kind == SemanticType::Class || constrainedOperator) &&
      sourceOperator) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *sourceOperator, expr.left(), leftType, expr.oper(),
        std::span<const SemanticType>(&rightType, 1),
        std::span<const ExprPtr>(&expr.right(), 1));
    if (selected && *sourceOperator == OverloadedOperator::NotEqual &&
        expr.oper().generated &&
        (selected->parameterTypes.size() != 1 ||
         (selected->parameterTypes.front() != rightType &&
          selected->parameterTypes.front() !=
              SemanticType::referenceTo(rightType, AccessMode::ReadOnly)))) {
      report(expr.oper(),
             "Range iterator operator!= must accept its sentinel by "
             "value or read-only reference.",
             "GTI-S2022");
    }
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }

  switch (expr.oper().kind) {
  case TokenKind::COMMA:
    currentType = rightType;
    return;
  case TokenKind::EQUAL_EQUAL:
  case TokenKind::BANG_EQUAL:
    if (!isComparable(leftType, rightType, expr.left().get(),
                      expr.right().get())) {
      report(expr.oper(), "Equality operands have incompatible types.");
    }
    currentType = SemanticType::Bool;
    return;
  case TokenKind::GREATER:
  case TokenKind::GREATER_EQUAL:
  case TokenKind::LESS:
  case TokenKind::LESS_EQUAL:
    requireOrdered(leftType, rightType, expr.oper());
    if (isInteger(leftType) && isInteger(rightType) &&
        numericResult(leftType, rightType, expr.left().get(),
                      expr.right().get()) == SemanticType::Unknown) {
      report(expr.oper(),
             "Signed and unsigned operands have no safe common type.");
    }
    currentType = SemanticType::Bool;
    return;
  case TokenKind::PLUS:
  case TokenKind::MINUS:
  case TokenKind::STAR:
  case TokenKind::SLASH:
    requireNumeric(leftType, rightType, expr.oper());
    currentType = numericResult(leftType, rightType, expr.left().get(),
                                expr.right().get());
    if (isInteger(leftType) && isInteger(rightType) &&
        currentType == SemanticType::Unknown) {
      report(expr.oper(),
             "Signed and unsigned operands have no safe common type.");
    }
    if (expr.oper().kind == TokenKind::SLASH && isIntegral(leftType) &&
        isIntegral(rightType)) {
      validateIntegerDivisor(expr.oper(), expr.right().get());
    }
    return;
  case TokenKind::PERCENT:
  case TokenKind::AMPERSAND:
  case TokenKind::CARET:
  case TokenKind::PIPE:
    requireInteger(leftType, rightType, expr.oper());
    if (!isIntegral(leftType) || !isIntegral(rightType)) {
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = numericResult(leftType, rightType, expr.left().get(),
                                expr.right().get());
    if (isInteger(leftType) && isInteger(rightType) &&
        currentType == SemanticType::Unknown) {
      report(expr.oper(),
             "Signed and unsigned operands have no safe common type.");
    }
    if (expr.oper().kind == TokenKind::PERCENT) {
      validateIntegerDivisor(expr.oper(), expr.right().get());
    }
    return;
  case TokenKind::SHIFT_LEFT:
  case TokenKind::SHIFT_RIGHT:
    requireInteger(leftType, rightType, expr.oper());
    if (!isIntegral(leftType) || !isIntegral(rightType)) {
      currentType = SemanticType::Unknown;
      return;
    }
    currentType = promotedInteger(leftType);
    validateShiftCount(currentType, expr.right().get(), expr.oper());
    return;
  default:
    currentType = SemanticType::Unknown;
  }
}

auto SemanticVisitor::Impl::visitCallExpr(const Call &expr) -> void {
  const bool immediateReceiver = immediateCallReceiver == &expr;
  struct RestoreImmediateReceiver {
    const Call *&slot;
    const Call *previous;
    ~RestoreImmediateReceiver() { slot = previous; }
  } restoreImmediate{immediateCallReceiver, immediateCallReceiver};
  immediateCallReceiver = directCall(expr.callee());
  // In factory(callback)(args), only each call's own callee is a callee-name
  // context. Its arguments remain values, including exact function items.
  struct RestoreCalleeContext {
    bool &flag;
    bool previous;
    ~RestoreCalleeContext() { flag = previous; }
  } restoreCalleeContext{analyzingCallCallee, analyzingCallCallee};
  analyzingCallCallee = false;
  if (!constexprForFrames.empty() &&
      constexprForFrames.back().initializer == &expr) {
    // The generated `constexpr for` element initializer: default-construct
    // the current element in place of the synthesized callee.
    analyzeTypeParameterCall(expr, constexprForFrames.back().element);
    return;
  }
  if (designatedMoves.contains(&expr)) {
    analyzeMoveCall(expr, nullptr, immediateReceiver);
    return;
  }
  if (const auto enumerator = payloadEnumeratorForCallee(expr.callee())) {
    analyzePayloadConstruction(expr, *enumerator);
    return;
  }
  if (const std::optional<CallableCandidate> intrinsic =
          intrinsicCandidate(expr.callee())) {
    if (intrinsic->record().intrinsic == IntrinsicKind::Move) {
      analyzeMoveCall(expr, &*intrinsic, immediateReceiver);
    } else {
      analyzeIntrinsicCall(expr, *intrinsic);
    }
    return;
  }
  if (const auto *callee = exprAs<Variable>(expr.callee().get())) {
    if (const std::optional<SemanticType> target =
            resolveTypeParameter(NamePath(callee->name()))) {
      analyzeTypeParameterCall(expr, *target);
      return;
    }
  }
  if (const std::optional<ResolvedTypeAlias> alias =
          typeAliasForCallee(expr.callee())) {
    const RegisteredTypeAlias &declaration = typeAliases[alias->id - 1];
    if (declaration.resolution != TypeAliasResolution::Resolved) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      currentType = SemanticType::Unknown;
      return;
    }
    if (!validateTypeAliasAccess(*alias, expressionToken(expr.callee()))) {
      for (const ExprPtr &argument : expr.arguments()) {
        analyze(argument);
      }
      currentType = SemanticType::Unknown;
      return;
    }
    if (const auto *variable = exprAs<Variable>(expr.callee().get()))
      recordTypeUse(TypeRef(NamePath(variable->name())));
    else if (const auto *qualified = exprAs<QualifiedName>(expr.callee().get()))
      recordTypeUse(TypeRef(qualified->name()));
    if (alias->type.kind == SemanticType::TypeParameter) {
      analyzeTypeParameterCall(expr, alias->type);
      return;
    }
    if (!expr.typeArguments().empty()) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "Type alias '" + alias->qualifiedName +
                 "' does not take generic arguments.",
             "GTI-S2030");
    }
    if (isNumeric(alias->type) ||
        alias->type.kind == SemanticType::RawPointer) {
      analyzeAliasConversion(expr, alias->type, alias->qualifiedName);
      return;
    }

    ContextualCallArguments argumentAnalysis =
        analyzeContextualCallArguments(expr.arguments());
    std::vector<SemanticType> &argumentTypes = argumentAnalysis.types;
    if (alias->type.kind == SemanticType::Class) {
      analyzeConstructorCall(expr, alias->type.classId, alias->type.arguments,
                             alias->type.valueArguments, argumentTypes,
                             expr.arguments(), expr.paren());
    } else {
      report(expressionToken(expr.callee()),
             "Type alias '" + alias->qualifiedName +
                 "' does not name a constructible class, numeric or raw "
                 "pointer type.",
             "GTI-S2030");
      currentType = SemanticType::Unknown;
    }
    return;
  }
  const bool enclosingCallCallee = analyzingCallCallee;
  analyzingCallCallee = true;
  const SemanticType calleeType = analyze(expr.callee());
  analyzingCallCallee = enclosingCallCallee;

  ContextualCallArguments argumentAnalysis =
      analyzeContextualCallArguments(expr.arguments());
  std::vector<SemanticType> &argumentTypes = argumentAnalysis.types;

  if (const auto *member = exprAs<Get>(expr.callee().get())) {
    const SemanticType *objectType = semanticModel.findType(*member->object());
    if (objectType != nullptr && objectType->kind == SemanticType::Class &&
        objectType->classId != 0 && objectType->classId <= classes.size()) {
      const CompilerTypeKind metaObject =
          classInfo(objectType->classId).compilerType;
      if (metaObject == CompilerTypeKind::MetaField &&
          (member->name().lexeme == "get" ||
           member->name().lexeme == "get_mut")) {
        analyzeMetaFieldAccess(expr, *member, *objectType,
                               member->name().lexeme == "get_mut");
        return;
      }
      if ((metaObject == CompilerTypeKind::MetaField ||
           metaObject == CompilerTypeKind::MetaType) &&
          member->name().lexeme == "attribute") {
        analyzeMetaAttributeAccess(expr, *member, *objectType,
                                   metaObject == CompilerTypeKind::MetaType);
        return;
      }
      if (metaObject == CompilerTypeKind::MetaField &&
          member->name().lexeme == "default_value") {
        analyzeMetaFieldDefault(expr, *member, *objectType);
        return;
      }
    }
  }

  if (calleeType.kind == SemanticType::NativeFunction) {
    if (argumentAnalysis.hasDeferred()) {
      const auto parameters = calleeType.nativeFunctionParameterTypes();
      if (!finishContextualCallArguments(
              std::vector<SemanticType>(parameters.begin(), parameters.end()),
              expr.arguments(), argumentAnalysis, expr.paren(),
              "Native function pointer")) {
        currentType = SemanticType::Unknown;
        return;
      }
    }
    analyzeNativeIndirectCall(expr, calleeType, argumentTypes);
    return;
  }

  if (calleeType.kind == SemanticType::Lambda) {
    if (argumentAnalysis.hasDeferred()) {
      std::vector<SemanticType> parameters(
          calleeType.lambdaParameterTypes().begin(),
          calleeType.lambdaParameterTypes().end());
      if (!finishContextualCallArguments(parameters, expr.arguments(),
                                         argumentAnalysis, expr.paren(),
                                         "Lambda")) {
        currentType = SemanticType::Unknown;
        return;
      }
    }
    analyzeLambdaCall(expr, calleeType, argumentTypes);
    return;
  }

  if (calleeType.kind == SemanticType::FunctionItem) {
    if (argumentAnalysis.hasDeferred()) {
      std::vector<SemanticType> parameters(
          calleeType.functionItemParameterTypes().begin(),
          calleeType.functionItemParameterTypes().end());
      if (!finishContextualCallArguments(parameters, expr.arguments(),
                                         argumentAnalysis, expr.paren(),
                                         "Function item")) {
        currentType = SemanticType::Unknown;
        return;
      }
    }
    analyzeFunctionItemCall(expr, calleeType, argumentTypes);
    return;
  }

  SemanticType deferredCalleeType = calleeType;
  if (deferredCalleeType.kind == SemanticType::Reference &&
      deferredCalleeType.arguments.size() == 1) {
    SemanticType referent = deferredCalleeType.arguments.front();
    deferredCalleeType = std::move(referent);
  }
  if (deferredCalleeType.kind == SemanticType::TypeParameter) {
    if (argumentAnalysis.hasDeferred()) {
      for (std::size_t index = 0; index < expr.arguments().size(); ++index) {
        if (!argumentAnalysis.deferred[index])
          continue;
        const ExprPtr &argument = expr.arguments()[index];
        if (isContextualArrayArgument(argument)) {
          report(expressionToken(argument),
                 "A brace argument requires a callable with a concrete fixed-"
                 "array parameter signature.",
                 "GTI-S2015");
          currentType = SemanticType::Unknown;
          return;
        }
        // The open receiver does not supply an argument type. Record a
        // result-inference obligation for direct nested callable producers,
        // then resolve the complete composition in concrete semantics. Do
        // not guess the inner result from the enclosing return type.
        argumentTypes[index] = isContextualCallableResultArgument(argument)
                                   ? analyzeExpectedCallableResult(
                                         argument, SemanticType::Unknown, true)
                                   : analyze(argument);
      }
    }
    analyzeDeferredCallableCall(expr, deferredCalleeType, argumentTypes);
    return;
  }

  if (calleeType.kind == SemanticType::Class) {
    if (std::any_of(expr.arguments().begin(), expr.arguments().end(),
                    [&](const ExprPtr &argument) {
                      return isContextualArrayArgument(argument) ||
                             isContextualNativeFunctionArgument(argument);
                    })) {
      report(expr.paren(),
             "Contextual brace arguments are not yet available through "
             "operator(); use a named function or constructor with an "
             "exact fixed-array parameter.",
             "GTI-S2015");
      currentType = SemanticType::Unknown;
      return;
    }
    if (!expr.typeArguments().empty()) {
      for (const TypeRef &argument : expr.typeArguments()) {
        validateType(argument);
      }
      report(expr.paren(),
             "operator() calls do not take explicit type arguments.",
             "GTI-S2022");
    }
    const CallableParameterContract *contract =
        callableParameterContract(expr.callee());
    const std::optional<CallableBoundary> boundary =
        contract == nullptr ? std::nullopt : std::optional{contract->boundary};
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, OverloadedOperator::Call, expr.callee(), calleeType, expr.paren(),
        argumentTypes, expr.arguments(), false, boundary, std::nullopt, false,
        false, &argumentAnalysis);
    const bool valid =
        selected && validateCallableReturn(expr, selected->returnType) &&
        validateCallableCapability(
            expr, callableInvocationCapability(selected->receiverMutability));
    currentType = valid ? callExpressionType(selected->returnType)
                        : SemanticType::Unknown;
    return;
  }

  if (calleeType.kind == SemanticType::TypeName) {
    const ResolvedClassArguments genericArguments = resolveClassArguments(
        calleeType.classId, expr.typeArguments(), expr.paren());
    // A computed compiler type (`std::meta::field_type<T, I>`) may denote a
    // scalar once its arguments are concrete; the call is then the explicit
    // conversion that scalar's own name would spell.
    const SemanticType constructed = selectExactClassSpecialization(
        SemanticType::classType(calleeType.classId, genericArguments.types,
                                genericArguments.values));
    if (constructed.kind == SemanticType::Class && constructed.classId != 0 &&
        constructed.classId <= classes.size() &&
        containsOpenTypeParameter(constructed)) {
      // A computed type whose arguments are still open denotes nothing yet;
      // concrete reanalysis resolves the call once it does.
      switch (classInfo(constructed.classId).compilerType) {
      case CompilerTypeKind::MetaFieldType:
      case CompilerTypeKind::MetaPayloadType:
      case CompilerTypeKind::MetaParameterType:
      case CompilerTypeKind::MetaConstructorParameterType:
      case CompilerTypeKind::MetaReturnType:
      case CompilerTypeKind::MetaListFirst:
      case CompilerTypeKind::MetaListRest:
        currentType = constructed;
        return;
      default:
        break;
      }
    }
    if (constructed != SemanticType::Unknown &&
        constructed.kind != SemanticType::Class &&
        !containsOpenTypeParameter(constructed)) {
      const std::optional<Token> target =
          builtinTypeToken(constructed, expr.paren());
      if (!target || expr.arguments().size() != 1) {
        report(expr.paren(),
               "'" + typeSpelling(constructed) +
                   "' is not a class type; a call constructs a class or "
                   "converts one value to a numeric type.",
               "GTI-S2014");
        currentType = SemanticType::Unknown;
        return;
      }
      auto conversion = std::make_shared<Conversion>(
          TypeRef(*target), expr.paren(), cloneExpr(*expr.arguments().front()));
      synthesizedExpressions.push_back(conversion);
      const Expr &alias = semanticModel.recordExpressionAlias(expr, conversion);
      currentType = analyze(alias);
      return;
    }
    // Constructor value parameters infer unambiguous function items as direct
    // braces do. Resume the deferred value tail exactly once, in source order.
    // Array braces still need the selected constructor's contextual type.
    bool contextualArrayTail = false;
    for (std::size_t index = 0; index < expr.arguments().size(); ++index) {
      contextualArrayTail =
          contextualArrayTail ||
          isContextualArrayArgument(expr.arguments()[index]) ||
          isContextualCallableResultArgument(expr.arguments()[index]);
      if (!contextualArrayTail && argumentAnalysis.deferred[index])
        argumentTypes[index] = analyze(expr.arguments()[index]);
    }
    analyzeConstructorCall(expr, calleeType.classId, genericArguments.types,
                           genericArguments.values, argumentTypes,
                           expr.arguments(), expr.paren());
    return;
  }

  const std::optional<Symbol> callee = resolveExpressionSymbol(expr.callee());
  const std::size_t genericDiagnosticStart = diagnostics.size();
  std::vector<SemanticType> explicitTypeArguments;
  explicitTypeArguments.reserve(expr.typeArguments().size());
  std::vector<CompileTimeValue> explicitValueArguments;
  explicitValueArguments.reserve(expr.typeArguments().size());
  bool sawExplicitValueArgument = false;
  std::size_t explicitArgumentIndex = 0;
  for (const TypeRef &argument : expr.typeArguments()) {
    // The target's parameter kind is authoritative. In particular a runtime
    // identifier in a value slot is a failed constant, not an unknown type.
    const bool targetRequiresValue =
        callee && !callee->overloads.empty() &&
        std::all_of(callee->overloads.begin(), callee->overloads.end(),
                    [&](const CallableCandidate &candidate) {
                      const auto &parameters =
                          candidate.record().genericParameters;
                      return explicitArgumentIndex < parameters.size() &&
                             parameters[explicitArgumentIndex].value;
                    });
    ++explicitArgumentIndex;
    std::optional<CompileTimeValue> value;
    if (targetRequiresValue ||
        argument.genericArgumentSyntax == GenericArgumentSyntax::Value ||
        (argument.genericArgumentSyntax ==
             GenericArgumentSyntax::UnresolvedIdentifier &&
         (value = genericValueArgument(argument)).has_value())) {
      sawExplicitValueArgument = true;
      if (!value) {
        value = genericValueArgument(argument);
      }
      if (!value) {
        if (!reportGenericValueArgumentFailure(
                argument, "Function value generic argument")) {
          report(argument.name.last(),
                 "A function value generic argument must be an in-scope "
                 "uint64_t compile-time value.",
                 "GTI-S2026");
        }
        explicitValueArguments.emplace_back();
      } else {
        explicitValueArguments.emplace_back(*value);
        if (argument.genericValueExpression) {
          recordArrayExtentUses(*argument.genericValueExpression);
        } else if (argument.name.last().kind == TokenKind::IDENTIFIER) {
          recordGenericParameterUse(argument.name.last(), *value,
                                    OccurrenceRole::Read);
        }
      }
      continue;
    }
    if (sawExplicitValueArgument) {
      report(argument.name.last(),
             "Explicit generic type arguments must appear before value "
             "arguments.",
             "GTI-S2026");
    }
    validateType(argument);
    const SemanticType argumentType = typeOf(argument);
    explicitTypeArguments.emplace_back(argumentType);
  }
  const bool hasExplicitGenericArguments =
      !explicitTypeArguments.empty() || !explicitValueArguments.empty();
  if (hasErrorDiagnostics(
          std::span(diagnostics).subspan(genericDiagnosticStart))) {
    currentType = SemanticType::Unknown;
    return;
  }

  if (const auto *member = exprAs<Get>(expr.callee().get())) {
    const SemanticType *objectType = semanticModel.findType(*member->object());
    if (objectType && containsOpenTypeParameter(*objectType)) {
      if (const auto result = symbolicReadMemberReturnType(
              *objectType, member->name().lexeme)) {
        std::optional<ExpandedStructuralRequirement> selected;
        bool ambiguous = false;
        for (const auto &requirement : activeStructuralRequirements()) {
          if (requirement.kind != StructuralConstraintKind::ReadMember ||
              requirement.memberName != member->name().lexeme ||
              requirement.arguments.size() != argumentTypes.size() + 2 ||
              requirement.arguments[0] != *objectType ||
              (requirement.mutableReceiver &&
               !isMutableObject(member->object())))
            continue;
          bool matches = true;
          for (std::size_t index = 0; index < argumentTypes.size(); ++index)
            matches &= callArgumentMatches(requirement.arguments[index + 2],
                                           argumentTypes[index],
                                           expr.arguments()[index]);
          if (!matches)
            continue;
          if (selected && selected->mutableReceiver &&
              !requirement.mutableReceiver)
            continue;
          if (selected && !selected->mutableReceiver &&
              requirement.mutableReceiver)
            selected.reset();
          if (selected && selected->arguments != requirement.arguments)
            ambiguous = true;
          if (selected && selected->arguments == requirement.arguments &&
              !selected->requiresUnsafe && requirement.requiresUnsafe)
            continue;
          selected = requirement;
        }
        if (hasExplicitGenericArguments || !selected || ambiguous) {
          report(expr.paren(),
                 "Call does not match the required member signature or "
                 "receiver access.",
                 "GTI-S2029");
          currentType = SemanticType::Unknown;
          return;
        }
        if (selected->requiresUnsafe) {
          requireUnsafe(expr, UnsafeOperationKind::DeclaredUnsafeCall,
                        expr.paren(), "Calling an unsafe required member");
        }
        semanticModel.record(
            expr, ResolvedCallInfo{.returnType = selected->arguments[1],
                                   .receiverMode =
                                       selected->mutableReceiver
                                           ? CallReceiverMode::MutablePlace
                                           : CallReceiverMode::ReadOnlyPlace});
        currentType = selected->arguments[1];
        return;
      }
    }
    if (objectType != nullptr &&
        objectType->kind == SemanticType::TypeParameter) {
      const bool generatedRangeEndpoint =
          member->name().generated && (member->name().lexeme == "begin" ||
                                       member->name().lexeme == "begin_mut" ||
                                       member->name().lexeme == "end");
      const std::optional<SemanticType> result =
          symbolicRangeMemberReturnType(*objectType, member->name().lexeme);
      if (result &&
          (member->name().lexeme == "size" || generatedRangeEndpoint)) {
        if (hasExplicitGenericArguments) {
          report(expr.paren(),
                 "Structural range members do not take generic arguments.",
                 "GTI-S2029");
        }
        if (!argumentTypes.empty()) {
          report(expr.paren(), "Structural range members take no arguments.",
                 "GTI-S2029");
        }
        const bool borrowedEndpoint = member->name().lexeme == "begin" ||
                                      member->name().lexeme == "begin_mut";
        const bool mutableEndpoint = member->name().lexeme == "begin_mut";
        semanticModel.record(
            expr,
            ResolvedCallInfo{
                .returnType = *result,
                .borrowOrigin = borrowedEndpoint ? BorrowOriginKind::Receiver
                                                 : BorrowOriginKind::None,
                .borrowAccess = mutableEndpoint ? AccessMode::Mutable
                                                : AccessMode::ReadOnly,
                .receiverMode = mutableEndpoint
                                    ? CallReceiverMode::MutablePlace
                                    : CallReceiverMode::ReadOnlyPlace});
        currentType = *result;
        return;
      }
    }
    if (objectType != nullptr && objectType->kind == SemanticType::Expected) {
      if (hasExplicitGenericArguments) {
        report(expr.paren(),
               "Expected member functions do not take generic arguments.");
      }
      reportLoanPlaceConflict(
          expr.paren(), semanticPlace(member->object()), AccessMode::ReadOnly,
          "Expected member receiver overlaps a mutable borrow or child "
          "reborrow that may still be live.");
      analyzeExpectedMemberCall(expr, *member, *objectType, argumentTypes,
                                expr.paren());
      return;
    }
    if (objectType != nullptr && objectType->kind == SemanticType::Array) {
      if (hasExplicitGenericArguments) {
        report(expr.paren(),
               "Fixed array member functions do not take generic arguments.");
      }
      reportLoanPlaceConflict(
          expr.paren(), semanticPlace(member->object()), AccessMode::ReadOnly,
          "Fixed-array member receiver overlaps a mutable borrow or child "
          "reborrow that may still be live.");
      analyzeArrayMemberCall(expr, *member, argumentTypes, expr.paren());
      return;
    }
    if (objectType != nullptr && objectType->kind == SemanticType::StringView) {
      if (hasExplicitGenericArguments) {
        report(expr.paren(),
               "String-view member functions do not take generic arguments.",
               "GTI-S2035");
      }
      reportLoanPlaceConflict(
          expr.paren(), semanticPlace(member->object()), AccessMode::ReadOnly,
          "String-view member receiver overlaps a mutable borrow or child "
          "reborrow that may still be live.");
      analyzeStringViewMemberCall(expr, *member, argumentTypes, expr.paren());
      return;
    }
  }

  if (calleeType != SemanticType::Unknown &&
      calleeType != SemanticType::Function) {
    report(expr.paren(), "Can only call functions.");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!callee || callee->type != SemanticType::Function ||
      callee->overloads.empty()) {
    currentType = SemanticType::Unknown;
    return;
  }

  if (hasPackExpansion(argumentTypes) &&
      std::none_of(callee->overloads.begin(), callee->overloads.end(),
                   [](const CallableCandidate &candidate) {
                     return candidate.record().parameterPack;
                   })) {
    report(expressionToken(expr.arguments().back()),
           "A parameter pack can only be forwarded to another variadic "
           "function or method.",
           "GTI-S2023");
    currentType = SemanticType::Unknown;
    return;
  }

  if (callee->overloads.size() == 1) {
    const CallableCandidate &candidate = callee->overloads.front();
    CallableCandidate resolved = candidate;
    const std::size_t contextualDiagnostics = diagnostics.size();
    bool valid = contextualizeFunctionItemArguments(
        candidate, explicitTypeArguments, explicitValueArguments,
        expr.arguments(), argumentAnalysis, true);
    if (!valid && diagnostics.size() != contextualDiagnostics) {
      // Context selection already owns this failure. A function item's type
      // has no source spelling, so a subsequent request to write F explicitly
      // is neither an independent error nor an actionable repair.
      currentType = SemanticType::Unknown;
      return;
    }
    valid = applyFunctionTypeArguments(resolved, explicitTypeArguments,
                                       explicitValueArguments, argumentTypes,
                                       expr.arguments(), expr.paren()) &&
            valid;
    std::vector<SemanticType> resolvedTypeArguments;
    std::vector<CompileTimeValue> resolvedValueArguments;
    CallableCandidate trial;
    ConstraintFailure constraintFailure;
    if (tryInstantiateFunction(candidate, explicitTypeArguments,
                               explicitValueArguments, argumentTypes,
                               expr.arguments(), trial, resolvedTypeArguments,
                               resolvedValueArguments, &constraintFailure)) {
      resolved = std::move(trial);
    } else if (hasConstraintFailure(constraintFailure)) {
      if (valid) {
        reportConstraintFailure(expr.paren(), constraintFailure);
      }
      valid = false;
    }

    const bool instantiatedPackForwardingShape =
        candidate.record().parameterPack && !resolved.parameterPack &&
        hasPackExpansion(argumentTypes) &&
        argumentTypes.size() == resolved.parameterTypes.size();
    if (resolved.parameterPack) {
      valid = false;
    } else if (!instantiatedPackForwardingShape &&
               !acceptsArgumentShape(resolved, argumentTypes)) {
      const bool exactArity =
          resolved.requiredParameterCount == resolved.parameterTypes.size();
      report(expr.paren(),
             exactArity ? "Function expects " +
                              std::to_string(resolved.parameterTypes.size()) +
                              " argument" +
                              (resolved.parameterTypes.size() == 1 ? "" : "s") +
                              " but received " +
                              std::to_string(argumentTypes.size()) + "."
                        : "Function expects between " +
                              std::to_string(resolved.requiredParameterCount) +
                              " and " +
                              std::to_string(resolved.parameterTypes.size()) +
                              " arguments but received " +
                              std::to_string(argumentTypes.size()) + ".",
             "GTI-S2005");
      valid = false;
    } else {
      std::vector<std::size_t> ephemeralBorrowArguments;
      for (std::size_t index = 0; index < argumentTypes.size(); ++index) {
        const CallArgumentMatchKind match = functionCallArgumentMatch(
            resolved, index, argumentTypes[index], expr.arguments()[index],
            argumentAnalysis.deferred[index]);
        if (match == CallArgumentMatchKind::EphemeralReadBorrow) {
          ephemeralBorrowArguments.push_back(index);
        } else if (match == CallArgumentMatchKind::Mismatch) {
          if (isContextualArrayArgument(expr.arguments()[index])) {
            const auto *initializer =
                exprAs<ArrayInitializer>(expr.arguments()[index].get());
            if (resolved.parameterTypes[index].kind != SemanticType::Array) {
              report(expressionToken(expr.arguments()[index]),
                     "A brace argument requires one exact fixed-array "
                     "parameter type from the selected function.",
                     "GTI-S2015");
            } else {
              report(expressionToken(expr.arguments()[index]),
                     "Brace argument provides " +
                         std::to_string(initializer == nullptr
                                            ? 0
                                            : initializer->elements().size()) +
                         " elements but parameter type '" +
                         typeSpelling(resolved.parameterTypes[index]) +
                         "' requires exactly " +
                         std::to_string(
                             resolved.parameterTypes[index].arrayLength) +
                         ".",
                     "GTI-S2015");
            }
          } else {
            reportCallArgumentMismatch(
                index, resolved.parameterTypes[index], argumentTypes[index],
                expr.arguments()[index], "Function", &resolved);
          }
          valid = false;
        }
      }
      if (valid && argumentAnalysis.hasDeferred()) {
        valid = finishContextualCallArguments(
                    resolved.parameterTypes, expr.arguments(), argumentAnalysis,
                    expr.paren(), "Function") &&
                valid;
      }

      const CallReceiverMode receiverMode =
          callReceiverMode(expr.callee(), resolved);
      valid = validateSelectedFunction(resolved, expr.callee(), expr.paren(),
                                       expr.arguments(), receiverMode) &&
              valid;
      valid = validateConfinedCallableArguments(
                  candidate, resolved, argumentTypes, expr.arguments()) &&
              valid;
      if (valid) {
        recordResolvedCall(
            expr, resolved, resolvedTypeArguments, resolvedValueArguments,
            std::move(ephemeralBorrowArguments),
            lambdaArgumentBoundaries(argumentTypes), receiverMode);
      }
      currentType = valid ? callExpressionType(resolved.returnType)
                          : SemanticType::Unknown;
      return;
    }

    currentType = SemanticType::Unknown;
    return;
  }

  struct ViableOverload {
    const CallableCandidate *source = nullptr;
    CallableCandidate function;
    std::vector<SemanticType> typeArguments;
    std::vector<CompileTimeValue> valueArguments;
    std::vector<std::size_t> ephemeralBorrowArguments;
    ContextualCallArguments argumentAnalysis;
    CallReceiverMode receiverMode = CallReceiverMode::None;
  };
  std::vector<ViableOverload> viable;
  std::vector<ConstraintFailure> constraintFailures;
  bool rejectedMutableReceiver = false;
  bool rejectedConsumingReceiver = false;
  std::optional<CallableCandidate> rejectedMutableFunction;
  bool rejectedCompilerPrivate = false;
  for (const CallableCandidate &candidate : callee->overloads) {
    ContextualCallArguments candidateArguments = argumentAnalysis;
    if (!acceptsArgumentShape(candidate, candidateArguments.types) ||
        !contextualizeFunctionItemArguments(
            candidate, explicitTypeArguments, explicitValueArguments,
            expr.arguments(), candidateArguments, false)) {
      continue;
    }
    CallableCandidate resolved;
    std::vector<SemanticType> resolvedTypeArguments;
    std::vector<CompileTimeValue> resolvedValueArguments;
    std::vector<std::size_t> resolvedEphemeralBorrowArguments;
    ConstraintFailure constraintFailure;
    if (!tryInstantiateFunction(candidate, explicitTypeArguments,
                                explicitValueArguments,
                                candidateArguments.types, expr.arguments(),
                                resolved, resolvedTypeArguments,
                                resolvedValueArguments, &constraintFailure)) {
      if (hasConstraintFailure(constraintFailure)) {
        constraintFailures.emplace_back(std::move(constraintFailure));
      }
      continue;
    }
    bool exact = true;
    for (std::size_t index = 0; index < candidateArguments.types.size();
         ++index) {
      const CallArgumentMatchKind match = functionCallArgumentMatch(
          resolved, index, candidateArguments.types[index],
          expr.arguments()[index], candidateArguments.deferred[index]);
      if (match == CallArgumentMatchKind::Mismatch) {
        exact = false;
        break;
      }
      if (match == CallArgumentMatchKind::EphemeralReadBorrow) {
        resolvedEphemeralBorrowArguments.push_back(index);
      }
    }
    if (!exact) {
      continue;
    }
    const CallReceiverMode receiverMode =
        callReceiverMode(expr.callee(), resolved);
    if (resolved.receiverMutability == ReceiverMutability::Consuming &&
        receiverMode != CallReceiverMode::Consuming) {
      rejectedConsumingReceiver = true;
      continue;
    }
    if (resolved.ownerClass != 0 && !resolved.staticMember &&
        resolved.receiverMutability == ReceiverMutability::Mutable &&
        !receiverModeAllowsMutation(receiverMode)) {
      rejectedMutableReceiver = true;
      if (!rejectedMutableFunction) {
        rejectedMutableFunction = resolved;
      }
      continue;
    }
    if (!isCompilerTrustedRequester(expr.paren()) && resolved.compilerPrivate) {
      rejectedCompilerPrivate = true;
      continue;
    }
    viable.push_back({&candidate, std::move(resolved),
                      std::move(resolvedTypeArguments),
                      std::move(resolvedValueArguments),
                      std::move(resolvedEphemeralBorrowArguments),
                      std::move(candidateArguments), receiverMode});
  }

  if (std::any_of(viable.begin(), viable.end(),
                  [](const ViableOverload &candidate) {
                    return candidate.ephemeralBorrowArguments.empty();
                  })) {
    std::erase_if(viable, [](const ViableOverload &candidate) {
      return !candidate.ephemeralBorrowArguments.empty();
    });
  }

  if (std::any_of(viable.begin(), viable.end(),
                  [](const ViableOverload &candidate) {
                    return candidate.function.receiverMutability ==
                           ReceiverMutability::Consuming;
                  })) {
    std::erase_if(viable, [](const ViableOverload &candidate) {
      return candidate.function.receiverMutability !=
             ReceiverMutability::Consuming;
    });
  }

  if (std::any_of(viable.begin(), viable.end(),
                  [](const ViableOverload &candidate) {
                    return candidate.function.ownerClass != 0 &&
                           !candidate.function.staticMember &&
                           candidate.function.receiverMutability ==
                               ReceiverMutability::Mutable &&
                           receiverModeAllowsMutation(candidate.receiverMode);
                  })) {
    std::erase_if(viable, [](const ViableOverload &candidate) {
      return candidate.function.ownerClass != 0 &&
             !candidate.function.staticMember &&
             candidate.function.receiverMutability ==
                 ReceiverMutability::ReadOnly;
    });
  }

  std::vector<SemanticType> rankingArgumentTypes =
      viable.empty() ? argumentTypes : viable.front().argumentAnalysis.types;
  for (std::size_t index = 0; index < rankingArgumentTypes.size(); ++index) {
    if (std::any_of(viable.begin(), viable.end(),
                    [&](const ViableOverload &row) {
                      return index < row.argumentAnalysis.types.size() &&
                             row.argumentAnalysis.types[index].kind ==
                                 SemanticType::FunctionItem;
                    })) {
      // Contextual function-item selection is a viability decision, never an
      // overload-ranking conversion. Other written arguments may still rank
      // the enclosing call normally.
      rankingArgumentTypes[index] = SemanticType::Unknown;
    }
  }
  retainBestCallConversionMatches(
      viable, rankingArgumentTypes,
      [](const ViableOverload &candidate) -> const std::vector<SemanticType> & {
        return candidate.function.parameterTypes;
      });

  const bool hasUnknownArgument = std::any_of(
      argumentTypes.begin(), argumentTypes.end(),
      [](const SemanticType &type) { return type == SemanticType::Unknown; });
  if (viable.size() != 1) {
    if (!hasUnknownArgument || argumentAnalysis.hasDeferred()) {
      if (viable.empty() && rejectedCompilerPrivate) {
        reportCompilerPrivateAccess(expr.paren(), "function overload");
        currentType = SemanticType::Unknown;
        return;
      }
      if (viable.empty() && rejectedConsumingReceiver) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2022", DiagnosticPhase::Semantics, expr.paren(),
            "A consuming method requires an owned temporary or an explicitly "
            "moved receiver.");
        diagnostic.hints.emplace_back("Use std::move(value).method(...) to "
                                      "transfer ownership of a named value.");
        diagnostics.emplace_back(std::move(diagnostic));
        currentType = SemanticType::Unknown;
        return;
      }
      if (viable.empty() && rejectedMutableReceiver) {
        const auto *member = exprAs<Get>(expr.callee().get());
        if (member != nullptr && rejectedMutableFunction) {
          reportMutableReceiverFailure(member->object(), expr.paren(),
                                       *rejectedMutableFunction,
                                       "Mutable method");
        } else {
          report(expr.paren(), "Mutable method requires a mutable receiver.");
        }
        currentType = SemanticType::Unknown;
        return;
      }
      if (viable.empty() && constraintFailures.size() == 1) {
        reportConstraintFailure(expr.paren(), constraintFailures.front());
        currentType = SemanticType::Unknown;
        return;
      }
      std::vector<const CallableCandidate *> exactMatches;
      exactMatches.reserve(viable.size());
      for (const ViableOverload &match : viable) {
        exactMatches.emplace_back(&match.function);
      }
      reportOverloadResolutionFailure(expr, *callee, argumentTypes,
                                      exactMatches, constraintFailures);
    }
    currentType = SemanticType::Unknown;
    return;
  }

  if (argumentAnalysis.hasDeferred() &&
      !finishContextualCallArguments(
          viable.front().function.parameterTypes, expr.arguments(),
          viable.front().argumentAnalysis, expr.paren(), "Function")) {
    currentType = SemanticType::Unknown;
    return;
  }

  if (!validateSelectedFunction(viable.front().function, expr.callee(),
                                expr.paren(), expr.arguments(),
                                viable.front().receiverMode)) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (!validateConfinedCallableArguments(
          *viable.front().source, viable.front().function,
          viable.front().argumentAnalysis.types, expr.arguments())) {
    currentType = SemanticType::Unknown;
    return;
  }
  recordResolvedCall(
      expr, viable.front().function, viable.front().typeArguments,
      viable.front().valueArguments,
      std::move(viable.front().ephemeralBorrowArguments),
      lambdaArgumentBoundaries(argumentTypes), viable.front().receiverMode);
  currentType = callExpressionType(viable.front().function.returnType);
}

auto SemanticVisitor::Impl::isCharacterCodeConversion(
    const SemanticType &source, const SemanticType &target) -> bool {
  return (source == SemanticType::Char && target == SemanticType::UInt8) ||
         (source == SemanticType::UInt8 && target == SemanticType::Char);
}

auto SemanticVisitor::Impl::analyzeTypeParameterCall(
    const Call &expr, const SemanticType &targetType) -> void {
  bool valid = true;
  if (!expr.typeArguments().empty()) {
    for (const TypeRef &argument : expr.typeArguments()) {
      validateType(argument);
    }
    report(expr.paren(),
           "A generic type-parameter conversion does not take type "
           "arguments.",
           "GTI-S2029");
    valid = false;
  }
  if (!expr.arguments().empty() &&
      exprAs<PackExpansion>(expr.arguments().back().get()) != nullptr) {
    const ContextualCallArguments analysis =
        analyzeContextualCallArguments(expr.arguments());
    const auto arguments =
        concreteCallArguments(analysis.types, expr.arguments());
    // A forwarded pack has no declaration-wide arity or constructor signature.
    // Only concrete semantic reanalysis may select its executable construction;
    // no provisional constructor/call target is published for HIR to execute.
    if (targetType.kind == SemanticType::TypeParameter || !arguments) {
      currentType = valid ? targetType : SemanticType::Unknown;
      return;
    }
    semanticModel.suppressInheritedCall(expr);
    if (!valid) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (targetType.kind == SemanticType::Class) {
      analyzeConstructorCallArguments(
          expr, targetType.classId, targetType.arguments,
          targetType.valueArguments, *arguments, expr.paren());
      return;
    }
    if (arguments->empty() && isDefaultInitializable(targetType, true)) {
      currentType = targetType;
      semanticModel.record(
          expr,
          ResolvedCallInfo{
              .returnType = targetType,
              .parameterTypes = analysis.types,
              .typeArguments = {targetType},
              .intrinsic = IntrinsicKind::DefaultTypeParameterConstruction});
      return;
    }
    const bool scalar =
        isNumeric(targetType) || targetType == SemanticType::Bool ||
        targetType == SemanticType::Char ||
        targetType == SemanticType::NullPtr ||
        targetType.kind == SemanticType::RawPointer ||
        targetType.kind == SemanticType::NativeFunction ||
        (targetType.kind == SemanticType::Enum && targetType.enumId != 0 &&
         targetType.enumId <= enums.size() &&
         !enums[targetType.enumId - 1].payload);
    if (arguments->size() == 1 && arguments->front().type == targetType &&
        scalar) {
      currentType = targetType;
      semanticModel.record(
          expr,
          ResolvedCallInfo{.returnType = targetType,
                           .parameterTypes = analysis.types,
                           .typeArguments = {targetType},
                           .intrinsic =
                               IntrinsicKind::ExactTypeParameterConstruction});
      return;
    }
    report(expr.paren(),
           "Forwarded construction requires a class constructor or one exact "
           "scalar value (or a default-initializable type and an empty pack).",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }
  if (expr.arguments().empty()) {
    if (!satisfiesConstraint(
            targetType,
            constraintBit(GenericConstraintKind::DefaultInitializable))) {
      report(expressionToken(expr.callee()),
             "Generic default construction of '" + typeSpelling(targetType) +
                 "' requires the default-initialization capability.",
             "GTI-S2029");
      valid = false;
    }
    // Generic instance analysis substitutes T before revisiting T(). Once
    // that substitution is a class, resolve the exact zero-argument
    // constructor like any other class construction. Leaving the generic
    // intrinsic in place loses the constructor instance from HIR/MIR and
    // can produce a declaration with no definition after the MIR cutover.
    if (valid && targetType.kind == SemanticType::Class) {
      semanticModel.suppressInheritedCall(expr);
      analyzeConstructorCall(expr, targetType.classId, targetType.arguments,
                             targetType.valueArguments, {}, expr.arguments(),
                             expr.paren());
      return;
    }
    currentType = valid ? targetType : SemanticType::Unknown;
    if (valid) {
      semanticModel.record(
          expr,
          ResolvedCallInfo{
              .returnType = targetType,
              .typeArguments = {targetType},
              .intrinsic = IntrinsicKind::DefaultTypeParameterConstruction});
    }
    return;
  }
  if (expr.arguments().size() != 1) {
    for (const ExprPtr &argument : expr.arguments()) {
      analyze(argument);
    }
    report(expr.paren(),
           "A numeric conversion requires exactly one value argument.",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }

  if (targetType.kind == SemanticType::RawPointer) {
    analyzeAliasConversion(expr, targetType, typeSpelling(targetType));
    if (!valid)
      currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType valueType = analyze(expr.arguments().front());
  if (valid && targetType.kind == SemanticType::TypeParameter &&
      valueType.kind == SemanticType::RawPointer) {
    // Pointer-shaped input permits dependent explicit address policy. The
    // concrete instance must resolve and validate the destination before HIR.
    currentType = targetType;
    return;
  }
  const bool characterCodeConversion =
      isCharacterCodeConversion(valueType, targetType);
  if (!isNumeric(targetType) && !characterCodeConversion) {
    report(expressionToken(expr.callee()),
           "Generic conversion target '" + typeSpelling(targetType) +
               "' requires the numeric capability.",
           "GTI-S2029");
    valid = false;
  }
  if (valueType != SemanticType::Unknown && !isNumeric(valueType) &&
      !characterCodeConversion) {
    report(expressionToken(expr.arguments().front()),
           "Cannot explicitly convert '" + typeSpelling(valueType) + "' to '" +
               typeSpelling(targetType) +
               "'; numeric conversions require a numeric value.",
           "GTI-S2014");
    valid = false;
  }

  currentType = valid ? targetType : SemanticType::Unknown;
  if (valid) {
    semanticModel.record(
        expr, ResolvedCallInfo{
                  .returnType = targetType,
                  .parameterTypes = {valueType},
                  .typeArguments = {targetType},
                  .intrinsic = IntrinsicKind::NumericTypeParameterConversion});
  }
}

auto SemanticVisitor::Impl::analyzeAliasConversion(
    const Call &expr, const SemanticType &targetType,
    const std::string &aliasName) -> void {
  if (expr.arguments().size() != 1) {
    for (const ExprPtr &argument : expr.arguments()) {
      analyze(argument);
    }
    report(expr.paren(),
           "An explicit conversion requires exactly one value argument.",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType valueType = analyze(expr.arguments().front());
  if (targetType.kind == SemanticType::RawPointer) {
    const bool shaped =
        valueType.kind == SemanticType::RawPointer &&
        valueType.arguments.size() == 1 && targetType.arguments.size() == 1 &&
        valueType.arguments.front().kind != SemanticType::RawPointer &&
        targetType.arguments.front().kind != SemanticType::RawPointer;
    const bool compatible =
        shaped &&
        (valueType.pointerAccess == AccessMode::Mutable ||
         targetType.pointerAccess == AccessMode::ReadOnly) &&
        (valueType.arguments.front() == targetType.arguments.front() ||
         valueType.arguments.front() == SemanticType::Void ||
         targetType.arguments.front() == SemanticType::Void);
    if (!compatible) {
      report(expressionToken(expr.callee()),
             "Explicit raw pointer conversion requires typed/void pointers "
             "or the same pointee type, and cannot remove pointee constness.",
             "GTI-S2014");
      currentType = SemanticType::Unknown;
      return;
    }
    if (targetType.arguments.front() != SemanticType::Void) {
      for (const auto &origin : rawPointerOrigins(expr.arguments().front())) {
        if (origin.pointeeType != SemanticType::Unknown &&
            origin.pointeeType != targetType.arguments.front()) {
          diagnostics.push_back(makeDiagnostic(
              origin.possible ? DiagnosticId::RawAuditPotential
                              : DiagnosticId::RawAuditViolation,
              expressionToken(expr.callee()),
              {std::string(origin.possible
                               ? "Explicit raw pointer conversion may "
                                 "contradict a possible pointee type '"
                               : "Explicit raw pointer conversion "
                                 "contradicts the known pointee type '") +
               typeSpelling(origin.pointeeType) + "'."}));
          diagnostics.back().related.push_back(
              {origin.addressSite,
               "Pointee type retained at this conversion."});
          break;
        }
      }
    }
    currentType = targetType;
    semanticModel.record(
        expr,
        ResolvedCallInfo{.returnType = targetType,
                         .parameterTypes = {valueType},
                         .intrinsic = IntrinsicKind::RawPointerConversion});
    return;
  }
  const bool characterCodeConversion =
      isCharacterCodeConversion(valueType, targetType);
  if (valueType != SemanticType::Unknown && !isNumeric(valueType) &&
      !characterCodeConversion) {
    report(expressionToken(expr.arguments().front()),
           "Cannot explicitly convert '" + typeSpelling(valueType) + "' to '" +
               aliasName + "'; numeric conversions require a numeric value.",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }
  if (isInteger(targetType) && isInteger(valueType)) {
    if (const std::optional<IntegerConstant> constant =
            integerConstant(expr.arguments().front().get());
        constant && !integerFits(targetType, *constant)) {
      report(expressionToken(expr.arguments().front()),
             "Integer value is outside the range of '" + aliasName + "'.",
             "GTI-S2014");
    }
  }

  currentType = targetType;
  semanticModel.record(
      expr,
      ResolvedCallInfo{.returnType = targetType,
                       .parameterTypes = {valueType},
                       .intrinsic = IntrinsicKind::NumericAliasConversion});
}

auto SemanticVisitor::Impl::visitConditionalExpr(const ConditionalExpr &expr)
    -> void {
  const SemanticType conditionType =
      analyzeExpectedCallableResult(expr.condition(), SemanticType::Bool);
  requireBool(expr.condition(), conditionType,
              expressionToken(expr.condition()),
              "Conditional expression condition must be bool.");

  const ScopeStack beforeBranches = scopes;
  const SemanticType contextualType =
      contextualInitializerType.value_or(SemanticType::Unknown);
  ++loanFlow.rawConditionalExpressionDepth;
  // An auto initializer can defer an arm's callable result until concrete
  // semantic reanalysis. A discarded conditional keeps the existing void-call
  // requirement; unresolved must not silently stand for void in a value join.
  const bool inferredValue = contextualInitializerType.has_value();
  const SemanticType thenType =
      contextualType == SemanticType::Unknown && !inferredValue
          ? analyze(expr.thenExpression())
          : analyzeExpectedCallableResult(expr.thenExpression(), contextualType,
                                          contextualType ==
                                              SemanticType::Unknown);
  const ScopeStack thenScopes = scopes;

  // An enclosing assignment may hold a place rooted in the current scopes.
  restoreScopeState(beforeBranches);
  const SemanticType elseExpectation =
      contextualType == SemanticType::Unknown ? thenType : contextualType;
  const SemanticType elseType =
      elseExpectation == SemanticType::Unknown && !inferredValue
          ? analyze(expr.elseExpression())
          : analyzeExpectedCallableResult(
                expr.elseExpression(), elseExpectation,
                elseExpectation == SemanticType::Unknown);
  --loanFlow.rawConditionalExpressionDepth;
  const ScopeStack elseScopes = scopes;

  restoreScopeState(beforeBranches);
  mergeValueStates(beforeBranches, thenScopes, elseScopes);
  const ScopeStack loanScopes = scopes;
  ScopeStack moveScopes = loanScopes;
  if (const std::optional<bool> condition = constantBoolean(expr.condition())) {
    moveScopes = *condition ? thenScopes : elseScopes;
  }
  restoreScopeState(loanScopes);
  applyMoveStates(moveScopes);

  if (thenType == SemanticType::Unknown || elseType == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  SemanticType joinedType = thenType;
  if (thenType != elseType) {
    const bool thenPointerJoin =
        thenType.kind == SemanticType::RawPointer &&
        isAssignable(thenType, elseType, expr.elseExpression().get());
    const bool elsePointerJoin =
        elseType.kind == SemanticType::RawPointer &&
        isAssignable(elseType, thenType, expr.thenExpression().get());
    if (thenPointerJoin) {
      joinedType = thenType;
    } else if (elsePointerJoin) {
      joinedType = elseType;
    } else {
      report(expr.colon(),
             "Conditional expression arms must have the same exact type; "
             "the true arm is '" +
                 typeSpelling(thenType) + "' and the false arm is '" +
                 typeSpelling(elseType) + "'.",
             "GTI-S2050");
      currentType = SemanticType::Unknown;
      return;
    }
  }

  currentType = joinedType;
  if (currentType == SemanticType::Void) {
    return;
  }

  const SemanticTypeTraits traits = typeTraits(currentType);
  if (traits.containsBorrowedState) {
    report(expr.question(),
           "Conditional expressions cannot yet produce a value of type '" +
               typeSpelling(currentType) +
               "' because it contains borrowed state.",
           "GTI-S2051");
    return;
  }

  const auto validateArmMaterialization = [&](const ExprPtr &arm,
                                              std::string_view name) {
    const ExpressionInfo *info =
        arm ? semanticModel.findExpression(*arm) : nullptr;
    if (info == nullptr || info->category != ValueCategory::Place ||
        traits.copyable) {
      return;
    }
    report(expressionToken(arm),
           std::string("The ") + std::string(name) +
               " arm is a place of non-copyable type '" +
               typeSpelling(currentType) +
               "'; transfer it explicitly with std::move(...).",
           "GTI-S2052");
  };
  validateArmMaterialization(expr.thenExpression(), "true");
  validateArmMaterialization(expr.elseExpression(), "false");

  if (!traits.movable) {
    report(expr.question(),
           "Conditional result type '" + typeSpelling(currentType) +
               "' is not movable and cannot be merged into one value.",
           "GTI-S2053");
  }
}

auto SemanticVisitor::Impl::visitConversionExpr(const Conversion &expr)
    -> void {
  validateType(expr.targetType());
  const SemanticType targetType = typeOf(expr.targetType());
  const SemanticType valueType = analyze(expr.value());
  const bool characterCodeConversion =
      isCharacterCodeConversion(valueType, targetType);
  if (!isNumeric(targetType) && !characterCodeConversion) {
    report(expr.targetType().name.last(),
           "Explicit conversions currently require a numeric target type.",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }
  if (valueType != SemanticType::Unknown && !isNumeric(valueType) &&
      !characterCodeConversion) {
    report(expressionToken(expr.value()),
           "Cannot explicitly convert '" + typeSpelling(valueType) + "' to '" +
               typeSpelling(targetType) +
               "'; numeric conversions require a numeric value.",
           "GTI-S2014");
    currentType = SemanticType::Unknown;
    return;
  }
  if (isInteger(targetType) && isInteger(valueType)) {
    if (const std::optional<IntegerConstant> constant =
            integerConstant(expr.value().get());
        constant && !integerFits(targetType, *constant)) {
      report(expressionToken(expr.value()),
             "Integer value is outside the range of '" +
                 typeSpelling(targetType) + "'.",
             "GTI-S2014");
    }
  }
  currentType = targetType;
}

auto SemanticVisitor::Impl::analyzeDesignatedInitializer(
    const DirectInitializer &expr) -> void {
  const SemanticType target =
      contextualInitializerType
          ? selectExactClassSpecialization(*contextualInitializerType)
          : SemanticType::Unknown;
  const ClassInfo *owner =
      target.kind == SemanticType::Class ? classInfo(target) : nullptr;
  // An enclosing return/call's scalar expectation is not a struct destination
  // for this list. Refine the existing rejection without changing acceptance
  // of eligible contextual construction, including nested designated fields.
  if (owner == nullptr && (target == SemanticType::Unknown ||
                           contextualInitializerExpression != &expr)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2091", DiagnosticPhase::Semantics, expr.brace(),
        "A designated initializer requires a destination struct type; this "
        "expression has no contextual destination type.");
    diagnostic.hints.emplace_back(
        "Initialize a binding with an explicit struct type using this list, "
        "then use that value in the expression.");
    diagnostics.emplace_back(std::move(diagnostic));
    currentType = SemanticType::Unknown;
    return;
  }
  if (owner == nullptr || owner->kind != ClassKind::Struct ||
      !owner->designatedConstructor) {
    report(
        expr.brace(),
        "Designated construction requires a struct with public value fields, "
        "no declared constructors, and no inheritance; reference, fixed-array, "
        "borrowed-state, and native-layout fields are not supported here.",
        "GTI-S2091");
    currentType = SemanticType::Unknown;
    return;
  }
  const ConstructorInfo &constructor = *owner->designatedConstructor;
  std::vector<const Expr *> inputs(owner->fields.size(), nullptr);
  std::vector<SemanticType> types;
  std::optional<std::size_t> previous;
  bool valid = true;
  for (const FieldInfo &entry : owner->fields) {
    types.push_back(
        substituteSymbol(
            owner->members.at(entry.declaration->name().lexeme).symbol, target)
            .type);
  }
  for (std::size_t i = 0; i < expr.designators().size(); ++i) {
    const Token &name = expr.designators()[i];
    auto found =
        std::find_if(owner->fields.begin(), owner->fields.end(),
                     [&](const FieldInfo &field) {
                       return field.declaration->name().lexeme == name.lexeme;
                     });
    if (found == owner->fields.end()) {
      report(name,
             "Unknown instance field '" + name.lexeme + "' in struct '" +
                 owner->name.lexeme + "'.",
             "GTI-S2091");
      valid = false;
      continue;
    }
    const std::size_t index = std::distance(owner->fields.begin(), found);
    if (inputs[index] != nullptr || (previous && index < *previous)) {
      report(name,
             inputs[index] != nullptr
                 ? "Duplicate designated initializer for field '" +
                       name.lexeme + "'."
                 : "Designated fields must follow struct declaration order.",
             "GTI-S2091");
      valid = false;
    }
    previous = index;
    inputs[index] = expr.arguments()[i].get();
    const SemanticType actual =
        analyzeInitializer(expr.arguments()[i], types[index]);
    const bool needsExplicitNumericConversion =
        actual != types[index] && isNumeric(actual) &&
        isNumeric(types[index]) &&
        !isContextualIntegerLiteral(expr.arguments()[i].get());
    if (actual != SemanticType::Unknown &&
        (needsExplicitNumericConversion ||
         !isOwnershipAssignable(types[index], actual, expr.arguments()[i]))) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2091", DiagnosticPhase::Semantics, name,
                         "Cannot initialize designated field '" + name.lexeme +
                             "' of type '" + typeSpelling(types[index]) +
                             "' from '" + typeSpelling(actual) + "'.");
      if (actual == types[index] && isMoveOnlyOwnerType(actual)) {
        diagnostic.hints.emplace_back(
            "This field is move-only; transfer a named owner with std::move, "
            "or supply a fresh value.");
      }
      if (needsExplicitNumericConversion) {
        diagnostic.hints.emplace_back(
            "Designated construction requires an exact numeric field type; "
            "convert this value explicitly to '" +
            typeSpelling(types[index]) + "'.");
      }
      diagnostic.related.push_back(
          {tokenSpan(found->declaration->name()), "Field declared here."});
      diagnostics.push_back(std::move(diagnostic));
      valid = false;
    }
    if ((types[index].kind == SemanticType::StringView ||
         types[index].kind == SemanticType::CString) &&
        isOwnerDependentTextExpression(expr.arguments()[i])) {
      reportOwnerDependentTextStorage(expr.arguments()[i], "stored in a field");
      valid = false;
    }
    const auto &member = owner->members.at(name.lexeme);
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(name),
         .kind = SemanticOccurrenceKind::Expression,
         .symbol = toolingSymbolFor(member.symbol),
         .roles = OccurrenceRole::Reference | OccurrenceRole::Write,
         .name = name.lexeme,
         .type = types[index]});
  }
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    if (inputs[index] == nullptr) {
      const VariableDecl &field = *owner->fields[index].declaration;
      inputs[index] = field.initializer().get();
      if (inputs[index] == nullptr) {
        report(expr.brace(),
               "Missing initializer for field '" + field.name().lexeme +
                   "'; the field has no declaration default.",
               "GTI-S2091");
        valid = false;
      } else {
        const SemanticType actual = semanticModel.typeOf(*inputs[index]);
        if (actual != types[index] && isNumeric(actual) &&
            isNumeric(types[index]) &&
            !isContextualIntegerLiteral(inputs[index])) {
          report(expr.brace(),
                 "Default initializer for designated field '" +
                     field.name().lexeme +
                     "' requires an explicit conversion to '" +
                     typeSpelling(types[index]) + "'.",
                 "GTI-S2091");
          valid = false;
        }
      }
    }
  }
  if (valid) {
    semanticModel.record(expr, ResolvedConstructionInfo{
                                   .constructor = constructor.id,
                                   .declaration = constructor.declaration,
                                   .constructedType = target,
                                   .parameterTypes = std::move(types),
                                   .designatedArguments = std::move(inputs)});
  }
  currentType = valid ? target : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::visitDirectInitializerExpr(
    const DirectInitializer &expr) -> void {
  if (!expr.designators().empty()) {
    analyzeDesignatedInitializer(expr);
    return;
  }
  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(expr.arguments().size());
  bool deferredCallableTail = false;
  for (const ExprPtr &argument : expr.arguments()) {
    deferredCallableTail =
        deferredCallableTail || isContextualCallableResultArgument(argument);
    argumentTypes.emplace_back(deferredCallableTail ? SemanticType::Unknown
                                                    : analyze(argument));
  }

  if (!contextualInitializerType) {
    report(expr.brace(),
           "'auto' cannot use direct brace initialization because the "
           "constructed class type is not known; use 'auto name = "
           "Type(arguments)'.",
           "GTI-S2028");
    currentType = SemanticType::Unknown;
    return;
  }

  const SemanticType targetType = *contextualInitializerType;
  if (targetType == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (targetType.kind != SemanticType::Class) {
    if (targetType.kind == SemanticType::Reference) {
      report(expr.brace(),
             "Direct brace construction cannot initialize a reference; "
             "bind an existing addressable value with '='.",
             "GTI-S2038");
    } else if (targetType.kind == SemanticType::Array) {
      report(expr.brace(),
             "Direct brace construction is limited to classes and "
             "structs; initialize a fixed array with '= {...}'.",
             "GTI-S2038");
    } else if (targetType.kind == SemanticType::Enum) {
      report(expr.brace(),
             "Scoped enums require an explicit enumerator initializer "
             "with '='.",
             "GTI-S2038");
    } else {
      report(expr.brace(),
             "Direct brace construction is limited to classes and "
             "structs.",
             "GTI-S2038");
    }
    currentType = SemanticType::Unknown;
    return;
  }

  analyzeConstructorCall(expr, targetType.classId, targetType.arguments,
                         targetType.valueArguments, argumentTypes,
                         expr.arguments(), expr.brace());
}

auto SemanticVisitor::Impl::visitDereferenceSetExpr(const DereferenceSet &expr)
    -> void {
  const SemanticType ownerType = analyzeProjectionBase(expr.object());
  const bool simpleAssignment = expr.oper().kind == TokenKind::EQUAL;
  SemanticType valueTarget = SemanticType::Unknown;
  bool mutableTarget = isMutableObject(expr.object());
  if (ownerType.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(ownerType, expr.dereference(),
                                      "raw-pointer dereference")) {
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }
    if (ownerType.arguments.size() != 1 ||
        ownerType.arguments.front() == SemanticType::Void) {
      report(expr.dereference(), "void* cannot be dereferenced.", "GTI-S2056");
    } else {
      valueTarget = ownerType.arguments.front();
    }
    mutableTarget = ownerType.pointerAccess == AccessMode::Mutable;
    if (valueTarget != SemanticType::Unknown && mutableTarget) {
      requireUnsafe(expr, UnsafeOperationKind::RawDereference,
                    expr.dereference(), "Raw pointer dereference");
      validateRawPointerAccess(expr.object(), expr.dereference(),
                               AccessMode::Mutable);
    }
  } else if (ownerType.kind == SemanticType::Class ||
             (ownerType.kind == SemanticType::TypeParameter &&
              hasSymbolicOperatorRequirement(OverloadedOperator::Dereference,
                                             ownerType))) {
    const std::optional<CallableCandidate> selected =
        resolveOperator(expr, OverloadedOperator::Dereference, expr.object(),
                        ownerType, expr.dereference(), {}, {}, false,
                        std::nullopt, std::nullopt, !simpleAssignment);
    if (selected) {
      valueTarget = callExpressionType(selected->returnType);
      mutableTarget =
          selected->returnType.kind == SemanticType::Reference &&
          selected->returnType.referenceAccess == AccessMode::Mutable;
    }
  } else if (ownerType != SemanticType::Unknown) {
    report(expr.dereference(),
           "Dereference assignment requires an owning value or a class "
           "defining operator*.",
           "GTI-S2022");
  }

  SemanticPlace targetPlace = semanticPlace(expr.object());
  if (targetPlace.root != nullptr) {
    targetPlace.projections.push_back(
        {.kind = PlaceProjectionKind::Dereference});
  }
  const std::optional<OverloadedOperator> mutationOperator =
      compoundOperator(expr.oper().kind);
  const bool overloadedMutation = !simpleAssignment && mutationOperator &&
                                  valueTarget.kind == SemanticType::Class;
  const bool preferReadOnlyRhsProjection =
      simpleAssignment && ownerType.kind != SemanticType::RawPointer &&
      mutableTarget;
  if (preferReadOnlyRhsProjection) {
    ++preferReadOnlySubscriptReceiverDepth;
  }
  const SemanticType valueType =
      simpleAssignment ? analyzeInitializer(expr.value(), valueTarget)
                       : analyze(expr.value());
  if (preferReadOnlyRhsProjection) {
    --preferReadOnlySubscriptReceiverDepth;
  }
  if (!mutableTarget) {
    report(expr.dereference(),
           ownerType.kind == SemanticType::RawPointer
               ? "Cannot write through const raw "
                 "pointer memory."
               : "Dereference assignment requires "
                 "mutable access.",
           "GTI-S2002");
  } else if (!overloadedMutation) {
    if (targetPlace.root != nullptr) {
      reportLoanPlaceConflict(
          expr.dereference(), targetPlace, AccessMode::Mutable,
          "Cannot write through a dereference while an overlapping borrow "
          "may still be live.");
    }
  }
  if (simpleAssignment &&
      !isOwnershipAssignment(valueTarget, valueType, expr.value())) {
    const ExpressionInfo *valueInfo =
        semanticModel.findExpression(*expr.value());
    const bool moving =
        valueInfo != nullptr && valueInfo->category == ValueCategory::Value;
    const bool sameType =
        valueTarget == valueType && valueTarget != SemanticType::Unknown;
    report(expressionToken(expr.value()),
           sameType
               ? "Cannot " + std::string(moving ? "move" : "copy") +
                     " assign type '" + typeSpelling(valueTarget) +
                     "' through a dereference: that assignment is unavailable."
               : "Cannot assign a value of type '" + typeSpelling(valueType) +
                     "' through a dereference of type '" +
                     typeSpelling(valueTarget) + "'.",
           "GTI-S2003");
    if (sameType && !moving && typeTraits(valueTarget).moveAssignable) {
      diagnostics.back().hints.emplace_back(
          "Use an owned value or explicit std::move(source) to transfer "
          "ownership, or clone the value when the source must be preserved.");
    }
  }
  if (overloadedMutation && mutableTarget) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *mutationOperator, expr.object(), valueTarget, expr.oper(),
        std::span<const SemanticType>(&valueType, 1),
        std::span<const ExprPtr>(&expr.value(), 1), false, std::nullopt,
        OperatorReceiverState{.place = targetPlace, .mutableReceiver = true});
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }
  if (!simpleAssignment && !overloadedMutation) {
    validateCompoundAssignment(expr.oper(), valueTarget, valueType,
                               expr.value().get());
  }
  if (ownerType.kind == SemanticType::RawPointer && simpleAssignment &&
      mutableTarget &&
      isOwnershipAssignment(valueTarget, valueType, expr.value())) {
    assignRawPointerFields(targetPlace, expr.value().get());
  }
  currentType = valueTarget;
}

auto SemanticVisitor::Impl::visitPlaceSetExpr(const PlaceSet &expr) -> void {
  const SemanticType targetType = analyzeProjectionBase(expr.target());
  const ExpressionInfo *target = semanticModel.findExpression(*expr.target());
  const bool mutableTarget = target != nullptr &&
                             target->category == ValueCategory::Place &&
                             target->access == AccessMode::Mutable;
  const bool simpleAssignment = expr.oper().kind == TokenKind::EQUAL;
  const SemanticType valueType =
      simpleAssignment ? analyzeInitializer(expr.value(), targetType)
                       : analyze(expr.value());
  if (targetType == SemanticType::Unknown) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (!mutableTarget) {
    report(expr.oper(),
           "Assignment requires a mutable place; an owned call result or "
           "read-only reference is not writable.",
           "GTI-S2002");
    currentType = targetType;
    return;
  }

  const SemanticPlace targetPlace = semanticPlace(expr.target());
  if (targetPlace.root != nullptr) {
    reportLoanPlaceConflict(expr.oper(), targetPlace, AccessMode::Mutable,
                            "Cannot write while an overlapping borrow may "
                            "still be live.");
  }
  // Unlike a source operator's reserved projection, an ordinary accessor
  // executes fully before the right operand. Its mutable result remains
  // borrowed until the store, so later operands cannot access that owner.
  if (const auto production = transientBorrowProduction(expr.target());
      production && production->access == AccessMode::Mutable) {
    std::vector<SemanticLoanPlace> reads;
    std::vector<TransientBorrowProduction> productions;
    appendTransientReadPlaces(expr.value(), reads, productions);
    std::vector<SemanticLoanPlace> mutations;
    appendTransientMutationPlaces(expr.value(), mutations);
    const auto overlaps = [&](const SemanticLoanPlace &place) {
      return loanPlacesOverlap(production->place, place);
    };
    if (std::any_of(reads.begin(), reads.end(), overlaps) ||
        std::any_of(mutations.begin(), mutations.end(), overlaps)) {
      report(expr.oper(),
             "The right operand accesses storage still mutably borrowed "
             "by the assignment target.",
             "GTI-S2017");
      diagnostics.back().related.push_back(
          {tokenSpan(expressionToken(expr.target())),
           "This target produces the mutable reference before the right "
           "operand is evaluated."});
    }
  }
  const auto mutationOperator = compoundOperator(expr.oper().kind);
  if (!simpleAssignment && mutationOperator &&
      targetType.kind == SemanticType::Class) {
    const auto selected = resolveOperator(
        expr, *mutationOperator, expr.target(), targetType, expr.oper(),
        std::span<const SemanticType>(&valueType, 1),
        std::span<const ExprPtr>(&expr.value(), 1));
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }
  if (simpleAssignment &&
      !isOwnershipAssignment(targetType, valueType, expr.value())) {
    report(expressionToken(expr.value()),
           "Cannot assign a value of type '" + typeSpelling(valueType) +
               "' to a mutable place of type '" + typeSpelling(targetType) +
               "'.",
           "GTI-S2003");
  } else if (simpleAssignment && targetPlace.root != nullptr) {
    reinitializePlace(targetPlace);
    assignRawPointerFields(targetPlace, expr.value().get());
  } else if (!simpleAssignment) {
    validateCompoundAssignment(expr.oper(), targetType, valueType,
                               expr.value().get());
  }
  currentType = targetType;
}

auto SemanticVisitor::Impl::visitGetExpr(const Get &expr) -> void {
  SemanticType objectType = analyzeProjectionBase(expr.object());
  if (foldAttributeMemberRead(expr, objectType)) {
    return;
  }
  if (expr.access().kind == TokenKind::ARROW) {
    objectType =
        arrowTargetType(expr, expr.object(), objectType, expr.access());
    if (objectType == SemanticType::Unknown) {
      currentType = SemanticType::Unknown;
      return;
    }
  }
  if (expr.name().completion) {
    const ExpressionInfo *receiver =
        semanticModel.findExpression(*expr.object());
    const bool mutableReceiver = receiver != nullptr &&
                                 receiver->category == ValueCategory::Place &&
                                 receiver->access == AccessMode::Mutable;
    captureMemberCompletion(expr.name(), objectType, mutableReceiver);
    currentType = SemanticType::Unknown;
    return;
  }
  const bool generatedRangeEndpoint =
      expr.name().generated &&
      (expr.name().lexeme == "begin" || expr.name().lexeme == "begin_mut" ||
       expr.name().lexeme == "end");
  const std::optional<SemanticType> symbolicRangeMember =
      objectType.kind == SemanticType::TypeParameter
          ? symbolicRangeMemberReturnType(objectType, expr.name().lexeme)
          : std::nullopt;
  if (symbolicRangeMember && expr.name().lexeme != "size" &&
      !generatedRangeEndpoint) {
    report(expr.name(),
           "Direct structural range endpoint extraction is not supported "
           "yet. Use range-based for iteration.",
           "GTI-S2029");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbolicRangeMember) {
    if (!analyzingCallCallee) {
      report(expr.name(),
             "Structural range member names must be called immediately.");
    }
    currentType = SemanticType::Function;
    return;
  }
  if (containsOpenTypeParameter(objectType) &&
      symbolicReadMemberReturnType(objectType, expr.name().lexeme)) {
    if (!analyzingCallCallee) {
      report(expr.name(), "Required member names must be called immediately.");
    }
    currentType = SemanticType::Function;
    return;
  }
  if (objectType.kind == SemanticType::Array) {
    const bool generatedRangeEndpoint =
        expr.name().generated &&
        (expr.name().lexeme == "begin" || expr.name().lexeme == "begin_mut" ||
         expr.name().lexeme == "end");
    if (expr.name().lexeme == "size" || generatedRangeEndpoint) {
      if (!analyzingCallCallee) {
        report(expr.name(),
               "Function names must be called; function values are not "
               "supported yet.");
      }
      currentType = SemanticType::Function;
    } else {
      report(expr.name(),
             "Unknown fixed array member '" + expr.name().lexeme + "'.",
             "GTI-S2016");
      currentType = SemanticType::Unknown;
    }
    return;
  }
  if (objectType.kind == SemanticType::StringView) {
    if (expr.name().lexeme == "size" || expr.name().lexeme == "empty" ||
        textViewMemberFactory(expr.name().lexeme).has_value()) {
      if (!analyzingCallCallee) {
        report(expr.name(),
               "Function names must be called; function values are not "
               "supported yet.");
      }
      currentType = SemanticType::Function;
    } else {
      report(expr.name(),
             "Unknown std::string_view member '" + expr.name().lexeme + "'.",
             "GTI-S2035");
      currentType = SemanticType::Unknown;
    }
    return;
  }
  if (objectType.kind == SemanticType::Expected) {
    if (expr.name().lexeme == "has_value" || expr.name().lexeme == "value" ||
        expr.name().lexeme == "error" || expr.name().lexeme == "value_or" ||
        expr.name().lexeme == "transform_error" ||
        expr.name().lexeme == "transform" || expr.name().lexeme == "and_then" ||
        expr.name().lexeme == "expect") {
      if (!analyzingCallCallee) {
        report(expr.name(),
               "Function names must be called; function values are not "
               "supported yet.");
      }
      currentType = SemanticType::Function;
    } else {
      report(expr.name(),
             "Unknown expected member '" + expr.name().lexeme + "'.");
      currentType = SemanticType::Unknown;
    }
    return;
  }
  const MemberInfo *member = resolveMember(objectType, expr.name());
  if (member == nullptr) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (objectType.kind == SemanticType::Class && objectType.classId != 0 &&
      objectType.classId <= classes.size() &&
      classInfo(objectType.classId).kind == ClassKind::Union) {
    requireUnsafe(expr, UnsafeOperationKind::UnionMember, expr.name(),
                  "Union member access");
  }
  if (member->symbol.staticMember) {
    report(expr.name(),
           "Static member '" + expr.name().lexeme +
               "' must be accessed through its class or struct name.",
           "GTI-S2039");
    currentType = SemanticType::Unknown;
    return;
  }
  const SemanticType memberType =
      substituteSymbol(member->symbol, objectType).type;
  currentType = memberType.kind == SemanticType::Reference &&
                        memberType.arguments.size() == 1
                    ? memberType.arguments.front()
                    : memberType;
  if (currentType == SemanticType::Function && !analyzingCallCallee) {
    report(expr.name(),
           "Function names must be called; function values are not "
           "supported yet.");
  }
}

auto SemanticVisitor::Impl::visitGroupingExpr(const Grouping &expr) -> void {
  if (expr.unsafeKeyword())
    enterUnsafeRegion(*expr.unsafeKeyword(), true);
  currentType = analyze(expr.expression());
  if (expr.unsafeKeyword())
    leaveUnsafeRegion();
}

auto SemanticVisitor::Impl::visitIndexExpr(const Index &expr) -> void {
  const SemanticType objectType = analyzeProjectionBase(expr.object());
  const SemanticType indexType = analyze(expr.index());
  semanticModel.recordPlaceSelection(expr, nextPlaceSelectionId++);
  if (objectType.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(objectType, expr.bracket(),
                                      "raw-pointer indexing")) {
      currentType = SemanticType::Unknown;
      return;
    }
    const bool validIndex =
        isInteger(indexType) || indexType == SemanticType::Unknown;
    if (!validIndex) {
      report(expressionToken(expr.index()),
             "Raw pointer index must have an integer type.", "GTI-S2056");
    }
    const bool validPointee =
        objectType.arguments.size() == 1 &&
        objectType.arguments.front() != SemanticType::Void;
    if (!validPointee) {
      report(expr.bracket(), "void* cannot be indexed.", "GTI-S2056");
      currentType = SemanticType::Unknown;
    } else {
      currentType = objectType.arguments.front();
    }
    if (validIndex && validPointee) {
      requireUnsafe(expr, UnsafeOperationKind::RawIndex, expr.bracket(),
                    "Raw pointer indexing");
      validateRawPointerAccess(expr.object(), expr.bracket(),
                               AccessMode::ReadOnly);
    }
    return;
  }
  if (objectType.kind == SemanticType::StringView) {
    currentType = analyzeStringViewIndexAfterOperands(
        expr.object(), expr.index(), indexType, expr.bracket());
    return;
  }
  if (objectType.kind == SemanticType::Class ||
      (objectType.kind == SemanticType::TypeParameter &&
       hasSymbolicOperatorRequirement(
           OverloadedOperator::Subscript, objectType,
           std::span<const SemanticType>(&indexType, 1)))) {
    const bool mutableReferenceContext =
        contextualInitializerType &&
        contextualInitializerType->kind == SemanticType::Reference &&
        contextualInitializerType->referenceAccess == AccessMode::Mutable;
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, OverloadedOperator::Subscript, expr.object(), objectType,
        expr.bracket(), std::span<const SemanticType>(&indexType, 1),
        std::span<const ExprPtr>(&expr.index(), 1), false, std::nullopt,
        std::nullopt, false,
        preferReadOnlySubscriptReceiverDepth != 0 && !mutableReferenceContext);
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }
  currentType = analyzeArrayIndexAfterOperands(objectType, expr.index(),
                                               indexType, expr.bracket());
}

auto SemanticVisitor::Impl::visitIndexSetExpr(const IndexSet &expr) -> void {
  const SemanticType objectType = analyzeProjectionBase(expr.object());
  const SemanticType indexType = analyze(expr.index());
  const bool simpleAssignment = expr.oper().kind == TokenKind::EQUAL;
  semanticModel.recordPlaceSelection(expr, nextPlaceSelectionId++);
  if (objectType.kind == SemanticType::StringView) {
    const SemanticType elementType = analyzeStringViewIndexAfterOperands(
        expr.object(), expr.index(), indexType, expr.bracket());
    const SemanticType valueType =
        simpleAssignment ? analyzeInitializer(expr.value(), elementType)
                         : analyze(expr.value());
    report(expr.bracket(),
           "Cannot assign through std::string_view; character access is "
           "read-only.",
           "GTI-S2035");
    if (simpleAssignment &&
        !isAssignable(elementType, valueType, expr.value().get())) {
      report(expressionToken(expr.value()),
             "Cannot assign a value of type '" + typeSpelling(valueType) +
                 "' to a string-view character of type 'char'.",
             "GTI-S2003");
    }
    currentType = elementType;
    return;
  }
  SemanticType elementType = SemanticType::Unknown;
  const ResolvedOperatorInfo *resolvedOperator = nullptr;
  if (objectType.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(objectType, expr.bracket(),
                                      "raw-pointer indexing")) {
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }
    const bool validIndex =
        isInteger(indexType) || indexType == SemanticType::Unknown;
    if (!validIndex) {
      report(expressionToken(expr.index()),
             "Raw pointer index must have an integer type.", "GTI-S2056");
    }
    const bool validPointee =
        objectType.arguments.size() == 1 &&
        objectType.arguments.front() != SemanticType::Void;
    if (!validPointee) {
      report(expr.bracket(), "void* cannot be indexed.", "GTI-S2056");
    } else {
      elementType = objectType.arguments.front();
    }
    if (validIndex && validPointee &&
        objectType.pointerAccess == AccessMode::Mutable) {
      requireUnsafe(expr, UnsafeOperationKind::RawIndex, expr.bracket(),
                    "Raw pointer indexing");
      validateRawPointerAccess(expr.object(), expr.bracket(),
                               AccessMode::Mutable);
    }
  } else if (objectType.kind == SemanticType::Class ||
             (objectType.kind == SemanticType::TypeParameter &&
              hasSymbolicOperatorRequirement(
                  OverloadedOperator::Subscript, objectType,
                  std::span<const SemanticType>(&indexType, 1)))) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, OverloadedOperator::Subscript, expr.object(), objectType,
        expr.bracket(), std::span<const SemanticType>(&indexType, 1),
        std::span<const ExprPtr>(&expr.index(), 1), false, std::nullopt,
        std::nullopt, !simpleAssignment);
    if (selected) {
      elementType = callExpressionType(selected->returnType);
      resolvedOperator = simpleAssignment
                             ? semanticModel.findOperator(expr)
                             : semanticModel.findProjectionOperator(expr);
    }
  } else {
    elementType = analyzeArrayIndexAfterOperands(objectType, expr.index(),
                                                 indexType, expr.bracket());
  }
  SemanticPlace targetPlace = semanticPlace(expr);
  const std::optional<OverloadedOperator> mutationOperator =
      compoundOperator(expr.oper().kind);
  const bool overloadedMutation = !simpleAssignment && mutationOperator &&
                                  elementType.kind == SemanticType::Class;
  const bool trackedFixedArrayElement =
      objectType.kind == SemanticType::Array && targetPlace.root != nullptr &&
      !targetPlace.projections.empty() &&
      (targetPlace.projections.back().kind ==
           PlaceProjectionKind::ConstantIndex ||
       targetPlace.projections.back().kind ==
           PlaceProjectionKind::DynamicIndex);
  bool targetStateValid = true;
  if (trackedFixedArrayElement) {
    if (!simpleAssignment) {
      if (const ProjectedValueState *state =
              unavailableProjectedState(targetPlace)) {
        reportUnavailablePlace(expr.bracket(), *state, &targetPlace);
        targetStateValid = false;
      }
    } else if (const ProjectedValueState *state =
                   unavailableAncestorState(targetPlace)) {
      reportUnavailablePlace(expr.bracket(), *state, &targetPlace);
      targetStateValid = false;
    }
  }
  const bool preferReadOnlyRhsSubscripts =
      simpleAssignment && resolvedOperator != nullptr &&
      resolvedOperator->returnType.kind == SemanticType::Reference &&
      resolvedOperator->returnType.referenceAccess == AccessMode::Mutable;
  if (preferReadOnlyRhsSubscripts) {
    ++preferReadOnlySubscriptReceiverDepth;
  }
  const SemanticType valueType =
      simpleAssignment ? analyzeInitializer(expr.value(), elementType)
                       : analyze(expr.value());
  if (preferReadOnlyRhsSubscripts) {
    --preferReadOnlySubscriptReceiverDepth;
  }
  const SemanticPlace movedSource = movedSourcePlace(expr.value());
  const bool directSelfMove =
      trackedFixedArrayElement && samePlace(targetPlace, movedSource);
  bool uncertainMoveAlias = false;
  if (trackedFixedArrayElement && movedSource.root != nullptr &&
      !directSelfMove) {
    const PlaceRelationResult relation =
        placeRelation(placeKey(targetPlace), placeKey(movedSource));
    uncertainMoveAlias = !relation.compatibleDomain ||
                         relation.relation == PlaceRelation::MayAlias ||
                         relation.relation == PlaceRelation::LeftStrictPrefix ||
                         relation.relation == PlaceRelation::RightStrictPrefix;
  }
  if (directSelfMove) {
    report(expr.bracket(), "Cannot move a place into itself.", "GTI-S2018");
  } else if (uncertainMoveAlias) {
    report(expr.bracket(),
           "Cannot assign a moved value through an index that may alias "
           "its source place.",
           "GTI-S2018");
  }
  const bool mutableElement =
      objectType.kind == SemanticType::RawPointer
          ? objectType.pointerAccess == AccessMode::Mutable
      : resolvedOperator != nullptr
          ? resolvedOperator->returnType.kind == SemanticType::Reference &&
                resolvedOperator->returnType.referenceAccess ==
                    AccessMode::Mutable
          : isMutableObject(expr.object());
  if (!mutableElement) {
    if (objectType.kind == SemanticType::RawPointer) {
      report(expr.bracket(), "Cannot write through const raw pointer memory.",
             "GTI-S2002");
    } else if (objectType.kind == SemanticType::Class ||
               objectType.kind == SemanticType::TypeParameter) {
      report(expr.bracket(),
             "operator[] does not provide mutable element access.",
             "GTI-S2002");
    } else if (!reportReceiverRestrictedMutation(expr.object())) {
      report(expr.bracket(),
             "Cannot assign through an immutable fixed array binding.",
             "GTI-S2002");
      appendReadOnlyPlaceContext(diagnostics.back(), expr.object());
    }
  } else if (!overloadedMutation) {
    reportLoanPlaceConflict(
        expr.bracket(),
        trackedFixedArrayElement ? targetPlace : semanticPlace(expr.object()),
        AccessMode::Mutable,
        "Cannot write through an index while an overlapping borrow may "
        "still be live.");
  }
  const bool valueAssignable =
      !simpleAssignment ||
      isOwnershipAssignment(elementType, valueType, expr.value());
  if (simpleAssignment && !valueAssignable) {
    report(expressionToken(expr.value()),
           "Cannot assign a value of type '" + typeSpelling(valueType) +
               "' to an array element of type '" + typeSpelling(elementType) +
               "'.",
           "GTI-S2003");
  }
  if (overloadedMutation && mutableElement) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *mutationOperator, expr.object(), elementType, expr.oper(),
        std::span<const SemanticType>(&valueType, 1),
        std::span<const ExprPtr>(&expr.value(), 1), false, std::nullopt,
        OperatorReceiverState{.place = targetPlace, .mutableReceiver = true});
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }
  if (!simpleAssignment && !overloadedMutation) {
    if (elementType.kind == SemanticType::RawPointer &&
        (expr.oper().kind == TokenKind::PLUS_EQUAL ||
         expr.oper().kind == TokenKind::MINUS_EQUAL) &&
        isInteger(valueType) && elementType.arguments.size() == 1 &&
        elementType.arguments.front() != SemanticType::Void &&
        !isCOpaqueHandleType(elementType.arguments.front()) && mutableElement) {
      requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                    "Raw pointer arithmetic");
    }
    validateCompoundAssignment(expr.oper(), elementType, valueType,
                               expr.value().get());
  }
  if (simpleAssignment && trackedFixedArrayElement && mutableElement &&
      valueAssignable && targetStateValid && !directSelfMove &&
      !uncertainMoveAlias) {
    if (elementType.kind == SemanticType::Class ||
        elementType.kind == SemanticType::Array)
      invalidateRawPointerOrigins(targetPlace);
    assignRawPointerFields(targetPlace, expr.value().get());
    const ValueState reinitializeStateBefore =
        projectedValueState(*targetPlace.root, targetPlace.projections);
    reinitializePlace(targetPlace);
    semanticModel.recordOwnershipEvent(
        expr, {.kind = OwnershipEventKind::Reinitialize,
               .place = placeKey(targetPlace),
               .before = reinitializeStateBefore,
               .after = ValueState::Available});
  }
  if (objectType.kind == SemanticType::RawPointer && simpleAssignment &&
      mutableElement && valueAssignable) {
    assignRawPointerFields(targetPlace, expr.value().get());
  }
  currentType = elementType;
}

auto SemanticVisitor::Impl::visitLambdaExpr(const Lambda &expr) -> void {
  const LambdaInfo *lexicalLambda =
      instanceAnalysisActive && instanceBaseModel != nullptr
          ? instanceBaseModel->findLambda(expr)
          : nullptr;
  const LambdaId id =
      lexicalLambda == nullptr ? nextLambdaId++ : lexicalLambda->id;
  if (expr.returnType().name.last().kind == TokenKind::AUTO) {
    report(expr.returnType().name.last(),
           "Lambda return types must be explicit; 'auto' return deduction "
           "is not supported.",
           "GTI-S2027");
  } else {
    validateType(expr.returnType());
  }
  const bool directOwningPayloadTransport = expr.captures().empty();
  const SemanticType returnType = typeOf(expr.returnType());
  if (returnType.kind == SemanticType::FunctionSignature) {
    report(expr.returnType().name.last(),
           "A lambda cannot return a function signature; return an owning "
           "value instead.",
           "GTI-S2093");
  }
  validateOwningPayloadEnumByValueBoundary(
      returnType, expr.returnType().name.last(), "lambda return",
      directOwningPayloadTransport);
  if (returnType.kind == SemanticType::Reference) {
    report(expr.returnType().name.last(),
           "Lambda reference returns require an escape-aware lifetime model "
           "and are not supported yet.",
           "GTI-S2027");
  } else if (typeTraits(returnType).containsBorrowedState) {
    report(expr.returnType().name.last(),
           "Lambda return types cannot carry borrowed state until lambda "
           "owner-dependency summaries are supported.",
           "GTI-S2027");
  }

  std::vector<SemanticType> parameterTypes;
  parameterTypes.reserve(expr.parameters().size());
  for (const Parameter &parameter : expr.parameters()) {
    validateType(parameter.type);
    validateReferencePlacement(parameter.type, true, "lambda parameter");
    const SemanticType parameterType = typeOf(parameter);
    validateOwningPayloadEnumByValueBoundary(
        parameterType, parameter.type.name.last(), "lambda parameter",
        directOwningPayloadTransport);
    const SymbolId parameterSymbol =
        recordParameterBinding(parameter, parameterType);
    BindingInfo parameterInfo =
        bindingInfo(parameterType, parameter.mutability == Mutability::Mutable
                                       ? AccessMode::Mutable
                                       : AccessMode::ReadOnly);
    parameterInfo.symbol = parameterSymbol;
    semanticModel.record(parameter, std::move(parameterInfo));
    if (parameterType == SemanticType::Void) {
      report(parameter.type.name.last(),
             "Lambda parameters cannot have type void.", "GTI-S2027");
    }
    if (parameter.pack) {
      report(*parameter.pack,
             "Variadic lambda parameters are not supported yet.", "GTI-S2027");
    }
    parameterTypes.push_back(parameterType);
  }

  analyzeParameterDefaults(expr.parameters(), parameterTypes,
                           std::span<const std::size_t>{},
                           DefaultArgumentOwnerKind::Function, false,
                           "lambda parameters", false, !instanceAnalysisActive);

  std::unordered_set<std::string> capturedNames;
  std::vector<LambdaCaptureInfo> captures;
  Scope captureScope;
  for (const LambdaCapture &capture : expr.captures()) {
    if (!capturedNames.insert(capture.name.lexeme).second) {
      report(capture.name,
             "Lambda capture '" + capture.name.lexeme +
                 "' is listed more than once.",
             "GTI-S2027");
      continue;
    }
    const Call *moveCall = capture.explicitInitializer()
                               ? directCall(capture.initializer)
                               : nullptr;
    const auto *moveName =
        moveCall == nullptr ? nullptr
                            : exprAs<QualifiedName>(moveCall->callee().get());
    const auto *sourceExpression =
        moveCall == nullptr || moveCall->arguments().size() != 1
            ? nullptr
            : exprAs<Variable>(moveCall->arguments().front().get());
    const bool explicitMoveSyntax =
        capture.explicitInitializer() && moveCall != nullptr &&
        moveCall->typeArguments().empty() && moveName != nullptr &&
        moveName->name().segments.size() == 2 &&
        moveName->name().segments[0].lexeme == "std" &&
        moveName->name().segments[1].lexeme == "move" &&
        sourceExpression != nullptr;
    if (capture.explicitInitializer() && !explicitMoveSyntax) {
      report(capture.equal ? *capture.equal : capture.name,
             "Lambda init captures are limited to the explicit owned-move "
             "form '[name = std::move(local)]'.",
             "GTI-S2027");
      const SymbolId recoverySymbol =
          recordBindingOccurrence(capture.name, SemanticType::Unknown, false,
                                  SemanticBindingKind::LambdaCapture);
      captureScope.emplace(
          capture.name.lexeme,
          Symbol{.type = SemanticType::Unknown,
                 .sourceUnit = currentSourceUnit,
                 .assignable = false,
                 .declaration = capture.name,
                 .lambdaCapture = true,
                 .bindingKind = SemanticBindingKind::LambdaCapture,
                 .toolingSymbol = recoverySymbol});
      continue;
    }

    const auto *copySource = capture.explicitInitializer()
                                 ? nullptr
                                 : exprAs<Variable>(capture.initializer.get());
    const Variable *sourceExpressionForCapture =
        capture.explicitInitializer() ? sourceExpression : copySource;
    const Token &sourceName = sourceExpressionForCapture == nullptr
                                  ? capture.name
                                  : sourceExpressionForCapture->name();
    const Symbol *source = resolve(sourceName);
    if (source == nullptr ||
        (source->bindingKind != SemanticBindingKind::LocalVariable &&
         source->bindingKind != SemanticBindingKind::Parameter &&
         source->bindingKind != SemanticBindingKind::LambdaCapture) ||
        source->type == SemanticType::Function ||
        source->type.kind == SemanticType::TypeName ||
        source->ownerClass != 0) {
      report(capture.name,
             "Lambda captures must name a local value binding; '" +
                 sourceName.lexeme + "' is not capturable.",
             "GTI-S2027");
      continue;
    }

    Symbol sourceBinding = *source;
    const LambdaCaptureMode mode = capture.explicitInitializer()
                                       ? LambdaCaptureMode::Move
                                       : LambdaCaptureMode::Copy;
    const SemanticTypeTraits traits = typeTraits(source->type);
    if (containsOwningPayloadEnum(source->type)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics, capture.name,
          "Lambda capture '" + capture.name.lexeme + "' cannot " +
              (mode == LambdaCaptureMode::Move ? "move" : "copy") +
              " owning payload-enum type '" + typeSpelling(source->type) +
              "' in this local-value slice.");
      diagnostic.related.push_back(
          {tokenSpan(sourceBinding.declaration), "Captured source is here."});
      diagnostic.hints.emplace_back(
          "Keep the owning payload enum outside the lambda and observe it "
          "through a checked-reference function boundary.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (mode == LambdaCaptureMode::Copy &&
        source->type.kind == SemanticType::Reference) {
      report(capture.name,
             "Lambda capture '" + capture.name.lexeme +
                 "' is a reference; reference captures are not supported.",
             "GTI-S2027");
    }
    if (mode == LambdaCaptureMode::Copy && !traits.copyable) {
      report(capture.name,
             "Lambda capture '" + capture.name.lexeme +
                 "' is not copyable; use '[" + capture.name.lexeme +
                 " = std::move(" + sourceName.lexeme +
                 ")]' to transfer ownership explicitly.",
             "GTI-S2027");
    }
    if (mode == LambdaCaptureMode::Move && traits.containsBorrowedState) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2027", DiagnosticPhase::Semantics, capture.name,
          "Owned lambda move capture cannot contain tracked borrowed "
          "state.");
      diagnostic.related.push_back({tokenSpan(sourceBinding.declaration),
                                    "Borrowed-state source declared here."});
      diagnostic.hints.emplace_back(
          "Capture the owning value or keep this callable confined to a "
          "scope whose lifetime is already proven.");
      diagnostics.push_back(std::move(diagnostic));
      continue;
    }

    const std::size_t diagnosticsBeforeInitializer = diagnostics.size();
    const SemanticType initializerType = analyze(capture.initializer);
    if (mode == LambdaCaptureMode::Move &&
        diagnostics.size() == diagnosticsBeforeInitializer &&
        (moveCall == nullptr ||
         resolvedIntrinsicKind(*moveCall) != IntrinsicKind::Move)) {
      report(capture.equal ? *capture.equal : capture.name,
             "Owned lambda capture must use GTI's exact std::move "
             "intrinsic.",
             "GTI-S2027");
    }

    const SymbolId sourceSymbol = toolingSymbolFor(sourceBinding);
    const SymbolId bindingSymbol =
        recordBindingOccurrence(capture.name, sourceBinding.type, false,
                                SemanticBindingKind::LambdaCapture);
    semanticModel.recordLambdaCaptureMode(bindingSymbol, mode);
    captures.push_back({.capture = capture.name,
                        .declaration = sourceBinding.declaration,
                        .initializer = capture.initializer.get(),
                        .type = initializerType == SemanticType::Unknown
                                    ? sourceBinding.type
                                    : initializerType,
                        .traits = traits,
                        .mode = mode,
                        .sourceSymbol = sourceSymbol,
                        .bindingSymbol = bindingSymbol});
    captureScope.emplace(
        capture.name.lexeme,
        Symbol{.type = sourceBinding.type,
               .sourceUnit = sourceBinding.sourceUnit,
               .assignable = false,
               .declaration = capture.name,
               .lambdaCapture = true,
               .bindingKind = SemanticBindingKind::LambdaCapture,
               .toolingSymbol = bindingSymbol});
  }

  // Lambda body analysis is isolated, so transfer the enclosing stack
  // after capture initializers have executed left-to-right in that stack.
  ScopeStack enclosingScopes = std::move(scopes);
  auto enclosingRawProgramStorage = saveRawProgramStorage();

  std::unordered_map<std::string, Token> unavailableLocals;
  for (auto scope = enclosingScopes.rbegin(); scope != enclosingScopes.rend();
       ++scope) {
    for (const auto &[name, symbol] : *scope) {
      if (name == receiverStateName || capturedNames.contains(name) ||
          symbol.ownerClass != 0 || symbol.type == SemanticType::Function ||
          symbol.type.kind == SemanticType::TypeName) {
        continue;
      }
      unavailableLocals.try_emplace(name, symbol.declaration);
    }
  }

  const SemanticType enclosingReturnType = currentReturnType;
  const ReceiverMutability enclosingReceiverMutability =
      currentReceiverMutability;
  LoanFlowState enclosingLoanFlow = std::move(loanFlow);
  std::vector<ValueControlFlowContext> enclosingValueControlFlow =
      std::move(valueControlFlow);
  const std::size_t enclosingLoopDepth = loopDepth;
  auto enclosingRawPointerLoops = std::move(valueFlow.rawPointerLoops);
  const std::size_t enclosingSwitchDepth = switchDepth;
  const std::size_t enclosingConstructorDepth = constructorDepth;
  const std::size_t enclosingDestructorDepth = destructorDepth;
  const std::size_t enclosingUnsafeDepth = unsafeDepth;
  const bool enclosingAnalyzingCallCallee = analyzingCallCallee;
  const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
  const std::optional<SemanticType> enclosingInitializerType =
      contextualInitializerType;
  const Expr *enclosingInitializerExpression = contextualInitializerExpression;
  const std::optional<ContextualCallableResult> enclosingCallableResult =
      contextualCallableResult;
  scopes.clear();
  scopes.push_back(std::move(captureScope));
  currentReturnType = returnType;
  currentReceiverMutability = ReceiverMutability::ReadOnly;
  loanFlow = {};
  valueControlFlow.clear();
  analyzingCallCallee = false;
  contextualInitializerType.reset();
  contextualInitializerExpression = nullptr;
  contextualCallableResult.reset();
  loopDepth = 0;
  switchDepth = 0;
  constructorDepth = 0;
  destructorDepth = 0;
  unsafeDepth = 0;
  ++functionDepth;
  ++lambdaDepth;
  currentProgramEffectOwner = {.kind = ProgramEffectOwnerKind::Lambda,
                               .id = id};
  lambdaUncapturedLocals.push_back(std::move(unavailableLocals));

  const auto enclosingRawAccessObservation =
      requestSession.rawAccessObservation;
  requestSession.rawAccessObservation = 0;

  for (const Parameter &parameter : expr.parameters()) {
    if (!parameter.name.lexeme.empty()) {
      recordProgramCleanupEffects(typeOf(parameter), tokenSpan(parameter.name));
      declare(parameter.name, typeOf(parameter),
              parameter.mutability == Mutability::Mutable,
              SemanticBindingKind::Parameter, nullptr, &parameter);
      recordParameterEntryLoan(parameter);
    }
  }
  analyze(expr.body());
  if (returnType != SemanticType::Void && returnType != SemanticType::Unknown &&
      summarizeFlow(expr.body()).canFallThrough &&
      !loanFlow.deferredConstexprSelection) {
    report(expr.arrow(),
           "Non-void lambda can reach the end without returning a value "
           "of type '" +
               typeSpelling(returnType) + "'.",
           "GTI-S2031");
  }
  finalizeLoanFlow();

  lambdaUncapturedLocals.pop_back();
  const bool rawAccessFree = requestSession.rawAccessObservation == 0 &&
                             !loanFlow.deferredConstexprSelection;
  requestSession.rawAccessObservation = enclosingRawAccessObservation;
  --lambdaDepth;
  --functionDepth;
  destructorDepth = enclosingDestructorDepth;
  unsafeDepth = enclosingUnsafeDepth;
  constructorDepth = enclosingConstructorDepth;
  loopDepth = enclosingLoopDepth;
  switchDepth = enclosingSwitchDepth;
  contextualInitializerType = enclosingInitializerType;
  contextualInitializerExpression = enclosingInitializerExpression;
  contextualCallableResult = enclosingCallableResult;
  analyzingCallCallee = enclosingAnalyzingCallCallee;
  loanFlow = std::move(enclosingLoanFlow);
  valueControlFlow = std::move(enclosingValueControlFlow);
  valueFlow.rawPointerLoops = std::move(enclosingRawPointerLoops);
  currentReceiverMutability = enclosingReceiverMutability;
  currentReturnType = enclosingReturnType;
  currentProgramEffectOwner = enclosingEffectOwner;
  scopes = std::move(enclosingScopes);
  restoreRawProgramStorage(std::move(enclosingRawProgramStorage));

  std::vector<SemanticType> captureTypes;
  captureTypes.reserve(captures.size());
  for (const LambdaCaptureInfo &capture : captures) {
    captureTypes.emplace_back(capture.type);
  }
  std::vector<SemanticType> classTypeArguments;
  std::vector<CompileTimeValue> classValueArguments;
  if (currentClass) {
    const SemanticType owner = openClassType(*currentClass);
    classTypeArguments = owner.arguments;
    classValueArguments = owner.valueArguments;
  }
  std::vector<SemanticType> functionTypeArguments;
  std::vector<CompileTimeValue> functionValueArguments;
  if (currentFunctionDeclaration != nullptr) {
    for (const GenericParameterInfo &parameter :
         genericParametersFor(*currentFunctionDeclaration)) {
      if (parameter.value) {
        const auto concrete = instanceValueSubstitution.find(parameter.id);
        functionValueArguments.emplace_back(
            concrete == instanceValueSubstitution.end()
                ? CompileTimeValue::parameter(parameter.id)
                : concrete->second);
        continue;
      }
      const auto concrete = instanceTypeSubstitution.find(parameter.id);
      functionTypeArguments.emplace_back(
          concrete == instanceTypeSubstitution.end()
              ? SemanticType::typeParameter(parameter.id)
              : concrete->second);
    }
  }
  currentType = SemanticType::lambdaType(
      id, returnType, parameterTypes, captureTypes, classTypeArguments,
      functionTypeArguments, classValueArguments, functionValueArguments);
  semanticModel.record(
      expr,
      LambdaInfo{.id = id,
                 .declaration = &expr,
                 .returnType = returnType,
                 .parameterTypes = std::move(parameterTypes),
                 .captures = std::move(captures),
                 .traits = lambdaTraitsForCaptures(captureTypes),
                 .rawAccessFreeType =
                     rawAccessFree && !containsOpenTypeParameter(currentType)
                         ? currentType
                         : SemanticType::Unknown});
}

auto SemanticVisitor::Impl::layoutScalarKind(const SemanticType &type)
    -> std::optional<TargetScalarKind> {
  switch (type.kind) {
  case SemanticType::Bool:
    return TargetScalarKind::Bool;
  case SemanticType::Char:
    return TargetScalarKind::Char;
  case SemanticType::Int8:
    return TargetScalarKind::Int8;
  case SemanticType::Int16:
    return TargetScalarKind::Int16;
  case SemanticType::Int32:
    return TargetScalarKind::Int32;
  case SemanticType::Int64:
    return TargetScalarKind::Int64;
  case SemanticType::UInt8:
    return TargetScalarKind::UInt8;
  case SemanticType::UInt16:
    return TargetScalarKind::UInt16;
  case SemanticType::UInt32:
    return TargetScalarKind::UInt32;
  case SemanticType::UInt64:
    return TargetScalarKind::UInt64;
  case SemanticType::Float:
    return TargetScalarKind::Float32;
  case SemanticType::Double:
    return TargetScalarKind::Float64;
  case SemanticType::RawPointer:
  case SemanticType::NativeFunction:
    return TargetScalarKind::Pointer;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::evaluateLayout(const SemanticType &type) const
    -> LayoutEvaluation {
  if (const std::optional<TargetScalarKind> scalar = layoutScalarKind(type)) {
    const std::optional<TargetTypeLayout> layout =
        target.dataLayout.scalarLayout(*scalar);
    if (!layout) {
      return {.error = LayoutEvaluationError::UnsupportedType};
    }
    return {.sizeBytes = layout->sizeBytes,
            .abiAlignmentBytes = layout->abiAlignmentBytes};
  }
  if (type.kind == SemanticType::Class) {
    const ClassTypeInfo *record = semanticModel.findClassType(type.classId);
    if (record != nullptr && record->cAbiRecord && record->cAbiLayout) {
      return {.sizeBytes = record->cAbiLayout->sizeBytes,
              .abiAlignmentBytes = record->cAbiLayout->abiAlignmentBytes};
    }
    if (record != nullptr && record->kind == ClassKind::Union &&
        record->unionLayout) {
      return {.sizeBytes = record->unionLayout->sizeBytes,
              .abiAlignmentBytes = record->unionLayout->abiAlignmentBytes};
    }
    return {.error = LayoutEvaluationError::UnsupportedType};
  }
  if (type.kind == SemanticType::Enum) {
    const EnumTypeInfo *enumeration = semanticModel.findEnumType(type.enumId);
    if (enumeration == nullptr || enumeration->payload) {
      return {.error = LayoutEvaluationError::UnsupportedType};
    }
    return evaluateLayout(enumeration->underlyingType);
  }
  if (type.kind != SemanticType::Array) {
    return {.error = type.kind == SemanticType::TypeParameter
                         ? LayoutEvaluationError::SymbolicType
                         : LayoutEvaluationError::UnsupportedType};
  }
  if (type.arrayLengthParameterId != 0) {
    return {.error = LayoutEvaluationError::SymbolicExtent};
  }
  if (type.arrayLength == 0) {
    return {.error = LayoutEvaluationError::ZeroExtent};
  }
  if (type.arguments.size() != 1) {
    return {.error = LayoutEvaluationError::UnsupportedType};
  }
  const LayoutEvaluation element = evaluateLayout(type.arguments.front());
  if (!element) {
    return element;
  }
  if (element.sizeBytes >
      std::numeric_limits<std::uint64_t>::max() / type.arrayLength) {
    return {.error = LayoutEvaluationError::SizeOverflow};
  }
  return {.sizeBytes = element.sizeBytes * type.arrayLength,
          .abiAlignmentBytes = element.abiAlignmentBytes};
}

auto SemanticVisitor::Impl::lastArrayExtentToken(const ArrayExtentExpr &extent)
    -> const Token & {
  return extent.right ? lastArrayExtentToken(*extent.right) : extent.token;
}

auto SemanticVisitor::Impl::typeNameSpan(const TypeRef &type) -> SourceSpan {
  SourceSpan span = tokenSpan(type.name.first());
  const SourceSpan end = tokenSpan(type.name.last());
  if (span.source == end.source) {
    span.end = end.end;
  }
  return span;
}

auto SemanticVisitor::Impl::directLayoutExtentErrorToken(
    const TypeRef &type, LayoutEvaluationError error) const -> const Token * {
  for (const ArrayExtentExprPtr &extent : type.arrayExtents) {
    if (!extent) {
      continue;
    }
    const CompileTimeValue *value = semanticModel.findArrayExtent(*extent);
    if (error == LayoutEvaluationError::SymbolicExtent && value != nullptr &&
        value->kind == CompileTimeValue::Parameter) {
      return &lastArrayExtentToken(*extent);
    }
    if (error == LayoutEvaluationError::ZeroExtent && value != nullptr &&
        value->kind == CompileTimeValue::UInt64 && value->value == 0) {
      return &lastArrayExtentToken(*extent);
    }
  }

  if (error != LayoutEvaluationError::SizeOverflow) {
    return nullptr;
  }

  SemanticType element = baseTypeOf(type, currentNamespace);
  for (const auto &pointer : type.pointers) {
    element = SemanticType::rawPointerTo(
        std::move(element),
        pointer.pointeeConst ? AccessMode::ReadOnly : AccessMode::Mutable);
  }
  LayoutEvaluation accumulated = evaluateLayout(element);
  if (!accumulated) {
    return nullptr;
  }
  for (auto extent = type.arrayExtents.rbegin();
       extent != type.arrayExtents.rend(); ++extent) {
    if (!*extent) {
      continue;
    }
    const CompileTimeValue *value = semanticModel.findArrayExtent(**extent);
    if (value == nullptr || value->kind != CompileTimeValue::UInt64 ||
        value->value == 0) {
      continue;
    }
    if (accumulated.sizeBytes >
        std::numeric_limits<std::uint64_t>::max() / value->value) {
      return &lastArrayExtentToken(**extent);
    }
    accumulated.sizeBytes *= value->value;
  }
  return nullptr;
}

auto SemanticVisitor::Impl::layoutDiagnosticSpan(
    const TypeRef &type, LayoutEvaluationError error) const -> SourceSpan {
  if (const Token *extent = directLayoutExtentErrorToken(type, error)) {
    return tokenSpan(*extent);
  }
  if (error == LayoutEvaluationError::UnsupportedType && type.reference) {
    return tokenSpan(*type.reference);
  }
  return typeNameSpan(type);
}

auto SemanticVisitor::Impl::visitLayoutQueryExpr(const LayoutQuery &expr)
    -> void {
  currentType = SemanticType::UInt64;
  const std::size_t diagnosticsBefore = diagnostics.size();
  validateType(expr.type());
  const SemanticType queriedType = typeOf(expr.type());
  if (diagnostics.size() != diagnosticsBefore ||
      queriedType == SemanticType::Unknown) {
    return;
  }

  const LayoutEvaluation layout = evaluateLayout(queriedType);
  if (!layout) {
    if (analyzingDependentConstantCondition &&
        (layout.error == LayoutEvaluationError::SymbolicType ||
         layout.error == LayoutEvaluationError::SymbolicExtent)) {
      return;
    }
    std::string message;
    switch (layout.error) {
    case LayoutEvaluationError::SymbolicType:
      message = "Layout query '" + expr.keyword().lexeme +
                "' requires a concrete type; a direct type parameter has "
                "no single source-level layout.";
      break;
    case LayoutEvaluationError::SymbolicExtent:
      message = "Layout query '" + expr.keyword().lexeme +
                "' requires a concrete fixed-array extent.";
      break;
    case LayoutEvaluationError::ZeroExtent:
      message = "Layout query '" + expr.keyword().lexeme +
                "' is not defined for a zero-length fixed array until its "
                "object representation is specified.";
      break;
    case LayoutEvaluationError::SizeOverflow:
      message = "The fixed-array layout queried by '" + expr.keyword().lexeme +
                "' exceeds uint64_t.";
      break;
    case LayoutEvaluationError::UnsupportedType:
    case LayoutEvaluationError::None:
      message = "Type '" + typeRefSpelling(expr.type()) +
                "' has no GTI layout contract for '" + expr.keyword().lexeme +
                "'.";
      break;
    }
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2063", DiagnosticPhase::Semantics,
        layoutDiagnosticSpan(expr.type(), layout.error), std::move(message));
    diagnostic.hints.emplace_back(
        "Use a primitive scalar, one-level raw pointer, integral scoped "
        "enum, valid passive union or [[c_abi]] record, or concrete "
        "non-zero fixed array whose element layout is supported.");
    diagnostics.emplace_back(std::move(diagnostic));
    return;
  }

  const std::uint64_t value = expr.kind() == LayoutQueryKind::Size
                                  ? layout.sizeBytes
                                  : layout.abiAlignmentBytes;
  semanticModel.recordConstant(
      expr,
      ConstantValue{ConstantInteger{
          .magnitude = value, .domain = {.width = 64, .signedValue = false}}});
}

auto SemanticVisitor::Impl::recordMemberRequirementReceiver(
    const MemberRequirement &query, const SemanticType &type) -> void {
  const auto symbol = recordToolingSymbol(
      query.binding(), SymbolKind::RequirementParameter,
      qualifiedName(currentNamespace, query.binding().lexeme), type);
  semanticModel.recordOccurrence(
      {.sourceUnit = sourceUnitFor(query.binding()),
       .span = tokenSpan(query.binding()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
       .name = query.binding().lexeme,
       .type = type});
  semanticModel.recordOccurrence(
      {.sourceUnit = sourceUnitFor(query.receiverUse()),
       .span = tokenSpan(query.receiverUse()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Reference,
       .name = query.receiverUse().lexeme,
       .type = type});
  for (std::size_t index = 0; index < query.parameters().size(); ++index) {
    const auto parameterType = typeOf(query.parameters()[index]);
    const auto &binding = query.bindings()[index];
    const auto parameterSymbol = recordToolingSymbol(
        binding, SymbolKind::Parameter,
        qualifiedName(currentNamespace, binding.lexeme), parameterType);
    semanticModel.recordOccurrence(
        {.sourceUnit = sourceUnitFor(binding),
         .span = tokenSpan(binding),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = parameterSymbol,
         .roles = OccurrenceRole::Declaration | OccurrenceRole::Definition,
         .name = binding.lexeme,
         .type = parameterType});
    const auto &use = query.arguments()[index];
    semanticModel.recordOccurrence({.sourceUnit = sourceUnitFor(use),
                                    .span = tokenSpan(use),
                                    .kind = SemanticOccurrenceKind::Symbol,
                                    .symbol = parameterSymbol,
                                    .roles = OccurrenceRole::Reference,
                                    .name = use.lexeme,
                                    .type = parameterType});
  }
}

auto SemanticVisitor::Impl::visitMemberRequirementExpr(
    const MemberRequirement &expr) -> void {
  currentType = SemanticType::Bool;
  const auto before = diagnostics.size();
  validateType(expr.receiver());
  validateType(expr.result());
  recordTypeUse(expr.receiver());
  recordTypeUse(expr.result());
  const auto receiver = typeOf(expr.receiver());
  const auto result = typeOf(expr.result());
  std::vector<SemanticType> parameters;
  for (const auto &parameter : expr.parameters()) {
    validateType(parameter);
    recordTypeUse(parameter);
    parameters.push_back(typeOf(parameter));
  }
  recordMemberRequirementReceiver(expr, receiver);
  if (diagnostics.size() != before || receiver == SemanticType::Unknown ||
      result == SemanticType::Unknown)
    return;
  if (containsOpenTypeParameter(receiver) ||
      containsOpenTypeParameter(result) ||
      std::any_of(parameters.begin(), parameters.end(),
                  containsOpenTypeParameter)) {
    if (analyzingDependentConstantCondition)
      constantConditionGenericDependency = true;
    return;
  }
  const bool satisfied =
      receiver.kind == SemanticType::Reference &&
      receiver.arguments.size() == 1 &&
      readMemberRequirementSatisfied(
          receiver.arguments.front(), expr.member().lexeme, result, nullptr,
          parameters, receiver.referenceAccess == AccessMode::Mutable,
          expr.unsafeKeyword().has_value(),
          isCompilerTrustedRequester(expr.member()));
  semanticModel.recordConstant(expr, ConstantValue{satisfied});
}

auto SemanticVisitor::Impl::visitLiteralExpr(const LiteralExpr &expr) -> void {
  if (contextualOperatorIntegerType &&
      std::holds_alternative<std::uint64_t>(expr.value())) {
    currentType = *contextualOperatorIntegerType;
    return;
  }
  if (contextualInitializerExpression == &expr && contextualInitializerType &&
      isInteger(*contextualInitializerType) &&
      std::holds_alternative<std::uint64_t>(expr.value())) {
    const std::optional<IntegerConstant> constant = integerConstant(&expr);
    if (constant && integerFits(*contextualInitializerType, *constant)) {
      currentType = *contextualInitializerType;
      return;
    }
  }
  currentType = literalType(expr.value());
}

auto SemanticVisitor::Impl::visitLogicalExpr(const Logical &expr) -> void {
  const SemanticType leftType =
      analyzeExpectedCallableResult(expr.left(), SemanticType::Bool);
  const ScopeStack afterLeft = scopes;
  const LoanFlowState loanFlowAfterLeft = loanFlow;
  const std::size_t rightOwnershipStart = semanticModel.ownershipEventCount();
  ++loanFlow.rawConditionalExpressionDepth;
  const SemanticType rightType =
      analyzeExpectedCallableResult(expr.right(), SemanticType::Bool);
  --loanFlow.rawConditionalExpressionDepth;
  const ScopeStack afterRight = scopes;
  requireBool(expr.left(), leftType, expr.oper(),
              "Logical operands must be bool.");
  requireBool(expr.right(), rightType, expr.oper(),
              "Logical operands must be bool.");

  const std::optional<bool> left = constantBoolean(expr.left());
  const bool skipsRight =
      left && ((expr.oper().kind == TokenKind::AND && !*left) ||
               (expr.oper().kind == TokenKind::OR && *left));
  const bool requiresRight =
      left && ((expr.oper().kind == TokenKind::AND && *left) ||
               (expr.oper().kind == TokenKind::OR && !*left));
  if (!skipsRight) {
    validateSequencedTransientLoans(expr.left(), expr.right(), expr.oper());
  }
  ScopeStack moveScopes = afterRight;
  if (skipsRight) {
    semanticModel.markOwnershipEventsUnreachableFrom(rightOwnershipStart);
    moveScopes = afterLeft;
    loanFlow = loanFlowAfterLeft;
  } else if (requiresRight) {
    moveScopes = afterRight;
  } else {
    moveScopes = mergedValueStates(afterLeft, {afterLeft, afterRight});
  }
  // An enclosing assignment may hold a place rooted in the current scopes.
  restoreScopeState(skipsRight ? afterLeft : afterRight);
  applyMoveStates(moveScopes);
  currentType = SemanticType::Bool;
}

auto SemanticVisitor::Impl::visitPackFoldExpr(const PackFold &expr) -> void {
  const auto *call = exprAs<Call>(expr.pattern().get());
  if (call == nullptr) {
    report(expr.ellipsis(),
           "A pack fold requires one ordinary function-call pattern.",
           "GTI-S2023");
    currentType = SemanticType::Unknown;
    return;
  }

  bool valid = true;
  if (!call->typeArguments().empty()) {
    for (const TypeRef &argument : call->typeArguments()) {
      validateType(argument);
    }
    report(call->paren(),
           "A pack-fold target infers its one element type and does not "
           "take explicit generic arguments.",
           "GTI-S2023");
    valid = false;
  }
  if (call->arguments().empty()) {
    report(expr.ellipsis(),
           "A pack-fold call must contain one named parameter-pack "
           "argument.",
           "GTI-S2023");
    currentType = SemanticType::Unknown;
    return;
  }

  const bool enclosingCallCallee = analyzingCallCallee;
  analyzingCallCallee = true;
  const SemanticType calleeType = analyze(call->callee());
  analyzingCallCallee = enclosingCallCallee;
  const std::optional<Symbol> callee = resolveExpressionSymbol(call->callee());

  std::vector<SemanticType> argumentTypes;
  argumentTypes.reserve(call->arguments().size());
  const std::size_t packIndex = call->arguments().size() - 1;
  const Variable *packVariable = nullptr;
  const Symbol *packSymbol = nullptr;
  for (std::size_t index = 0; index < call->arguments().size(); ++index) {
    const ExprPtr &argument = call->arguments()[index];
    const auto *variable = exprAs<Variable>(argument.get());
    if (variable == nullptr) {
      report(expressionToken(argument),
             "Pack-fold call arguments must be named local values so the "
             "fold cannot repeat expression side effects.",
             "GTI-S2023");
      argumentTypes.emplace_back(analyze(argument));
      valid = false;
      continue;
    }
    if (index == packIndex) {
      packVariable = variable;
      packSymbol = resolve(variable->name());
      const bool enclosingPackElement = analyzingPackFoldElement;
      analyzingPackFoldElement = true;
      argumentTypes.emplace_back(analyze(argument));
      analyzingPackFoldElement = enclosingPackElement;
    } else {
      argumentTypes.emplace_back(analyze(argument));
    }
  }

  if (calleeType != SemanticType::Function || !callee ||
      callee->type != SemanticType::Function || callee->overloads.empty()) {
    if (calleeType != SemanticType::Unknown) {
      report(call->paren(),
             "A pack fold can call only one ordinary free function.",
             "GTI-S2023");
    }
    currentType = SemanticType::Unknown;
    return;
  }
  if (callee->overloads.size() != 1) {
    report(call->paren(),
           "A pack-fold target must name exactly one function declaration; "
           "overload selection is not deferred to individual elements.",
           "GTI-S2023");
    valid = false;
  }
  if (packVariable == nullptr || packSymbol == nullptr ||
      packSymbol->type.kind != SemanticType::TypePack) {
    report(expressionToken(call->arguments().back()),
           "The final pack-fold argument must name the current function's "
           "parameter pack.",
           "GTI-S2023");
    currentType = SemanticType::Unknown;
    return;
  }

  const CallableCandidate &candidate = callee->overloads.front();
  const bool exactGenericShape =
      candidate.record().genericParameters.size() == 1 &&
      !candidate.record().genericParameters.front().pack &&
      !candidate.record().genericParameters.front().value &&
      !candidate.record().parameterPack && candidate.ownerClass == 0 &&
      !candidate.staticMember &&
      candidate.record().intrinsic == IntrinsicKind::None &&
      candidate.returnType == SemanticType::Void &&
      candidate.parameterTypes.size() == argumentTypes.size();
  const SemanticType *elementParameter =
      exactGenericShape && !candidate.parameterTypes.empty()
          ? &candidate.parameterTypes.back()
          : nullptr;
  const bool exactElementShape =
      elementParameter != nullptr &&
      elementParameter->kind == SemanticType::Reference &&
      elementParameter->referenceAccess == AccessMode::ReadOnly &&
      elementParameter->arguments.size() == 1 &&
      elementParameter->arguments.front().kind == SemanticType::TypeParameter &&
      elementParameter->arguments.front().genericParameterId ==
          candidate.record().genericParameters.front().id;
  if (!exactGenericShape || !exactElementShape) {
    report(call->paren(),
           "A pack-fold target must be one non-overloaded free generic "
           "function returning void, with one inferred type parameter and "
           "a final read-only reference to that element type.",
           "GTI-S2023");
    valid = false;
  }
  if (candidate.compilerPrivate && !isCompilerTrustedRequester(call->paren())) {
    reportCompilerPrivateAccess(call->paren(), "pack-fold target");
    valid = false;
  }
  if (valid && !satisfiesConstraint(
                   packSymbol->type,
                   candidate.record().genericParameters.front().constraints)) {
    report(expr.ellipsis(),
           "The source parameter pack does not prove the constraints "
           "required by the pack-fold target.",
           "GTI-S2023");
    valid = false;
  }

  ResolvedPackFoldInfo fold{
      .pattern = call,
      .function = candidate.id,
      .declaration = candidate.declaration,
      .packSymbol = toolingSymbolFor(*packSymbol),
      .packParameter = packSymbol->type.genericParameterId,
      .packArgument = packIndex,
      .fixedArgumentTypes = std::vector<SemanticType>(
          argumentTypes.begin(),
          argumentTypes.begin() + static_cast<std::ptrdiff_t>(packIndex))};

  if (valid && packSymbol->type.concretePack) {
    fold.elements.reserve(packSymbol->type.arguments.size());
    for (const SemanticType &element : packSymbol->type.arguments) {
      std::vector<SemanticType> concreteArguments = argumentTypes;
      concreteArguments.back() = element;
      CallableCandidate resolved;
      std::vector<SemanticType> typeArguments;
      std::vector<CompileTimeValue> valueArguments;
      ConstraintFailure failure;
      if (!tryInstantiateFunction(candidate, {}, {}, concreteArguments,
                                  call->arguments(), resolved, typeArguments,
                                  valueArguments, &failure)) {
        if (hasConstraintFailure(failure)) {
          reportConstraintFailure(expr.ellipsis(), failure);
        } else {
          report(expr.ellipsis(),
                 "Pack element type '" + typeSpelling(element) +
                     "' cannot instantiate the pack-fold target.",
                 "GTI-S2023");
        }
        valid = false;
        continue;
      }
      bool argumentsMatch = true;
      for (std::size_t index = 0; index < concreteArguments.size(); ++index) {
        if (!callArgumentMatches(resolved.parameterTypes[index],
                                 concreteArguments[index],
                                 call->arguments()[index])) {
          reportCallArgumentMismatch(index, resolved.parameterTypes[index],
                                     concreteArguments[index],
                                     call->arguments()[index], "Pack fold");
          argumentsMatch = false;
        }
      }
      if (!argumentsMatch ||
          !validateSelectedFunction(
              resolved, call->callee(), call->paren(), call->arguments(),
              callReceiverMode(call->callee(), resolved)) ||
          !validateConfinedCallableArguments(
              candidate, resolved, concreteArguments, call->arguments())) {
        valid = false;
        continue;
      }
      fold.elements.push_back(
          {.elementType = element,
           .call = ResolvedCallInfo{.function = resolved.id,
                                    .declaration = resolved.declaration,
                                    .returnType = resolved.returnType,
                                    .parameterTypes = resolved.parameterTypes,
                                    .typeArguments = std::move(typeArguments),
                                    .valueArguments = std::move(valueArguments),
                                    .requirements = resolved.requirements,
                                    .borrowOrigin = resolved.returnBorrowOrigin,
                                    .borrowArgument =
                                        resolved.returnBorrowParameter,
                                    .borrowAccess = resolved.returnBorrowAccess,
                                    .borrowPlace = resolved.returnBorrowPlace,
                                    .dispatch = CallDispatch::Static,
                                    .dispatchOwner = resolved.dispatchOwner}});
    }
  }

  if (valid) {
    // The fixed prefix's mutable-reference access is independent of the pack
    // element type. Preserve that source use even in primary analysis, where
    // no concrete element calls exist yet.
    for (std::size_t index = 0; index < packIndex; ++index) {
      const SemanticType &parameter = candidate.parameterTypes[index];
      if (parameter.kind == SemanticType::Reference &&
          parameter.referenceAccess == AccessMode::Mutable) {
        recordMutableBindingUse(call->arguments()[index]);
      }
    }
    semanticModel.record(expr, std::move(fold));
    ResolvedCallInfo symbolic{.function = candidate.id,
                              .declaration = candidate.declaration,
                              .returnType = candidate.returnType,
                              .parameterTypes = candidate.parameterTypes,
                              .requirements = candidate.requirements,
                              .borrowOrigin = candidate.returnBorrowOrigin,
                              .borrowArgument = candidate.returnBorrowParameter,
                              .borrowAccess = candidate.returnBorrowAccess,
                              .borrowPlace = candidate.returnBorrowPlace,
                              .dispatch = CallDispatch::Static,
                              .dispatchOwner = candidate.dispatchOwner};
    semanticModel.record(*call, symbolic);
    recordSelectedCallOccurrence(*call, std::move(symbolic));
  }
  currentType = valid ? SemanticType::Void : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::visitPackExpansionExpr(const PackExpansion &expr)
    -> void {
  const Symbol *symbol = resolve(expr.name());
  if (symbol == nullptr) {
    report(expr.name(),
           "Undefined parameter pack '" + expr.name().lexeme + "'.",
           "GTI-S2001");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!isCompilerTrustedRequester(expr.name()) && symbol->compilerPrivate) {
    reportCompilerPrivateAccess(expr.name(), expr.name().lexeme);
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->type.kind != SemanticType::TypePack) {
    report(expr.ellipsis(),
           "'" + expr.name().lexeme + "' is not a parameter pack.",
           "GTI-S2023");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->valueState != ValueState::Available) {
    reportUnavailableValue(expr.name(), *symbol);
  } else if (packRequiresMove(symbol->type)) {
    if (Symbol *pack = resolveMutable(expr.name())) {
      pack->valueState = ValueState::Moved;
      pack->moveOrigins = {tokenSpan(expr.name())};
    }
  }
  currentType = symbol->type;
}

auto SemanticVisitor::Impl::visitPostfixExpr(const Postfix &expr) -> void {
  const SemanticType type = analyze(
      expr.expression(),
      OccurrenceRole::Reference | OccurrenceRole::Read | OccurrenceRole::Write);
  if (type.kind == SemanticType::RawPointer ||
      type.kind == SemanticType::StringView) {
    if (rejectCOpaquePointeeOperation(type, expr.oper(),
                                      "raw-pointer arithmetic")) {
      currentType = SemanticType::Unknown;
      return;
    }
    const bool validPointee = type.arguments.size() == 1 &&
                              type.arguments.front() != SemanticType::Void;
    const bool mutableTarget = isMutableTarget(expr.expression());
    if (!validPointee) {
      report(expr.oper(), "Arithmetic on void* is not supported.", "GTI-S2056");
    }
    if (validPointee && mutableTarget) {
      requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                    "Raw pointer arithmetic");
    }
  } else if (type != SemanticType::Unknown && !isNumeric(type)) {
    report(expr.oper(), "Increment and decrement require a numeric value.");
  }
  if (!isMutableTarget(expr.expression())) {
    report(expr.oper(),
           type.kind == SemanticType::RawPointer
               ? "Increment and decrement require a reseatable "
                 "'mut' raw pointer binding."
               : "Increment and decrement require an "
                 "assignable value.",
           type.kind == SemanticType::RawPointer ? "GTI-S2002" : "GTI-S2000");
  }
  currentType = type;
}

auto SemanticVisitor::Impl::visitQualifiedNameExpr(const QualifiedName &expr)
    -> void {
  if (reportCompilerPrivatePath(expr.name())) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (expr.name().last().completion) {
    captureQualifiedCompletion(expr.name(),
                               expr.ownerType() ? &*expr.ownerType() : nullptr);
    currentType = SemanticType::Unknown;
    return;
  }
  recordQualifiedPathUses(expr.name());
  if (!validateClassPathAliases(expr.name(), expr.name().last())) {
    currentType = SemanticType::Unknown;
    return;
  }
  if (expr.name().segments.size() > 1) {
    NamePath ownerPath(std::vector<Token>(expr.name().segments.begin(),
                                          expr.name().segments.end() - 1));
    if (const auto owner = resolveClassPath(ownerPath, currentNamespace);
        owner && !validateNestedClassAccess(*owner, expr.name().last())) {
      currentType = SemanticType::Unknown;
      return;
    }
  }
  if (expr.ownerType()) {
    validateType(*expr.ownerType());
    recordTypeUse(*expr.ownerType());
  }
  const auto concreteEnumerator = resolveEnumerator(expr);
  const Symbol *symbol = concreteEnumerator ? &concreteEnumerator->symbol
                                            : resolveQualified(expr.name());
  if (symbol && !concreteEnumerator &&
      symbol->type.kind == SemanticType::Enum && symbol->type.enumId != 0 &&
      symbol->type.enumId <= enums.size() &&
      !enums[symbol->type.enumId - 1].genericParameters.empty()) {
    report(expr.name().last(),
           "A generic payload variant requires a concrete enum owner type.",
           "GTI-S2067");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol == nullptr) {
    if (expr.name().segments.size() >= 2) {
      const NamePath ownerPath(std::vector<Token>(
          expr.name().segments.begin(), expr.name().segments.end() - 1));
      if (const std::optional<ClassId> classId =
              resolveClassPath(ownerPath, currentNamespace)) {
        const ClassInfo &owner = classInfo(*classId);
        const auto member = owner.members.find(expr.name().last().lexeme);
        if (member != owner.members.end() &&
            !member->second.symbol.staticMember) {
          report(expr.name().last(),
                 "Instance member '" + expr.name().last().lexeme +
                     "' requires an object.",
                 "GTI-S2039");
        } else {
          report(expr.name().last(),
                 "Unknown static member '" + expr.name().last().lexeme +
                     "' on '" + owner.name.lexeme + "'.",
                 "GTI-S2039");
        }
        currentType = SemanticType::Unknown;
        return;
      }
      if (resolveEnumPath(ownerPath, currentNamespace)) {
        report(expr.name().last(),
               "Unknown enumerator '" + expr.name().last().lexeme +
                   "' in scoped enum '" + pathSpelling(ownerPath) + "'.",
               "GTI-S2036");
        currentType = SemanticType::Unknown;
        return;
      }
    }
    if (!reportInvisibleSymbol(expr.name().last(), pathSpelling(expr.name()),
                               resolveQualifiedGlobally(expr.name()))) {
      report(expr.name().last(),
             "Undefined qualified name '" + pathSpelling(expr.name()) + "'.");
      diagnostics.back().missingIncludeName = expr.name().last().lexeme;
    }
    currentType = SemanticType::Unknown;
    return;
  }
  if (!isCompilerTrustedRequester(expr.name().last()) &&
      symbol->compilerPrivate) {
    reportCompilerPrivateAccess(expr.name().last(), pathSpelling(expr.name()));
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->ownerClass != 0 && symbol->type != SemanticType::Function &&
      symbol->access == AccessModifier::Private &&
      !isWithinClass(symbol->ownerClass)) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, expr.name().last(),
        "Static member '" + expr.name().last().lexeme + "' of '" +
            classInfo(symbol->ownerClass).name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(symbol->declaration), "Static member declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (const auto &enumerator = concreteEnumerator) {
    const SemanticType &type = enumerator->symbol.type;
    if (type.enumId != 0 && type.enumId <= enums.size() &&
        !enums[type.enumId - 1].genericParameters.empty() &&
        type.arguments.empty() && type.valueArguments.empty()) {
      report(expr.name().last(),
             "A generic payload variant requires a concrete enum owner type.",
             "GTI-S2067");
      currentType = SemanticType::Unknown;
      return;
    }
    semanticModel.record(
        expr, ResolvedEnumeratorInfo{.owner = type.enumId,
                                     .type = type,
                                     .declaration = enumerator->declaration,
                                     .value = enumerator->value,
                                     .variantIndex = enumerator->variantIndex});
    if (type.enumId != 0 && type.enumId <= enums.size() &&
        enums[type.enumId - 1].payload && !enumerator->payloadTypes.empty() &&
        !allowPayloadEnumeratorReference) {
      report(expr.name().last(),
             "Payload variant '" + expr.name().last().lexeme + "' requires " +
                 std::to_string(enumerator->payloadTypes.size()) +
                 " constructor argument" +
                 (enumerator->payloadTypes.size() == 1 ? "." : "s."),
             "GTI-S2067");
    }
  }
  if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
    if (contextualInitializerType &&
        contextualInitializerType->kind == SemanticType::NativeFunction) {
      currentType = analyzeNativeFunctionItem(
          expr, *symbol, *contextualInitializerType, expr.name().last());
      return;
    }
    if (contextualInitializerType &&
        contextualInitializerType->kind == SemanticType::FunctionItem) {
      currentType = analyzeFunctionItem(
          expr, *symbol, *contextualInitializerType, expr.name().last());
      return;
    }
    currentType = analyzeFunctionItem(expr, *symbol, SemanticType::Unknown,
                                      expr.name().last());
    return;
  }
  currentType = symbol->type;
}

auto SemanticVisitor::Impl::visitThisExpr(const This &expr) -> void {
  if (analyzingDefaultArgument) {
    report(expr.keyword(),
           "A default argument is declaration-scoped and cannot use "
           "'this'.",
           "GTI-S2077");
    currentType = SemanticType::Unknown;
    return;
  }
  if (analyzingConstructorInitializer) {
    report(expr.keyword(),
           "Cannot use 'this' in a constructor initializer expression.");
    currentType = SemanticType::Unknown;
    return;
  }
  if (lambdaDepth > 0) {
    report(expr.keyword(),
           "Lambdas cannot capture 'this' yet; class borrows require an "
           "explicit lifetime design.",
           "GTI-S2027");
    currentType = SemanticType::Unknown;
    return;
  }
  if (!currentClass || functionDepth == 0) {
    report(expr.keyword(),
           "Cannot use 'this' outside a class or struct method.");
    currentType = SemanticType::Unknown;
    return;
  }
  if (currentStaticMemberFunction) {
    report(expr.keyword(), "Static methods do not have a 'this' object.",
           "GTI-S2039");
    currentType = SemanticType::Unknown;
    return;
  }
  currentType = openClassType(*currentClass);
}

auto SemanticVisitor::Impl::visitSetExpr(const Set &expr) -> void {
  SemanticType objectType = analyzeProjectionBase(expr.object());
  const SemanticType receiverType = objectType;
  const bool simpleAssignment = expr.oper().kind == TokenKind::EQUAL;
  bool mutableReceiver = isMutableObject(expr.object());
  if (expr.access().kind == TokenKind::ARROW) {
    objectType = arrowTargetType(expr, expr.object(), objectType, expr.access(),
                                 !simpleAssignment);
    if (objectType == SemanticType::Unknown) {
      analyze(expr.value());
      currentType = SemanticType::Unknown;
      return;
    }
    if (const ResolvedOperatorInfo *resolved =
            simpleAssignment ? semanticModel.findOperator(expr)
                             : semanticModel.findProjectionOperator(expr)) {
      mutableReceiver =
          resolved->returnType.kind == SemanticType::Reference &&
          resolved->returnType.referenceAccess == AccessMode::Mutable;
    } else if (receiverType.kind == SemanticType::RawPointer) {
      mutableReceiver = receiverType.pointerAccess == AccessMode::Mutable;
    }
  }

  const MemberInfo *member = resolveMember(objectType, expr.name());
  if (member == nullptr) {
    analyze(expr.value());
    currentType = SemanticType::Unknown;
    return;
  }
  if (objectType.kind == SemanticType::Class && objectType.classId != 0 &&
      objectType.classId <= classes.size() &&
      classInfo(objectType.classId).kind == ClassKind::Union) {
    requireUnsafe(expr, UnsafeOperationKind::UnionMember, expr.name(),
                  "Union member assignment");
  }
  if (member->symbol.staticMember) {
    analyze(expr.value());
    report(expr.name(),
           "Static member '" + expr.name().lexeme +
               "' must be accessed through its class or struct name.",
           "GTI-S2039");
    currentType = SemanticType::Unknown;
    return;
  }
  const Symbol resolvedMember = substituteSymbol(member->symbol, objectType);
  const SemanticType targetType =
      resolvedMember.type.kind == SemanticType::Reference &&
              resolvedMember.type.arguments.size() == 1
          ? resolvedMember.type.arguments.front()
          : resolvedMember.type;
  SemanticPlace targetPlace = semanticPlace(expr);
  const std::optional<OverloadedOperator> mutationOperator =
      compoundOperator(expr.oper().kind);
  const bool overloadedMutation = !simpleAssignment && mutationOperator &&
                                  targetType.kind == SemanticType::Class;
  bool targetStateValid = true;
  if (!simpleAssignment) {
    if (const ProjectedValueState *state =
            unavailableProjectedState(targetPlace)) {
      reportUnavailablePlace(expr.name(), *state, &targetPlace);
      targetStateValid = false;
    }
  } else if (const ProjectedValueState *state =
                 unavailableAncestorState(targetPlace)) {
    reportUnavailablePlace(expr.name(), *state, &targetPlace);
    targetStateValid = false;
  }
  const SemanticType valueType =
      simpleAssignment ? analyzeInitializer(expr.value(), targetType)
                       : analyze(expr.value());
  if (simpleAssignment &&
      (targetType.kind == SemanticType::StringView ||
       targetType.kind == SemanticType::CString) &&
      isOwnerDependentTextExpression(expr.value())) {
    reportOwnerDependentTextStorage(expr.value(), "stored in a field");
  }
  const bool directSelfMove =
      samePlace(targetPlace, movedSourcePlace(expr.value()));
  if (directSelfMove) {
    report(expr.name(), "Cannot move a place into itself.", "GTI-S2018");
  }
  if (resolvedMember.type == SemanticType::Function) {
    report(expr.name(), "Methods are not assignable.");
  } else if (!resolvedMember.assignable) {
    report(expr.name(), "Member is immutable.");
    diagnostics.back().related.push_back({tokenSpan(resolvedMember.declaration),
                                          "Immutable field declared here."});
    diagnostics.back().hints.emplace_back(
        resolvedMember.constant
            ? "A constant cannot be mutated; use separate mutable storage if a "
              "changing value is needed."
        : resolvedMember.staticMember &&
                target.executionProfile == ExecutionProfile::Concurrent
            ? "Concurrent static fields must stay immutable. Use local "
              "mutable storage or an immutable value with an approved atomic "
              "or mutex contract for synchronized mutation."
            : "Declare the field with 'mut' if it is intended to change after "
              "construction; access through its containing value must also be "
              "mutable.");
  } else if (!mutableReceiver) {
    const std::optional<ResolvedFieldUse> root = receiverRestrictedMemberRoot(
        expr.object(), expr.name(), member->symbol);
    if (!root || !reportReceiverRestrictedMutation(expr.name(), *root)) {
      report(expr.name(), "Cannot mutate through a read-only receiver.");
      appendReadOnlyPlaceContext(diagnostics.back(), expr.object());
    }
  } else if (!overloadedMutation) {
    reportLoanPlaceConflict(
        expr.name(), targetPlace, AccessMode::Mutable,
        "Cannot assign to a field while an overlapping borrow may still be "
        "live.");
  }
  if (simpleAssignment &&
      !isOwnershipAssignment(targetType, valueType, expr.value())) {
    report(expressionToken(expr.value()),
           "Cannot assign a value of type '" + typeSpelling(valueType) +
               "' to member '" + expr.name().lexeme + "' of type '" +
               typeSpelling(targetType) + "'.",
           "GTI-S2003");
  }
  if (overloadedMutation && resolvedMember.assignable && mutableReceiver) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *mutationOperator, expr.object(), targetType, expr.oper(),
        std::span<const SemanticType>(&valueType, 1),
        std::span<const ExprPtr>(&expr.value(), 1), false, std::nullopt,
        OperatorReceiverState{.place = targetPlace, .mutableReceiver = true});
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }
  if (!simpleAssignment && !overloadedMutation) {
    if (targetType.kind == SemanticType::RawPointer &&
        (expr.oper().kind == TokenKind::PLUS_EQUAL ||
         expr.oper().kind == TokenKind::MINUS_EQUAL) &&
        isInteger(valueType) && targetType.arguments.size() == 1 &&
        targetType.arguments.front() != SemanticType::Void &&
        !isCOpaqueHandleType(targetType.arguments.front()) &&
        resolvedMember.assignable && mutableReceiver) {
      if (expr.access().kind != TokenKind::ARROW ||
          receiverType.kind != SemanticType::RawPointer) {
        requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                      "Raw pointer arithmetic");
      }
    }
    validateCompoundAssignment(expr.oper(), targetType, valueType,
                               expr.value().get());
  }
  if (simpleAssignment && resolvedMember.type != SemanticType::Function &&
      resolvedMember.assignable && mutableReceiver && targetStateValid &&
      !directSelfMove &&
      isOwnershipAssignment(targetType, valueType, expr.value())) {
    if (targetType.kind == SemanticType::Class ||
        targetType.kind == SemanticType::Array)
      invalidateRawPointerOrigins(targetPlace);
    assignRawPointerFields(targetPlace, expr.value().get());
    if (targetPlace.root != nullptr) {
      semanticModel.recordOwnershipEvent(
          expr, {.kind = OwnershipEventKind::Reinitialize,
                 .place = placeKey(targetPlace),
                 .before = projectedValueState(*targetPlace.root,
                                               targetPlace.projections),
                 .after = ValueState::Available});
    }
    reinitializePlace(targetPlace);
  }
  currentType = targetType;
}

auto SemanticVisitor::Impl::visitUnaryExpr(const Unary &expr) -> void {
  if (expr.oper().kind == TokenKind::QUESTION) {
    const SemanticType operandType = analyze(expr.right());
    if (operandType == SemanticType::Unknown) {
      currentType = SemanticType::Unknown;
      return;
    }
    if (operandType.kind != SemanticType::Expected ||
        operandType.arguments.size() != 2) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2086", DiagnosticPhase::Semantics, expr.oper(),
          "Error propagation requires an 'expected<T, E>' operand, but "
          "received '" +
              typeSpelling(operandType) + "'.");
      diagnostic.hints.emplace_back(
          "Apply '?' to a fallible result, or handle this value without "
          "error propagation.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }
    if (currentReturnType.kind != SemanticType::Expected ||
        currentReturnType.arguments.size() != 2) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2086", DiagnosticPhase::Semantics, expr.oper(),
          "Error propagation is only available in a function or lambda "
          "returning 'expected<U, E>'.");
      diagnostic.hints.emplace_back(
          "Return an expected type from this callable, or inspect "
          "has_value(), value(), and error() explicitly.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }

    const SemanticType &valueType = operandType.arguments[0];
    const SemanticType &errorType = operandType.arguments[1];
    const SemanticType &returnErrorType = currentReturnType.arguments[1];
    if (errorType != returnErrorType) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2086", DiagnosticPhase::Semantics, expr.oper(),
          "Error propagation requires the exact enclosing error type '" +
              typeSpelling(returnErrorType) + "', but the operand uses '" +
              typeSpelling(errorType) + "'.");
      diagnostic.hints.emplace_back(
          "Convert the error explicitly before applying '?'; propagation "
          "does not perform implicit error conversion.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }

    const ExpressionInfo *operandInfo =
        semanticModel.findExpression(*expr.right());
    const bool moves =
        operandInfo != nullptr && operandInfo->category == ValueCategory::Value;
    const auto transferable = [&](const SemanticType &type) {
      if (type == SemanticType::Void) {
        return true;
      }
      const SemanticTypeTraits traits = typeTraits(type);
      return moves ? traits.movable : traits.copyable;
    };
    const bool borrowedSuccess = isFallibleBorrowType(operandType);
    if ((!borrowedSuccess && !transferable(valueType)) ||
        !transferable(errorType)) {
      const SemanticType &blocked =
          !borrowedSuccess && !transferable(valueType) ? valueType : errorType;
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2086", DiagnosticPhase::Semantics, expr.oper(),
          std::string("Error propagation cannot ") + (moves ? "move" : "copy") +
              " the active '" + typeSpelling(blocked) + "' alternative.");
      diagnostic.hints.emplace_back(
          moves ? "Use a result whose value and error alternatives are "
                  "movable."
                : "Apply '?' to std::move(result) to consume a named "
                  "move-only result explicitly.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }

    currentType = borrowedSuccess ? valueType.arguments.front() : valueType;
    return;
  }
  if (contextualOperatorIntegerType &&
      (expr.oper().kind == TokenKind::PLUS ||
       expr.oper().kind == TokenKind::MINUS) &&
      contextualIntegerLiteral(&expr) != nullptr) {
    const SemanticType target = *contextualOperatorIntegerType;
    semanticModel.recordContextualIntegerOperand(expr);
    contextualOperatorIntegerType.reset();
    (void)analyze(expr.right());
    contextualOperatorIntegerType = target;
    currentType = target;
    return;
  }
  if (expr.oper().kind == TokenKind::MINUS) {
    if (const auto *literal = exprAs<LiteralExpr>(expr.right().get());
        literal != nullptr) {
      const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
      if (magnitude != nullptr && *magnitude == (std::uint64_t{1} << 63U)) {
        const std::optional<SemanticType> enclosing =
            contextualOperatorIntegerType;
        contextualOperatorIntegerType = SemanticType::Int64;
        semanticModel.recordContextualIntegerOperand(expr);
        (void)analyze(expr.right());
        contextualOperatorIntegerType = enclosing;
        currentType = SemanticType::Int64;
        return;
      }
    }
  }
  if (expr.oper().kind == TokenKind::STAR && expr.rangeObject()) {
    const auto *rangeVariable = exprAs<Variable>(expr.rangeObject().get());
    const Symbol *rangeSymbol =
        rangeVariable == nullptr ? nullptr : resolve(rangeVariable->name());
    SemanticType rangeType =
        rangeSymbol == nullptr ? SemanticType::Unknown : rangeSymbol->type;
    if (rangeType.kind == SemanticType::Reference &&
        rangeType.arguments.size() == 1) {
      SemanticType referent = rangeType.arguments.front();
      rangeType = std::move(referent);
    }
    if (rangeType.kind == SemanticType::Array) {
      (void)analyzeProjectionBase(expr.rangeObject());
      const SemanticType indexType = analyze(expr.right());
      semanticModel.recordPlaceSelection(expr, nextPlaceSelectionId++);
      currentType = analyzeArrayIndexAfterOperands(rangeType, expr.right(),
                                                   indexType, expr.oper());
      return;
    }
  }
  const bool mutating = expr.oper().kind == TokenKind::PLUS_PLUS ||
                        expr.oper().kind == TokenKind::MINUS_MINUS;
  if (expr.oper().kind == TokenKind::AMPERSAND) {
    ++programStoragePlaceUseDepth;
  }
  const SemanticType rightType =
      mutating ? analyze(expr.right(), OccurrenceRole::Reference |
                                           OccurrenceRole::Read |
                                           OccurrenceRole::Write)
               : (expr.oper().kind == TokenKind::STAR
                      ? analyzeProjectionBase(expr.right())
                  : expr.oper().kind == TokenKind::BANG
                      ? analyzeExpectedCallableResult(expr.right(),
                                                      SemanticType::Bool)
                      : analyze(expr.right()));
  if (expr.oper().kind == TokenKind::AMPERSAND) {
    --programStoragePlaceUseDepth;
  }

  if (expr.oper().kind == TokenKind::AMPERSAND) {
    const ExpressionInfo *info = semanticModel.findExpression(*expr.right());
    if (info == nullptr || info->category != ValueCategory::Place ||
        rightType == SemanticType::Function ||
        rightType.kind == SemanticType::TypeName) {
      report(expr.oper(),
             "Raw address-of requires an addressable place, not a "
             "temporary or function.",
             "GTI-S2056");
      currentType = SemanticType::Unknown;
      return;
    }
    if (rightType.kind == SemanticType::Array) {
      report(expr.oper(),
             "Fixed arrays do not decay to raw pointers; take the address "
             "of an element such as '&values[0]'.",
             "GTI-S2056");
      currentType = SemanticType::Unknown;
      return;
    }
    SemanticType pointee = rightType;
    if (pointee.kind == SemanticType::Reference &&
        pointee.arguments.size() == 1) {
      SemanticType referent = pointee.arguments.front();
      pointee = std::move(referent);
    }
    SemanticType addressType = SemanticType::rawPointerTo(
        std::move(pointee), info->access == AccessMode::Mutable
                                ? AccessMode::Mutable
                                : AccessMode::ReadOnly);
    if (info->access == AccessMode::Mutable) {
      recordMutableBindingUse(expr.right());
    }
    // Forming an inert address does not assert pointee validity. Operand
    // evaluation above retains its own access, bounds and unsafe checks.
    currentType = std::move(addressType);
    return;
  }

  if ((expr.oper().kind == TokenKind::PLUS_PLUS ||
       expr.oper().kind == TokenKind::MINUS_MINUS) &&
      rightType.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(rightType, expr.oper(),
                                      "raw-pointer arithmetic")) {
      currentType = SemanticType::Unknown;
      return;
    }
    const bool validPointee = rightType.arguments.size() == 1 &&
                              rightType.arguments.front() != SemanticType::Void;
    const bool mutableTarget = isMutableTarget(expr.right());
    if (!validPointee) {
      report(expr.oper(), "Arithmetic on void* is not supported.", "GTI-S2056");
    }
    if (!mutableTarget) {
      report(expr.oper(),
             "Increment and decrement require a reseatable 'mut' raw "
             "pointer binding.",
             "GTI-S2002");
    }
    if (validPointee && mutableTarget) {
      requireUnsafe(expr, UnsafeOperationKind::PointerArithmetic, expr.oper(),
                    "Raw pointer arithmetic");
    }
    currentType = rightType;
    return;
  }

  const std::optional<OverloadedOperator> mutationOperator =
      expr.oper().kind == TokenKind::PLUS_PLUS
          ? std::optional<OverloadedOperator>{OverloadedOperator::PreIncrement}
      : expr.oper().kind == TokenKind::MINUS_MINUS
          ? std::optional<OverloadedOperator>{OverloadedOperator::PreDecrement}
          : std::nullopt;
  if (mutationOperator &&
      (rightType.kind == SemanticType::Class ||
       (rightType.kind == SemanticType::TypeParameter &&
        hasSymbolicOperatorRequirement(*mutationOperator, rightType)))) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *mutationOperator, expr.right(), rightType, expr.oper());
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }

  if (expr.oper().kind == TokenKind::STAR) {
    if (rightType.kind == SemanticType::RawPointer) {
      if (rejectCOpaquePointeeOperation(rightType, expr.oper(),
                                        "raw-pointer dereference")) {
        currentType = SemanticType::Unknown;
        return;
      }
      if (rightType.arguments.size() != 1 ||
          rightType.arguments.front() == SemanticType::Void) {
        report(expr.oper(), "void* cannot be dereferenced.", "GTI-S2056");
        currentType = SemanticType::Unknown;
      } else {
        requireUnsafe(expr, UnsafeOperationKind::RawDereference, expr.oper(),
                      "Raw pointer dereference");
        validateRawPointerAccess(expr.right(), expr.oper(),
                                 AccessMode::ReadOnly);
        currentType = rightType.arguments.front();
      }
    } else if (rightType.kind == SemanticType::Class ||
               (rightType.kind == SemanticType::TypeParameter &&
                hasSymbolicOperatorRequirement(OverloadedOperator::Dereference,
                                               rightType))) {
      const std::optional<CallableCandidate> selected = resolveOperator(
          expr, OverloadedOperator::Dereference, expr.right(), rightType,
          expr.oper(), {}, {}, false, std::nullopt, std::nullopt, false,
          preferReadOnlySubscriptReceiverDepth != 0 &&
              !(contextualInitializerType &&
                contextualInitializerType->kind == SemanticType::Reference &&
                contextualInitializerType->referenceAccess ==
                    AccessMode::Mutable));
      currentType = selected ? callExpressionType(selected->returnType)
                             : SemanticType::Unknown;
    } else {
      report(expr.oper(), "Dereference requires a class defining operator*.",
             "GTI-S2022");
      currentType = SemanticType::Unknown;
    }
    return;
  }

  const std::optional<OverloadedOperator> valueOperator =
      unaryValueOperator(expr.oper().kind);
  if (valueOperator &&
      (rightType.kind == SemanticType::Class ||
       (rightType.kind == SemanticType::TypeParameter &&
        hasSymbolicOperatorRequirement(*valueOperator, rightType)))) {
    const std::optional<CallableCandidate> selected = resolveOperator(
        expr, *valueOperator, expr.right(), rightType, expr.oper());
    currentType = selected ? callExpressionType(selected->returnType)
                           : SemanticType::Unknown;
    return;
  }

  if (expr.oper().kind == TokenKind::BANG) {
    requireBool(expr.right(), rightType, expr.oper(),
                "Logical negation requires bool.");
    currentType = SemanticType::Bool;
    return;
  }

  if (expr.oper().kind == TokenKind::TILDE) {
    if (rightType != SemanticType::Unknown && !isIntegral(rightType)) {
      report(expr.oper(), "Bitwise complement requires an integer operand.");
      currentType = SemanticType::Unknown;
    } else {
      currentType = promotedInteger(rightType);
    }
    return;
  }

  if (rightType != SemanticType::Unknown &&
      (expr.oper().kind == TokenKind::MINUS
           ? !isSignedNumeric(rightType) && !isUnsignedInteger(rightType)
           : !isNumeric(rightType))) {
    report(expr.oper(), expr.oper().kind == TokenKind::MINUS
                            ? "Unary '-' requires a signed numeric value."
                            : "Unary operator requires a numeric value.");
  }
  if ((expr.oper().kind == TokenKind::PLUS_PLUS ||
       expr.oper().kind == TokenKind::MINUS_MINUS) &&
      !isMutableTarget(expr.right())) {
    report(expr.oper(), "Increment and decrement require an assignable value.");
  }
  if ((expr.oper().kind == TokenKind::PLUS ||
       expr.oper().kind == TokenKind::MINUS) &&
      (rightType == SemanticType::Int8 || rightType == SemanticType::Int16 ||
       rightType == SemanticType::UInt8 || rightType == SemanticType::UInt16)) {
    currentType = SemanticType::Int32;
    return;
  }
  if (expr.oper().kind == TokenKind::MINUS && isUnsignedInteger(rightType)) {
    report(expr.oper(), "Unary '-' cannot be applied to an unsigned integer.");
    currentType = SemanticType::Unknown;
    return;
  }
  currentType = rightType;
}

auto SemanticVisitor::Impl::visitUnexpectedExpr(const Unexpected &expr)
    -> void {
  const SemanticType errorType = analyze(expr.error());
  if (!isOwnershipAssignable(errorType, errorType, expr.error())) {
    const ExpressionInfo *payload = semanticModel.findExpression(*expr.error());
    const bool moving =
        payload != nullptr && payload->category == ValueCategory::Value;
    const auto *call = exprAs<Call>(expr.error().get());
    const Token token = call != nullptr ? callableToken(call->callee())
                                        : expressionToken(expr.error());
    report(token,
           "Cannot " + std::string(moving ? "move" : "copy") +
               " error payload of type '" + typeSpelling(errorType) +
               "' into unexpected: that construction is unavailable.",
           "GTI-S2003");
    if (!moving && typeTraits(errorType).movable) {
      diagnostics.back().hints.emplace_back(
          "Transfer the error with explicit std::move, or clone it when the "
          "source must be preserved. For an expected observer, move the "
          "carrier before calling error().");
    }
    currentType = SemanticType::Unknown;
    return;
  }
  currentType = SemanticType(SemanticType::Unexpected, {errorType});
}

auto SemanticVisitor::Impl::visitVariableExpr(const Variable &expr) -> void {
  if (expr.isReferencePattern()) {
    report(expr.name(),
           "A borrowed payload binding is only valid directly in a "
           "payload-enum case pattern.",
           "GTI-S2067");
    currentType = SemanticType::Unknown;
    return;
  }
  if (expr.name().completion) {
    captureUnqualifiedCompletion(expr.name());
    currentType = SemanticType::Unknown;
    return;
  }
  const Symbol *symbol = resolve(expr.name());
  if (symbol == nullptr) {
    if (analyzingDefaultArgument &&
        currentDefaultParameterNames.contains(expr.name().lexeme)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2077", DiagnosticPhase::Semantics, expr.name(),
          "A default argument cannot reference a parameter from its "
          "declaration.");
      diagnostic.hints.emplace_back(
          "Move parameter-dependent computation into the function body or "
          "supply the value explicitly at the call site.");
      diagnostics.emplace_back(std::move(diagnostic));
      currentType = SemanticType::Unknown;
      return;
    }
    if (const std::optional<CompileTimeValue> parameter =
            resolveValueParameter(expr.name())) {
      if (analyzingDependentConstantCondition &&
          parameter->kind == CompileTimeValue::Parameter) {
        constantConditionGenericDependency = true;
      }
      currentType = SemanticType::UInt64;
      return;
    }
    if (!reportMissingLambdaCapture(expr.name()) &&
        !reportInvisibleSymbol(expr.name(), expr.name().lexeme,
                               resolveGlobally(expr.name()))) {
      report(expr.name(), "Undefined name '" + expr.name().lexeme + "'.",
             "GTI-S2001");
      diagnostics.back().missingIncludeName = expr.name().lexeme;
    }
    currentType = SemanticType::Unknown;
    return;
  }
  if (!isCompilerTrustedRequester(expr.name()) && symbol->compilerPrivate) {
    reportCompilerPrivateAccess(expr.name(), expr.name().lexeme);
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->ownerClass != 0 && !symbol->staticMember &&
      currentClass != symbol->ownerClass && isWithinClass(symbol->ownerClass) &&
      !isSameOrDerivedClass(openClassType(*currentClass),
                            openClassType(symbol->ownerClass))) {
    report(expr.name(),
           "Outer instance member '" + expr.name().lexeme +
               "' requires an explicit enclosing object; a nested type "
               "does not capture its enclosing instance.",
           "GTI-S2039");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->type == SemanticType::Function && !analyzingCallCallee) {
    if (contextualInitializerType &&
        contextualInitializerType->kind == SemanticType::NativeFunction) {
      currentType = analyzeNativeFunctionItem(
          expr, *symbol, *contextualInitializerType, expr.name());
      return;
    }
    if (contextualInitializerType &&
        contextualInitializerType->kind == SemanticType::FunctionItem) {
      currentType = analyzeFunctionItem(
          expr, *symbol, *contextualInitializerType, expr.name());
      return;
    }
    currentType =
        analyzeFunctionItem(expr, *symbol, SemanticType::Unknown, expr.name());
    return;
  }
  if (symbol->type.kind == SemanticType::TypePack &&
      !analyzingPackFoldElement) {
    report(expr.name(),
           "Parameter pack '" + expr.name().lexeme +
               "' must be expanded as the final call argument with '...'.",
           "GTI-S2023");
  }
  if (symbol->ownerClass != 0 &&
      (analyzingFieldInitializer || analyzingConstructorInitializer)) {
    report(expr.name(), analyzingConstructorInitializer
                            ? "Class and struct members cannot be referenced "
                              "from constructor initializer expressions."
                            : "Class and struct members cannot be referenced "
                              "from field initializers yet.");
  }
  if (symbol->ownerClass != 0 && symbol->access == AccessModifier::Private &&
      !isWithinClass(symbol->ownerClass)) {
    const ClassInfo &declaringOwner = classInfo(symbol->ownerClass);
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, expr.name(),
                       "Member '" + expr.name().lexeme + "' of '" +
                           declaringOwner.name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(symbol->declaration), "Member declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  if (currentStaticMemberFunction && symbol->ownerClass != 0 &&
      !symbol->staticMember) {
    report(expr.name(),
           analyzingDefaultArgument
               ? "A default argument cannot access instance member '" +
                     expr.name().lexeme + "'."
               : "Static methods cannot access instance member '" +
                     expr.name().lexeme + "' without an object.",
           analyzingDefaultArgument ? "GTI-S2077" : "GTI-S2039");
    currentType = SemanticType::Unknown;
    return;
  }
  if (symbol->valueState != ValueState::Available) {
    reportUnavailableValue(expr.name(), *symbol);
  }
  currentType = symbol->type.kind == SemanticType::Reference &&
                        symbol->type.arguments.size() == 1
                    ? symbol->type.arguments[0]
                    : symbol->type;
}

auto SemanticVisitor::Impl::binaryOperator(TokenKind kind)
    -> std::optional<OverloadedOperator> {
  switch (kind) {
  case TokenKind::PLUS:
    return OverloadedOperator::Plus;
  case TokenKind::MINUS:
    return OverloadedOperator::Minus;
  case TokenKind::STAR:
    return OverloadedOperator::Multiply;
  case TokenKind::SLASH:
    return OverloadedOperator::Divide;
  case TokenKind::PERCENT:
    return OverloadedOperator::Remainder;
  case TokenKind::AMPERSAND:
    return OverloadedOperator::BitwiseAnd;
  case TokenKind::PIPE:
    return OverloadedOperator::BitwiseOr;
  case TokenKind::CARET:
    return OverloadedOperator::BitwiseXor;
  case TokenKind::SHIFT_LEFT:
    return OverloadedOperator::ShiftLeft;
  case TokenKind::SHIFT_RIGHT:
    return OverloadedOperator::ShiftRight;
  case TokenKind::EQUAL_EQUAL:
    return OverloadedOperator::Equal;
  case TokenKind::BANG_EQUAL:
    return OverloadedOperator::NotEqual;
  case TokenKind::LESS:
    return OverloadedOperator::Less;
  case TokenKind::LESS_EQUAL:
    return OverloadedOperator::LessEqual;
  case TokenKind::GREATER:
    return OverloadedOperator::Greater;
  case TokenKind::GREATER_EQUAL:
    return OverloadedOperator::GreaterEqual;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::compoundOperator(TokenKind kind)
    -> std::optional<OverloadedOperator> {
  switch (kind) {
  case TokenKind::PLUS_EQUAL:
    return OverloadedOperator::AddAssign;
  case TokenKind::MINUS_EQUAL:
    return OverloadedOperator::SubtractAssign;
  case TokenKind::STAR_EQUAL:
    return OverloadedOperator::MultiplyAssign;
  case TokenKind::SLASH_EQUAL:
    return OverloadedOperator::DivideAssign;
  case TokenKind::PERCENT_EQUAL:
    return OverloadedOperator::RemainderAssign;
  case TokenKind::AMPERSAND_EQUAL:
    return OverloadedOperator::BitwiseAndAssign;
  case TokenKind::PIPE_EQUAL:
    return OverloadedOperator::BitwiseOrAssign;
  case TokenKind::CARET_EQUAL:
    return OverloadedOperator::BitwiseXorAssign;
  case TokenKind::SHIFT_LEFT_EQUAL:
    return OverloadedOperator::ShiftLeftAssign;
  case TokenKind::SHIFT_RIGHT_EQUAL:
    return OverloadedOperator::ShiftRightAssign;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::unaryValueOperator(TokenKind kind)
    -> std::optional<OverloadedOperator> {
  switch (kind) {
  case TokenKind::PLUS:
    return OverloadedOperator::Plus;
  case TokenKind::MINUS:
    return OverloadedOperator::Minus;
  case TokenKind::TILDE:
    return OverloadedOperator::BitwiseNot;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::isCompoundOperator(OverloadedOperator kind)
    -> bool {
  switch (kind) {
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

auto SemanticVisitor::Impl::isArithmeticOrBitwiseValueOperator(
    OverloadedOperator kind) -> bool {
  switch (kind) {
  case OverloadedOperator::Plus:
  case OverloadedOperator::Minus:
  case OverloadedOperator::Multiply:
  case OverloadedOperator::Divide:
  case OverloadedOperator::Remainder:
  case OverloadedOperator::BitwiseNot:
  case OverloadedOperator::BitwiseAnd:
  case OverloadedOperator::BitwiseOr:
  case OverloadedOperator::BitwiseXor:
  case OverloadedOperator::ShiftLeft:
  case OverloadedOperator::ShiftRight:
    return true;
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::requiredParameterCount(
    std::span<const Parameter> parameters) -> std::size_t {
  const auto firstDefault = std::find_if(
      parameters.begin(), parameters.end(),
      [](const Parameter &parameter) { return parameter.hasDefault(); });
  return static_cast<std::size_t>(
      std::distance(parameters.begin(), firstDefault));
}

auto SemanticVisitor::Impl::omittedDefaultArguments(
    std::span<const Parameter> parameters, std::size_t suppliedArguments)
    -> std::vector<const Expr *> {
  std::vector<const Expr *> result;
  if (suppliedArguments >= parameters.size()) {
    return result;
  }
  result.reserve(parameters.size() - suppliedArguments);
  for (std::size_t index = suppliedArguments; index < parameters.size();
       ++index) {
    const Parameter &parameter = parameters[index];
    if (!parameter.defaultArgument || !parameter.defaultArgument->expression) {
      return {};
    }
    result.push_back(parameter.defaultArgument->expression.get());
  }
  return result;
}

auto SemanticVisitor::Impl::allocateDefaultArgumentEffectOwners(
    std::span<const Parameter> parameters) -> std::vector<std::size_t> {
  std::vector<std::size_t> result(parameters.size(), 0);
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (parameters[index].hasDefault()) {
      result[index] = nextDefaultArgumentEffectOwner++;
    }
  }
  return result;
}

auto SemanticVisitor::Impl::recordDefaultArgumentDependencies(
    std::span<const Expr *const> defaults) -> void {
  if (currentDefaultArgumentRoot == nullptr) {
    return;
  }
  std::vector<const Expr *> &dependencies =
      defaultArgumentDependencies[currentDefaultArgumentRoot];
  for (const Expr *dependency : defaults) {
    if (dependency != nullptr &&
        std::find(dependencies.begin(), dependencies.end(), dependency) ==
            dependencies.end()) {
      dependencies.push_back(dependency);
    }
  }
}

auto SemanticVisitor::Impl::validateDefaultArgumentCycles() -> void {
  enum class VisitState { Unvisited, Active, Complete };
  std::unordered_map<const Expr *, VisitState> states;
  std::vector<const Expr *> stack;
  const auto visit = [&](const Expr *root, const auto &self) -> void {
    states[root] = VisitState::Active;
    stack.push_back(root);
    const auto dependencies = defaultArgumentDependencies.find(root);
    if (dependencies != defaultArgumentDependencies.end()) {
      for (const Expr *dependency : dependencies->second) {
        if (!defaultArgumentSyntax.contains(dependency)) {
          continue;
        }
        const VisitState state = states[dependency];
        if (state == VisitState::Unvisited) {
          self(dependency, self);
          continue;
        }
        if (state != VisitState::Active) {
          continue;
        }
        const auto first = std::find(stack.begin(), stack.end(), dependency);
        if (first == stack.end() ||
            std::any_of(first, stack.end(), [&](const Expr *node) {
              return reportedDefaultArgumentCycles.contains(node);
            })) {
          continue;
        }
        std::vector<const Expr *> cycle(first, stack.end());
        for (const Expr *node : cycle) {
          reportedDefaultArgumentCycles.insert(node);
        }
        const ParameterDefault *primary =
            defaultArgumentSyntax.at(cycle.front());
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2077", DiagnosticPhase::Semantics, primary->equal,
            "Default arguments form a recursive expansion cycle.");
        for (std::size_t index = 1; index < cycle.size(); ++index) {
          const ParameterDefault *syntax =
              defaultArgumentSyntax.at(cycle[index]);
          diagnostic.related.push_back(
              {tokenSpan(syntax->equal),
               "This default argument participates in the cycle."});
        }
        diagnostic.hints.emplace_back(
            "Break the cycle by supplying an argument in the nested call "
            "or by moving the shared computation into a non-defaulted "
            "helper.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
    stack.pop_back();
    states[root] = VisitState::Complete;
  };
  for (const auto &[root, _] : defaultArgumentSyntax) {
    if (states[root] == VisitState::Unvisited) {
      visit(root, visit);
    }
  }
}

auto SemanticVisitor::Impl::analyzeParameterDefaults(
    std::span<const Parameter> parameters,
    std::span<const SemanticType> parameterTypes,
    std::span<const std::size_t> effectOwners,
    DefaultArgumentOwnerKind ownerKind, bool defaultsPermitted,
    std::string_view unsupportedContext, bool hasParameterPack,
    bool validateDeclarationRules) -> void {
  const std::size_t firstDefault = requiredParameterCount(parameters);
  const Parameter *firstDefaultParameter =
      firstDefault < parameters.size() ? &parameters[firstDefault] : nullptr;
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const Parameter &parameter = parameters[index];
    if (!parameter.hasDefault()) {
      if (validateDeclarationRules && firstDefaultParameter != nullptr &&
          index > firstDefault) {
        const Token &location = parameter.name.lexeme.empty()
                                    ? parameter.type.name.last()
                                    : parameter.name;
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2077", DiagnosticPhase::Semantics, location,
            "A required parameter cannot follow a parameter with a "
            "default argument.");
        diagnostic.related.push_back(
            {tokenSpan(firstDefaultParameter->defaultArgument->equal),
             "The default-argument suffix begins here."});
        diagnostic.hints.emplace_back(
            "Move required parameters before every defaulted parameter or "
            "give this parameter a default argument.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
      continue;
    }

    const ParameterDefault &syntax = *parameter.defaultArgument;
    if (validateDeclarationRules) {
      defaultArgumentSyntax.insert_or_assign(syntax.expression.get(), &syntax);
    }
    if (validateDeclarationRules && !defaultsPermitted) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2077", DiagnosticPhase::Semantics, syntax.equal,
                         "Default arguments are not supported on " +
                             std::string(unsupportedContext) + ".");
      diagnostic.hints.emplace_back(
          "Use an ordinary GTI function, non-polymorphic method, or "
          "ordinary constructor and supply any boundary arguments "
          "explicitly.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (validateDeclarationRules && (hasParameterPack || parameter.pack)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2077", DiagnosticPhase::Semantics, syntax.equal,
          "Default arguments cannot be combined with a parameter pack.");
      diagnostic.hints.emplace_back(
          "Keep the variadic declaration explicit, or use a fixed trailing "
          "parameter list for defaults.");
      diagnostics.emplace_back(std::move(diagnostic));
    }

    const SemanticType parameterType = index < parameterTypes.size()
                                           ? parameterTypes[index]
                                           : SemanticType::Unknown;
    if (parameterType.kind == SemanticType::Reference ||
        parameterType.kind == SemanticType::RawPointer ||
        parameterType == SemanticType::CString ||
        (parameterType != SemanticType::Unknown &&
         parameterType.kind != SemanticType::TypeParameter &&
         typeTraits(parameterType).containsBorrowedState)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2077", DiagnosticPhase::Semantics, syntax.equal,
          "A default argument must initialize an owned or plain value; "
          "reference, raw-pointer, C-string, and borrowed-state parameters "
          "remain explicit.");
      diagnostic.hints.emplace_back(
          "Pass the borrowed boundary explicitly so its origin and lifetime "
          "remain visible at the call site.");
      diagnostics.emplace_back(std::move(diagnostic));
    }

    const ProgramEffectOwner enclosingEffectOwner = currentProgramEffectOwner;
    if (validateDeclarationRules && index < effectOwners.size() &&
        effectOwners[index] != 0) {
      currentProgramEffectOwner = {
          .kind = ownerKind == DefaultArgumentOwnerKind::Function
                      ? ProgramEffectOwnerKind::FunctionDefaultArgument
                      : ProgramEffectOwnerKind::ConstructorDefaultArgument,
          .id = effectOwners[index]};
    }
    const bool enclosingDefaultArgument = analyzingDefaultArgument;
    const Expr *enclosingDefaultArgumentRoot = currentDefaultArgumentRoot;
    std::unordered_set<std::string> enclosingDefaultParameterNames =
        std::move(currentDefaultParameterNames);
    const bool enclosingStaticMemberFunction = currentStaticMemberFunction;
    analyzingDefaultArgument = true;
    currentDefaultArgumentRoot = syntax.expression.get();
    currentDefaultParameterNames.clear();
    for (const Parameter &candidate : parameters) {
      if (!candidate.name.lexeme.empty()) {
        currentDefaultParameterNames.insert(candidate.name.lexeme);
      }
    }
    // Defaults resolve in declaration scope and never receive an implicit
    // object. Static members remain available through the class scope.
    currentStaticMemberFunction = true;
    const SemanticType actual =
        analyzeInitializer(syntax.expression, parameterType);
    currentStaticMemberFunction = enclosingStaticMemberFunction;
    currentDefaultParameterNames = std::move(enclosingDefaultParameterNames);
    currentDefaultArgumentRoot = enclosingDefaultArgumentRoot;
    analyzingDefaultArgument = enclosingDefaultArgument;
    currentProgramEffectOwner = enclosingEffectOwner;

    if (parameterType != SemanticType::Unknown &&
        parameterType.kind != SemanticType::TypeParameter &&
        parameterType.kind != SemanticType::TypePack &&
        actual != SemanticType::Unknown &&
        !callArgumentMatches(parameterType, actual, syntax.expression)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2077", DiagnosticPhase::Semantics,
          expressionToken(syntax.expression),
          "Default argument for parameter " + std::to_string(index + 1) +
              " has type '" + typeSpelling(actual) +
              "' but the parameter requires '" + typeSpelling(parameterType) +
              "'.");
      diagnostic.related.push_back({tokenSpan(parameter.type.name.last()),
                                    "The parameter type is declared here."});
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
}

auto SemanticVisitor::Impl::makeConstexprInterpreter() const
    -> ConstexprInterpreter {
  ConstexprInterpreter context;
  context.work = requestSession.constexprWork
                     ? requestSession.constexprWork
                     : std::make_shared<ConstexprWorkBudget>();
  if (!instanceTypeSubstitution.empty() || !instanceValueSubstitution.empty()) {
    context.substitutions.push_back({.types = instanceTypeSubstitution,
                                     .values = instanceValueSubstitution});
  }
  return context;
}

auto SemanticVisitor::Impl::constexprType(
    SemanticType type, const ConstexprInterpreter &context) const
    -> SemanticType {
  for (const GenericSubstitution &substitution : context.substitutions) {
    type = substituteType(type, substitution);
  }
  return type;
}

auto SemanticVisitor::Impl::constexprValue(
    CompileTimeValue value, const ConstexprInterpreter &context) const
    -> CompileTimeValue {
  if (value.kind != CompileTimeValue::Parameter) {
    return value;
  }
  for (auto substitution = context.substitutions.rbegin();
       substitution != context.substitutions.rend(); ++substitution) {
    const auto found = substitution->values.find(value.parameterId);
    if (found != substitution->values.end()) {
      value = found->second;
      if (value.kind != CompileTimeValue::Parameter) {
        break;
      }
    }
  }
  return value;
}

auto SemanticVisitor::Impl::isSupportedConstexprType(
    const SemanticType &type, bool allowDependent,
    std::vector<SemanticType> *visiting, bool transient) const -> bool {
  if (allowDependent && type.kind == SemanticType::TypeParameter) {
    return true;
  }
  // An execution-local owner is not a persistable constant. Signature checks
  // admit it; the verified executor must destroy every allocation before
  // publishing the outer request's passive result.
  if (transient && type.kind == SemanticType::UniqueOwner)
    return type.arguments.size() == 1 &&
           isSupportedConstexprType(type.arguments[0], allowDependent, visiting,
                                    true);
  if (transient && type.kind == SemanticType::TypePack)
    return (!type.concretePack && allowDependent) ||
           (type.concretePack &&
            std::all_of(type.arguments.begin(), type.arguments.end(),
                        [&](const auto &element) {
                          return isSupportedConstexprType(
                              element, allowDependent, visiting, true);
                        }));
  if (type.kind == SemanticType::Expected && type.arguments.size() == 2) {
    return (type.arguments[0] == SemanticType::Void ||
            isSupportedConstexprType(type.arguments[0], allowDependent,
                                     visiting, transient)) &&
           isSupportedConstexprType(type.arguments[1], allowDependent, visiting,
                                    transient);
  }
  if (type.kind == SemanticType::Unexpected && type.arguments.size() == 1) {
    return isSupportedConstexprType(type.arguments[0], allowDependent, visiting,
                                    transient);
  }
  if (constantIntegerDomain(type).has_value() ||
      semanticFloatFormat(type).has_value() || type == SemanticType::Bool ||
      type == SemanticType::Char || type == SemanticType::StringView ||
      type == SemanticType::NullPtr) {
    return true;
  }
  if (type.kind == SemanticType::Enum) {
    const EnumTypeInfo *enumeration = semanticModel.findEnumType(type.enumId);
    return enumeration != nullptr && !enumeration->payload;
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1) {
    if (!allowDependent && type.arrayLengthParameterId != 0) {
      return false;
    }
    return isSupportedConstexprType(type.arguments.front(), allowDependent,
                                    visiting, transient);
  }
  if (type.kind != SemanticType::Class) {
    return false;
  }
  const ClassInfo *owner = classInfo(type);
  if (owner == nullptr || owner->kind == ClassKind::Interface ||
      owner->kind == ClassKind::Union || !owner->bases.empty() ||
      owner->polymorphic || owner->storedReference ||
      (!transient && owner->destructor)) {
    return false;
  }
  std::vector<SemanticType> localVisiting;
  if (visiting == nullptr) {
    visiting = &localVisiting;
  }
  if (std::find(visiting->begin(), visiting->end(), type) != visiting->end()) {
    return false;
  }
  visiting->push_back(type);
  const GenericSubstitution substitution = classSubstitution(type);
  for (const FieldInfo &field : owner->fields) {
    if (field.declaration == nullptr) {
      visiting->pop_back();
      return false;
    }
    const auto member = owner->members.find(field.declaration->name().lexeme);
    if (member == owner->members.end() ||
        !isSupportedConstexprType(
            substituteType(member->second.symbol.type, substitution),
            allowDependent, visiting, transient)) {
      visiting->pop_back();
      return false;
    }
  }
  visiting->pop_back();
  return true;
}

auto SemanticVisitor::Impl::validateConstexprFunction(
    const FunctionDecl &function, bool methodDeclaration,
    bool intrinsicDeclaration, const SemanticType &returnType, bool entryPoint)
    -> bool {
  if (!function.isConstexpr()) {
    return true;
  }

  bool valid = true;
  const auto reject = [&](const Token &location, std::string message,
                          std::string hint = {}) {
    report(location, std::move(message), "GTI-S2057");
    if (!hint.empty()) {
      diagnostics.back().hints.emplace_back(std::move(hint));
    }
    valid = false;
  };
  const auto *functionInfo = semanticModel.findFunction(function);
  const auto *definition =
      functionInfo == nullptr ? &function : functionInfo->bodyDeclaration();
  const bool staticMethod =
      functionInfo != nullptr && functionInfo->staticMember;
  if (!definition->body()) {
    reject(function.name(),
           "A constexpr function requires a body at its declaration.");
  }
  if (entryPoint) {
    reject(*function.constexprKeyword(),
           "The main entry point cannot be constexpr.");
  }
  // Concrete pack expansion is owned by ordinary semantic/HIR instantiation.
  // Constant execution sees only the resulting exact parameter sequence.
  if (methodDeclaration && !staticMethod &&
      function.receiverMutability() == ReceiverMutability::Consuming) {
    reject(function.operatorName() ? function.operatorName()->keyword
                                   : function.name(),
           "A constexpr instance method cannot consume its receiver.");
  }
  // The symbolic declaration must describe a potentially constant-capable
  // signature. Concrete generic substitutions can still be runtime-only:
  // constexpr does not forbid using an owning T at runtime. Required constant
  // calls independently check their exact types and executed operations.
  // Syntax, receiver, linkage and ordinary ownership checks are never deferred.
  if (methodDeclaration && !staticMethod && currentClass &&
      !instanceAnalysisActive &&
      !isSupportedConstexprType(openClassType(*currentClass), true, nullptr,
                                true)) {
    reject(function.operatorName() ? function.operatorName()->keyword
                                   : function.name(),
           "A constexpr instance method requires a structural aggregate "
           "receiver with no base, polymorphism, stored reference, union "
           "state, or unsupported field.");
  }
  if (function.runtimeBinding() || intrinsicDeclaration ||
      function.hasCLinkage()) {
    reject(function.name(),
           "Runtime, intrinsic, and extern C functions cannot be declared "
           "constexpr.");
  }
  if (function.isVirtual() || function.isOverride() || function.isPure()) {
    const Token &location =
        function.isVirtual()
            ? *function.virtualKeyword()
            : (function.isOverride() ? *function.overrideKeyword()
                                     : function.pureSpecifier()->equal);
    reject(location, "Polymorphic functions cannot participate in constexpr "
                     "execution.");
  }
  const auto observedType =
      [](const SemanticType &type) -> const SemanticType & {
    return type.kind == SemanticType::Reference && type.arguments.size() == 1
               ? type.arguments.front()
               : type;
  };
  if ((function.returnMutability() == Mutability::Mutable &&
       returnType.kind != SemanticType::Reference) ||
      (!instanceAnalysisActive && returnType != SemanticType::Unknown &&
       returnType != SemanticType::Void &&
       !isSupportedConstexprType(observedType(returnType), true, nullptr,
                                 true))) {
    reject(function.returnType().name.last(),
           "A constexpr function must return void or a supported value: a "
           "fixed-width integer, float, double, bool, char, string_view, "
           "nullptr_t, scoped enum, fixed array, structural aggregate, or "
           "expected result with supported alternatives, or a checked "
           "reference to a "
           "supported value.");
  }
  for (const Parameter &parameter : function.parameters()) {
    const SemanticType parameterType = typeOf(parameter);
    if (!instanceAnalysisActive && parameterType != SemanticType::Unknown &&
        !isSupportedConstexprType(observedType(parameterType), true, nullptr,
                                  true)) {
      reject(parameter.sourceAnchor(),
             "Constexpr function parameters must be supported "
             "values or checked references to supported values.");
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::validateConstexprConstructor(
    const ConstructorDecl &constructor, const ConstructorInfo *info,
    const SemanticType &ownerType) -> bool {
  if (!constructor.isConstexpr()) {
    return true;
  }
  bool valid = true;
  const auto reject = [&](const Token &location, std::string message) {
    report(location, std::move(message), "GTI-S2057");
    valid = false;
  };
  if (info == nullptr || info->kind != ConstructorKind::Ordinary ||
      constructor.specifier() || !constructor.body()) {
    reject(constructor.name(),
           "A constexpr constructor must be an ordinary constructor with "
           "a source body.");
  }
  if (!instanceAnalysisActive &&
      !isSupportedConstexprType(ownerType, true, nullptr, true)) {
    reject(*constructor.constexprKeyword(),
           "A constexpr constructor requires a structural class or struct "
           "with supported fields and no base, polymorphism, union state, "
           "stored reference, or unsupported field.");
  }
  for (const Parameter &parameter : constructor.parameters()) {
    const SemanticType parameterType = typeOf(parameter);
    const SemanticType &observed =
        parameterType.kind == SemanticType::Reference &&
                parameterType.arguments.size() == 1
            ? parameterType.arguments[0]
            : parameterType;
    if (!instanceAnalysisActive && parameterType != SemanticType::Unknown &&
        !isSupportedConstexprType(observed, true, nullptr, true)) {
      reject(parameter.name.lexeme.empty() ? parameter.type.name.last()
                                           : parameter.name,
             "Constexpr constructor parameters must be supported "
             "values or checked references to supported values.");
    }
  }
  return valid;
}

auto SemanticVisitor::Impl::constexprSuccess(const Expr &expression,
                                             ConstantValue value,
                                             ConstexprInterpreter &context)
    -> ConstexprEvaluation {
  // Function-body expressions can produce a different value per invocation.
  // Only cache constants belonging to the source expression being evaluated
  // outside a call frame.
  if (context.callDepth == 0) {
    semanticModel.recordConstant(expression, value);
  }
  return {.value = std::move(value), .location = expressionToken(expression)};
}

auto SemanticVisitor::Impl::constexprFailure(const Token &location,
                                             ConstantEvaluationFailure failure,
                                             std::string detail)
    -> ConstexprEvaluation {
  return {
      .failure = failure, .location = location, .detail = std::move(detail)};
}

auto SemanticVisitor::Impl::constexprStatementFailure(
    ConstexprEvaluation failure) -> ConstexprStatementEvaluation {
  return {.control = ConstexprControlFlow::Failure,
          .failure = std::move(failure)};
}

auto SemanticVisitor::Impl::findConstexprLocal(
    const ConstexprInterpreter &context, SymbolId symbol)
    -> const ConstexprLocalValue * {
  for (auto scope = context.scopes.rbegin(); scope != context.scopes.rend();
       ++scope) {
    if (const auto found = scope->find(symbol); found != scope->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::findMutableConstexprLocal(
    ConstexprInterpreter &context, SymbolId symbol) -> ConstexprLocalValue * {
  for (auto scope = context.scopes.rbegin(); scope != context.scopes.rend();
       ++scope) {
    if (const auto found = scope->find(symbol); found != scope->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::constexprCompoundOperator(TokenKind operation)
    -> std::optional<TokenKind> {
  switch (operation) {
  case TokenKind::PLUS_EQUAL:
    return TokenKind::PLUS;
  case TokenKind::MINUS_EQUAL:
    return TokenKind::MINUS;
  case TokenKind::STAR_EQUAL:
    return TokenKind::STAR;
  case TokenKind::SLASH_EQUAL:
    return TokenKind::SLASH;
  case TokenKind::PERCENT_EQUAL:
    return TokenKind::PERCENT;
  case TokenKind::AMPERSAND_EQUAL:
    return TokenKind::AMPERSAND;
  case TokenKind::PIPE_EQUAL:
    return TokenKind::PIPE;
  case TokenKind::CARET_EQUAL:
    return TokenKind::CARET;
  case TokenKind::SHIFT_LEFT_EQUAL:
    return TokenKind::SHIFT_LEFT;
  case TokenKind::SHIFT_RIGHT_EQUAL:
    return TokenKind::SHIFT_RIGHT;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::evaluateConstexprMutation(
    const Expr &source, const ExprPtr &target, const Token &operation,
    bool postfix, ConstexprInterpreter &context) -> ConstexprEvaluation {
  const SymbolId symbol =
      target ? semanticModel.findResolvedSymbol(*target) : SymbolId{0};
  ConstexprLocalValue *local = findMutableConstexprLocal(context, symbol);
  if (local == nullptr) {
    return constexprFailure(
        operation, ConstantEvaluationFailure::NonConstantReference,
        "A constexpr function can only mutate its local bindings and "
        "parameters.");
  }
  if (!*local) {
    return constexprFailure(
        operation, ConstantEvaluationFailure::NonConstantReference,
        "The increment or decrement reads an uninitialized local binding.");
  }
  const auto *integer = std::get_if<ConstantInteger>(&**local);
  if (integer == nullptr && !std::holds_alternative<BinaryFloat>(**local)) {
    return constexprFailure(operation,
                            ConstantEvaluationFailure::InvalidOperands);
  }
  const ConstantValue previous = **local;
  const auto *floating = std::get_if<BinaryFloat>(&previous);
  const ConstantValue one = ConstantValue{makeConstantInteger(
      {.magnitude = 1},
      integer == nullptr
          ? CheckedIntegerDomain{.width = 32, .signedValue = true}
          : integer->domain)};
  const ConstantEvaluation evaluated = evaluateConstantBinary(
      operation.kind == TokenKind::PLUS_PLUS ? TokenKind::PLUS
                                             : TokenKind::MINUS,
      previous, one,
      integer == nullptr ? std::optional<CheckedIntegerDomain>{}
                         : std::optional<CheckedIntegerDomain>{integer->domain},
      floating == nullptr ? std::optional<BinaryFloatFormat>{}
                          : std::optional<BinaryFloatFormat>{floating->format});
  if (!evaluated) {
    return constexprFailure(operation, evaluated.failure);
  }
  **local = *evaluated.value;
  return constexprSuccess(source, postfix ? previous : **local, context);
}

auto SemanticVisitor::Impl::defaultConstexprValue(
    const SemanticType &type, ConstexprInterpreter &context) const
    -> ConstantEvaluation {
  if (context.work->steps++ >= constexprExpressionStepLimit) {
    return {.failure = ConstantEvaluationFailure::ResourceLimit};
  }
  if (type.kind == SemanticType::Expected && type.arguments.size() == 2) {
    if (type.arguments[0] == SemanticType::Void)
      return makeConstantResult(type, true);
    const auto value = defaultConstexprValue(type.arguments[0], context);
    return value ? makeConstantResult(type, true, value.value) : value;
  }
  if (const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(type)) {
    return {.value = ConstantValue{makeConstantInteger({}, *domain)}};
  }
  if (const std::optional<BinaryFloatFormat> format =
          semanticFloatFormat(type)) {
    return convertConstantFloat(
        ConstantValue{makeConstantInteger(
            {}, CheckedIntegerDomain{.width = 32, .signedValue = true})},
        *format);
  }
  if (type == SemanticType::Bool) {
    return {.value = ConstantValue{false}};
  }
  if (type == SemanticType::Char) {
    return {.value = ConstantValue{CharacterLiteral{}}};
  }
  if (type == SemanticType::StringView) {
    return {.value = ConstantValue{std::string{}}};
  }
  if (type == SemanticType::NullPtr) {
    return {.value = ConstantValue{NullConstant{}}};
  }
  if (type.kind == SemanticType::Array && type.arguments.size() == 1 &&
      type.arrayLengthParameterId == 0) {
    ConstantAggregate aggregate{.kind = ConstantAggregateKind::Array,
                                .exactType =
                                    std::make_shared<const SemanticType>(type)};
    aggregate.elements.reserve(type.arrayLength);
    for (std::uint64_t index = 0; index < type.arrayLength; ++index) {
      ConstantEvaluation element =
          defaultConstexprValue(type.arguments.front(), context);
      if (!element) {
        return element;
      }
      aggregate.elements.push_back(*element.value);
    }
    return {.value =
                ConstantValue{makeConstantAggregate(std::move(aggregate))}};
  }
  return {.failure = ConstantEvaluationFailure::UnsupportedType};
}

auto SemanticVisitor::Impl::seedConstexprValueParameters(
    const std::vector<GenericParameterInfo> &parameters,
    const GenericSubstitution &substitution,
    std::unordered_map<SymbolId, ConstexprLocalValue> &scope) -> void {
  for (const GenericParameterInfo &parameter : parameters) {
    if (!parameter.value) {
      continue;
    }
    const auto found = substitution.values.find(parameter.id);
    if (found == substitution.values.end() ||
        found->second.kind != CompileTimeValue::UInt64) {
      continue;
    }
    const SymbolId symbol = symbolForDeclaration(parameter.name);
    if (symbol != 0) {
      scope.insert_or_assign(symbol,
                             ConstantValue{ConstantInteger{
                                 .magnitude = found->second.value,
                                 .domain = CheckedIntegerDomain{.width = 64}}});
    }
  }
}

auto SemanticVisitor::Impl::evaluateConstexprConstruction(
    const Expr &source, const ResolvedConstructionInfo &resolved,
    const ExprList &explicitArguments, const Token &callSite,
    ConstexprInterpreter &context) -> ConstexprEvaluation {
  if (resolved.directInitialization) {
    if (explicitArguments.size() != 1)
      return constexprFailure(callSite,
                              ConstantEvaluationFailure::InvalidOperands);
    // No execution rule is added here: the already-supported producer (which
    // may stage verified MIR) alone determines the value and consumes budget.
    return evaluateConstexprExpression(explicitArguments.front(), context);
  }
  const ConstructorDecl *declaration = resolved.declaration;
  if (resolved.generatedDefault) {
    auto concrete = resolved;
    concrete.constructedType = constexprType(resolved.constructedType, context);
    if (!explicitArguments.empty() ||
        !isSupportedConstexprType(concrete.constructedType))
      return constexprFailure(callSite,
                              ConstantEvaluationFailure::UnsupportedType);
    if (auto staged = executeStagedConstexpr(
            concrete, {}, classSubstitution(concrete.constructedType), {},
            callSite, context))
      return *staged ? constexprSuccess(source, *staged->value, context)
                     : std::move(*staged);
    return constexprFailure(callSite,
                            ConstantEvaluationFailure::UnsupportedExpression,
                            "Generated constant construction requires an "
                            "available verified MIR field schedule.");
  }
  if (declaration == nullptr || !declaration->isConstexpr() ||
      !constexprConstructorsAnalyzed.contains(declaration)) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The selected constructor is not an available constexpr "
        "constructor.");
  }
  if (context.callDepth >= constexprCallDepthLimit) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::ResourceLimit,
        "The constexpr call depth exceeded the compiler limit of " +
            std::to_string(constexprCallDepthLimit) + ".");
  }

  const SemanticType constructedType =
      constexprType(resolved.constructedType, context);
  if (!isSupportedConstexprType(constructedType) ||
      constructedType.kind != SemanticType::Class) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedType,
        "The selected constexpr constructor does not produce a supported "
        "structural aggregate.");
  }
  const ClassInfo *owner = classInfo(constructedType);
  const ConstructorInfo *constructor =
      semanticModel.findConstructor(*declaration);
  if (owner == nullptr || constructor == nullptr ||
      constructor->kind != ConstructorKind::Ordinary) {
    return constexprFailure(callSite,
                            ConstantEvaluationFailure::UnsupportedType);
  }

  std::vector<const Expr *> argumentExpressions;
  argumentExpressions.reserve(resolved.parameterTypes.size());
  for (const ExprPtr &argument : explicitArguments) {
    argumentExpressions.push_back(argument.get());
  }
  argumentExpressions.insert(argumentExpressions.end(),
                             resolved.defaultArguments.begin(),
                             resolved.defaultArguments.end());
  if (argumentExpressions.size() != resolved.parameterTypes.size()) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The resolved constexpr constructor has an invalid argument "
        "count.");
  }
  std::vector<ConstantValue> arguments;
  arguments.reserve(argumentExpressions.size());
  for (std::size_t index = 0; index < argumentExpressions.size(); ++index) {
    if (argumentExpressions[index] == nullptr) {
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "The resolved constexpr constructor has a missing default "
          "argument.");
    }
    ConstexprEvaluation argument =
        evaluateConstexprExpression(*argumentExpressions[index], context);
    if (!argument) {
      return argument;
    }
    const SemanticType parameterType =
        constexprType(resolved.parameterTypes[index], context);
    ConstantEvaluation converted = convertConstantToType(
        *argument.value, parameterType.kind == SemanticType::Reference
                             ? parameterType.arguments[0]
                             : parameterType);
    if (!converted) {
      return constexprFailure(expressionToken(*argumentExpressions[index]),
                              converted.failure);
    }
    arguments.push_back(*converted.value);
  }

  GenericSubstitution substitution = classSubstitution(constructedType);
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const GenericParameterInfo &parameter : constructor->genericParameters) {
    if (parameter.value) {
      if (valueIndex < resolved.valueArguments.size()) {
        substitution.values.insert_or_assign(
            parameter.id,
            constexprValue(resolved.valueArguments[valueIndex++], context));
      }
    } else if (typeIndex < resolved.typeArguments.size()) {
      substitution.types.insert_or_assign(
          parameter.id,
          constexprType(resolved.typeArguments[typeIndex++], context));
    }
  }

  ResolvedConstructionInfo concrete = resolved;
  concrete.constructedType = constructedType;
  for (auto &type : concrete.typeArguments)
    type = constexprType(type, context);
  for (auto &value : concrete.valueArguments)
    value = constexprValue(value, context);
  for (auto &type : concrete.parameterTypes)
    type = constexprType(type, context);
  const bool referenceInputs =
      std::any_of(concrete.parameterTypes.begin(),
                  concrete.parameterTypes.end(), [](const SemanticType &type) {
                    return type.kind == SemanticType::Reference;
                  });
  if (context.callDepth != 0 &&
      std::any_of(concrete.parameterTypes.begin(),
                  concrete.parameterTypes.end(), [](const SemanticType &type) {
                    return type.kind == SemanticType::Reference &&
                           type.referenceAccess == AccessMode::Mutable;
                  }))
    return constexprFailure(callSite,
                            ConstantEvaluationFailure::UnsupportedExpression,
                            "Mutable constructor arguments require their "
                            "enclosing body to execute as verified MIR.");
  if (auto staged = executeStagedConstexpr(concrete, arguments, substitution,
                                           {}, callSite, context)) {
    return *staged ? constexprSuccess(source, *staged->value, context)
                   : std::move(*staged);
  }
  if (referenceInputs)
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "Constexpr constructor references require verified MIR execution.");

  const std::size_t enclosingScopeCount = context.scopes.size();
  const std::size_t enclosingSubstitutionCount = context.substitutions.size();
  const std::size_t enclosingReceiverCount = context.receivers.size();
  context.scopes.emplace_back();
  context.substitutions.push_back(substitution);
  ++context.callDepth;
  const auto restore = [&]() {
    --context.callDepth;
    context.scopes.resize(enclosingScopeCount);
    context.substitutions.resize(enclosingSubstitutionCount);
    context.receivers.resize(enclosingReceiverCount);
  };

  seedConstexprValueParameters(owner->genericParameters, substitution,
                               context.scopes.back());
  seedConstexprValueParameters(constructor->genericParameters, substitution,
                               context.scopes.back());
  for (std::size_t index = 0; index < declaration->parameters().size();
       ++index) {
    const BindingInfo *binding =
        semanticModel.findBinding(declaration->parameters()[index]);
    if (binding == nullptr || binding->symbol == 0 ||
        index >= arguments.size()) {
      restore();
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "The constexpr evaluator could not resolve a constructor "
          "parameter.");
    }
    context.scopes.back().insert_or_assign(binding->symbol, arguments[index]);
  }

  ConstantAggregate aggregate{
      .kind = ConstantAggregateKind::Object,
      .identity = constructedType.classId,
      .exactType = std::make_shared<const SemanticType>(constructedType)};
  aggregate.fields.reserve(owner->fields.size());
  const GenericSubstitution classArguments = classSubstitution(constructedType);
  for (const FieldInfo &field : owner->fields) {
    if (field.declaration == nullptr) {
      restore();
      return constexprFailure(callSite,
                              ConstantEvaluationFailure::UnsupportedType);
    }
    const auto member = owner->members.find(field.declaration->name().lexeme);
    const BindingInfo *binding = semanticModel.findBinding(*field.declaration);
    if (member == owner->members.end() || binding == nullptr ||
        binding->symbol == 0) {
      restore();
      return constexprFailure(
          field.declaration->name(),
          ConstantEvaluationFailure::UnsupportedExpression,
          "The constexpr evaluator could not resolve a structural field.");
    }
    const SemanticType fieldType =
        substituteType(member->second.symbol.type, classArguments);
    const Expr *initializerExpression = nullptr;
    for (const ConstructorInitializer &initializer :
         declaration->initializers()) {
      const ResolvedConstructorInitializerInfo *initializerInfo =
          semanticModel.findConstructorInitializer(initializer);
      if (initializerInfo != nullptr &&
          initializerInfo->kind == ConstructorInitializerTargetKind::Field &&
          initializerInfo->field == binding->symbol &&
          initializer.arguments.size() == 1) {
        initializerExpression = initializer.arguments.front().get();
        break;
      }
    }
    if (initializerExpression == nullptr) {
      initializerExpression = field.declaration->initializer().get();
    }
    if (initializerExpression == nullptr) {
      restore();
      return constexprFailure(
          field.declaration->name(),
          ConstantEvaluationFailure::UnsupportedExpression,
          "Every field of a constexpr aggregate must be initialized.");
    }
    ConstexprEvaluation initializer =
        evaluateConstexprExpression(*initializerExpression, context);
    if (!initializer) {
      restore();
      return initializer;
    }
    ConstantEvaluation converted =
        convertConstantToType(*initializer.value, fieldType);
    if (!converted) {
      restore();
      return constexprFailure(expressionToken(*initializerExpression),
                              converted.failure);
    }
    aggregate.fields.push_back(
        {.symbol = binding->symbol, .value = *converted.value});
  }

  ConstantValue object = makeConstantAggregate(std::move(aggregate));
  context.receivers.push_back(object);
  ConstexprStatementEvaluation execution =
      executeConstexprStatements(declaration->body()->statements(), context);
  restore();
  if (execution.control == ConstexprControlFlow::Failure) {
    return std::move(execution.failure);
  }
  if (execution.control != ConstexprControlFlow::Next) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "A constexpr constructor body cannot transfer control outside the "
        "constructor.");
  }
  return constexprSuccess(source, std::move(object), context);
}

auto SemanticVisitor::Impl::evaluateConstexprExpression(
    const ExprPtr &expression, ConstexprInterpreter &context)
    -> ConstexprEvaluation {
  if (!expression) {
    return constexprFailure({},
                            ConstantEvaluationFailure::UnsupportedExpression);
  }
  return evaluateConstexprExpression(*expression, context);
}

auto SemanticVisitor::Impl::evaluateConstexprExpression(
    const Expr &source, ConstexprInterpreter &context) -> ConstexprEvaluation {
  if (context.work->steps++ >= constexprExpressionStepLimit) {
    return constexprFailure(expressionToken(source),
                            ConstantEvaluationFailure::ResourceLimit);
  }

  const SemanticType sourceType =
      constexprType(semanticModel.typeOf(source), context);
  if (const auto *unexpected = exprAs<Unexpected>(&source)) {
    const auto error =
        evaluateConstexprExpression(unexpected->error(), context);
    if (!error)
      return error;
    const auto result = makeConstantResult(sourceType, false, error.value);
    return result ? constexprSuccess(source, *result.value, context)
                  : constexprFailure(expressionToken(source), result.failure);
  }
  if (const ResolvedOperatorInfo *operation =
          semanticModel.findOperator(source)) {
    // Selection belongs to ordinary semantics. Normalize only the source
    // operands; the same call boundary and verified MIR execute the body.
    const Expr *receiver = nullptr;
    std::vector<const Expr *> arguments;
    if (const auto *binary = exprAs<Binary>(&source)) {
      receiver = binary->left().get();
      arguments.push_back(binary->right().get());
    } else if (const auto *unary = exprAs<Unary>(&source)) {
      receiver = unary->right().get();
    } else if (const auto *index = exprAs<Index>(&source)) {
      receiver = index->object().get();
      arguments.push_back(index->index().get());
    } else if (const auto *call = exprAs<Call>(&source)) {
      receiver = call->callee().get();
      for (const ExprPtr &argument : call->arguments()) {
        arguments.push_back(argument.get());
      }
      if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call)) {
        return evaluateConstexprCall(source, *resolved, receiver, arguments,
                                     call->paren(), context);
      }
    }
    if (receiver == nullptr ||
        operation->receiverMutability == ReceiverMutability::Consuming) {
      return constexprFailure(expressionToken(source),
                              ConstantEvaluationFailure::UnsupportedExpression,
                              "Compile-time operators require a supported "
                              "non-consuming receiver call.");
    }
    const ResolvedCallInfo call{.function = operation->function,
                                .declaration = operation->declaration,
                                .returnType = operation->returnType,
                                .parameterTypes = operation->parameterTypes,
                                .dispatch = operation->dispatch,
                                .dispatchOwner = operation->dispatchOwner,
                                .receiverMode = operation->receiverMode};
    return evaluateConstexprCall(source, call, receiver, arguments,
                                 expressionToken(source), context);
  }
  if (semanticModel.isContextualIntegerOperand(source)) {
    // A signed literal is one contextual operand, not a negation in the
    // target domain: +128 is not an int8_t, but -128 is. Preserve the
    // declaration's checked literal meaning instead of evaluating that
    // unrepresentable positive intermediate.
    const auto value = integerConstant(&source);
    const auto domain = constantIntegerDomain(sourceType);
    if (value && domain) {
      const CheckedIntegerValue integer{.negative = value->negative,
                                        .magnitude = value->magnitude};
      if (!checkedIntegerFits(integer, *domain)) {
        return constexprFailure(expressionToken(source),
                                ConstantEvaluationFailure::IntegerOverflow);
      }
      return constexprSuccess(source, makeConstantInteger(integer, *domain),
                              context);
    }
  }
  if (exprAs<LayoutQuery>(&source) != nullptr ||
      exprAs<MemberRequirement>(&source) != nullptr) {
    if (const std::optional<ConstantValue> value =
            semanticModel.findConstant(source)) {
      return constexprSuccess(source, *value, context);
    }
    return constexprFailure(expressionToken(source),
                            ConstantEvaluationFailure::UnsupportedType);
  }
  if (const auto *literal = exprAs<LiteralExpr>(&source)) {
    const ConstantEvaluation evaluated = evaluateConstantLiteral(
        literal->value(), constantIntegerDomain(sourceType));
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : constexprFailure(literal->token(), evaluated.failure);
  }
  if (const auto *grouping = exprAs<Grouping>(&source)) {
    ConstexprEvaluation evaluated =
        evaluateConstexprExpression(grouping->expression(), context);
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : evaluated;
  }
  if (const auto *qualified = exprAs<QualifiedName>(&source)) {
    if (const ResolvedEnumeratorInfo *enumerator =
            semanticModel.findEnumerator(*qualified)) {
      if (sourceType.kind != SemanticType::Enum || sourceType.enumId == 0 ||
          sourceType.enumId != enumerator->owner) {
        return constexprFailure(
            expressionToken(source), ConstantEvaluationFailure::UnsupportedType,
            "A constexpr enumerator lost its exact scoped-enum identity.");
      }
      const EnumTypeInfo *owner = semanticModel.findEnumType(enumerator->owner);
      if (owner == nullptr || owner->payload) {
        return constexprFailure(
            expressionToken(source), ConstantEvaluationFailure::UnsupportedType,
            "Payload enumerators are outside the bounded constexpr "
            "subset.");
      }
      const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(owner->underlyingType);
      if (!domain) {
        return constexprFailure(expressionToken(source),
                                ConstantEvaluationFailure::UnsupportedType);
      }
      return constexprSuccess(
          source,
          ConstantValue{ConstantEnum{
              .enumId = enumerator->owner,
              .value = ConstantInteger{.negative = enumerator->value.negative,
                                       .magnitude = enumerator->value.magnitude,
                                       .domain = *domain}}},
          context);
    }
  }
  if (exprAs<Variable>(&source) != nullptr ||
      exprAs<QualifiedName>(&source) != nullptr) {
    const SymbolId symbol = semanticModel.findResolvedSymbol(source);
    if (const ConstexprLocalValue *local =
            findConstexprLocal(context, symbol)) {
      if (*local) {
        return constexprSuccess(source, **local, context);
      }
      return constexprFailure(
          expressionToken(source),
          ConstantEvaluationFailure::NonConstantReference,
          "A constexpr function read an uninitialized local binding.");
    }
    if (const std::optional<ConstantValue> value =
            semanticModel.findConstant(source)) {
      return constexprSuccess(source, *value, context);
    }
    return constexprFailure(expressionToken(source),
                            ConstantEvaluationFailure::NonConstantReference);
  }
  if (exprAs<This>(&source) != nullptr) {
    if (context.receivers.empty()) {
      return constexprFailure(
          expressionToken(source),
          ConstantEvaluationFailure::NonConstantReference,
          "'this' is not available outside a constexpr aggregate call.");
    }
    return constexprSuccess(source, context.receivers.back(), context);
  }
  if (const auto *member = exprAs<Get>(&source)) {
    ConstexprEvaluation object =
        evaluateConstexprExpression(member->object(), context);
    if (!object) {
      return object;
    }
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&*object.value);
    if (aggregate == nullptr || aggregate->value == nullptr ||
        aggregate->value->kind != ConstantAggregateKind::Object) {
      return constexprFailure(
          member->name(), ConstantEvaluationFailure::UnsupportedType,
          "Constexpr member access requires a structural aggregate value.");
    }
    const SymbolId field = semanticModel.findResolvedSymbol(source);
    const auto found = std::find_if(
        aggregate->value->fields.begin(), aggregate->value->fields.end(),
        [field](const ConstantAggregateField &candidate) {
          return candidate.symbol == field;
        });
    if (field == 0 || found == aggregate->value->fields.end()) {
      return constexprFailure(
          member->name(), ConstantEvaluationFailure::UnsupportedExpression,
          "The selected member is not a constexpr structural field.");
    }
    return constexprSuccess(source, found->value, context);
  }
  if (const auto *indexExpression = exprAs<Index>(&source)) {
    ConstexprEvaluation object =
        evaluateConstexprExpression(indexExpression->object(), context);
    if (!object) {
      return object;
    }
    ConstexprEvaluation index =
        evaluateConstexprExpression(indexExpression->index(), context);
    if (!index) {
      return index;
    }
    if (constexprType(semanticModel.typeOf(*indexExpression->object()),
                      context) == SemanticType::StringView) {
      const ConstantEvaluation evaluated =
          evaluateConstantTextIndex(*object.value, *index.value);
      if (evaluated) {
        return constexprSuccess(source, *evaluated.value, context);
      }
      const auto *bytes = std::get_if<std::string>(&*object.value);
      return constexprFailure(
          expressionToken(indexExpression->index()), evaluated.failure,
          bytes == nullptr
              ? "Constexpr string-view inspection requires constant text."
              : "Constexpr string-view index is outside the valid range [0, " +
                    std::to_string(bytes->size()) + ").");
    }
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&*object.value);
    const std::optional<std::uint64_t> offset =
        constantUnsignedMagnitude(*index.value);
    if (aggregate == nullptr || aggregate->value == nullptr ||
        aggregate->value->kind != ConstantAggregateKind::Array || !offset) {
      return constexprFailure(
          indexExpression->bracket(),
          ConstantEvaluationFailure::InvalidOperands,
          "Constexpr indexing requires a fixed array and a non-negative "
          "integer index.");
    }
    if (*offset >= aggregate->value->elements.size()) {
      return constexprFailure(
          indexExpression->bracket(),
          ConstantEvaluationFailure::InvalidOperands,
          "Constexpr fixed-array index " + std::to_string(*offset) +
              " is outside the bound " +
              std::to_string(aggregate->value->elements.size()) + ".");
    }
    return constexprSuccess(source, aggregate->value->elements[*offset],
                            context);
  }
  if (const auto *assignment = exprAs<Assign>(&source)) {
    ConstexprLocalValue *target = findMutableConstexprLocal(
        context, semanticModel.findResolvedSymbol(source));
    if (target == nullptr) {
      return constexprFailure(
          assignment->name(), ConstantEvaluationFailure::NonConstantReference,
          "A constexpr function can only assign to its local bindings and "
          "parameters.");
    }
    ConstexprEvaluation value =
        evaluateConstexprExpression(assignment->value(), context);
    if (!value) {
      return value;
    }

    ConstantEvaluation evaluated;
    if (assignment->oper().kind == TokenKind::EQUAL) {
      evaluated = convertConstantToType(*value.value, sourceType);
    } else {
      if (!*target) {
        return constexprFailure(
            assignment->oper(), ConstantEvaluationFailure::NonConstantReference,
            "A compound assignment reads an uninitialized local binding.");
      }
      const std::optional<TokenKind> operation =
          constexprCompoundOperator(assignment->oper().kind);
      evaluated =
          operation ? evaluateConstantBinary(*operation, **target, *value.value,
                                             constantIntegerDomain(sourceType),
                                             semanticFloatFormat(sourceType))
                    : ConstantEvaluation{
                          .failure =
                              ConstantEvaluationFailure::UnsupportedExpression};
      if (evaluated) {
        evaluated = convertConstantToType(*evaluated.value, sourceType);
      }
    }
    if (!evaluated) {
      return constexprFailure(assignment->oper(), evaluated.failure);
    }
    *target = *evaluated.value;
    return constexprSuccess(source, **target, context);
  }
  if (const auto *unary = exprAs<Unary>(&source)) {
    if (unary->oper().kind == TokenKind::PLUS_PLUS ||
        unary->oper().kind == TokenKind::MINUS_MINUS) {
      return evaluateConstexprMutation(source, unary->right(), unary->oper(),
                                       false, context);
    }
    ConstexprEvaluation operand =
        evaluateConstexprExpression(unary->right(), context);
    if (!operand) {
      return operand;
    }
    const ConstantEvaluation evaluated = evaluateConstantUnary(
        unary->oper().kind, *operand.value, constantIntegerDomain(sourceType));
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : constexprFailure(unary->oper(), evaluated.failure);
  }
  if (const auto *postfix = exprAs<Postfix>(&source)) {
    return evaluateConstexprMutation(source, postfix->expression(),
                                     postfix->oper(), true, context);
  }
  if (const auto *binary = exprAs<Binary>(&source)) {
    ConstexprEvaluation left =
        evaluateConstexprExpression(binary->left(), context);
    if (!left) {
      return left;
    }
    ConstexprEvaluation right =
        evaluateConstexprExpression(binary->right(), context);
    if (!right) {
      return right;
    }
    const ConstantEvaluation evaluated = evaluateConstantBinary(
        binary->oper().kind, *left.value, *right.value,
        constantIntegerDomain(sourceType), semanticFloatFormat(sourceType));
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : constexprFailure(binary->oper(), evaluated.failure);
  }
  if (const auto *logical = exprAs<Logical>(&source)) {
    ConstexprEvaluation left =
        evaluateConstexprExpression(logical->left(), context);
    if (!left) {
      return left;
    }
    const bool *leftBoolean = std::get_if<bool>(&*left.value);
    if (leftBoolean == nullptr) {
      return constexprFailure(logical->oper(),
                              ConstantEvaluationFailure::InvalidOperands);
    }
    if ((logical->oper().kind == TokenKind::AND && !*leftBoolean) ||
        (logical->oper().kind == TokenKind::OR && *leftBoolean)) {
      return constexprSuccess(source, ConstantValue{*leftBoolean}, context);
    }
    ConstexprEvaluation right =
        evaluateConstexprExpression(logical->right(), context);
    if (!right) {
      return right;
    }
    const ConstantEvaluation evaluated = evaluateConstantLogical(
        logical->oper().kind, *left.value, *right.value);
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : constexprFailure(logical->oper(), evaluated.failure);
  }
  if (const auto *conditional = exprAs<ConditionalExpr>(&source)) {
    ConstexprEvaluation condition =
        evaluateConstexprExpression(conditional->condition(), context);
    if (!condition) {
      return condition;
    }
    const bool *selected = std::get_if<bool>(&*condition.value);
    if (selected == nullptr) {
      return constexprFailure(conditional->question(),
                              ConstantEvaluationFailure::InvalidOperands);
    }
    ConstexprEvaluation result =
        evaluateConstexprExpression(*selected ? conditional->thenExpression()
                                              : conditional->elseExpression(),
                                    context);
    return result ? constexprSuccess(source, *result.value, context) : result;
  }
  if (const auto *conversion = exprAs<Conversion>(&source)) {
    ConstexprEvaluation operand =
        evaluateConstexprExpression(conversion->value(), context);
    if (!operand) {
      return operand;
    }
    const ConstantEvaluation evaluated =
        convertConstantToType(*operand.value, sourceType);
    return evaluated ? constexprSuccess(source, *evaluated.value, context)
                     : constexprFailure(conversion->paren(), evaluated.failure);
  }
  if (const auto *array = exprAs<ArrayInitializer>(&source)) {
    if (sourceType.kind != SemanticType::Array ||
        sourceType.arguments.size() != 1 ||
        sourceType.arrayLengthParameterId != 0) {
      return constexprFailure(
          array->brace(), ConstantEvaluationFailure::UnsupportedType,
          "Constexpr array initialization requires a concrete fixed-array "
          "type.");
    }
    ConstantAggregate aggregate{
        .kind = ConstantAggregateKind::Array,
        .exactType = std::make_shared<const SemanticType>(sourceType)};
    aggregate.elements.reserve(sourceType.arrayLength);
    if (array->elements().empty()) {
      for (std::uint64_t index = 0; index < sourceType.arrayLength; ++index) {
        ConstantEvaluation value =
            defaultConstexprValue(sourceType.arguments.front(), context);
        if (!value) {
          return constexprFailure(array->brace(), value.failure);
        }
        aggregate.elements.push_back(*value.value);
      }
    } else {
      if (array->elements().size() != sourceType.arrayLength) {
        return constexprFailure(
            array->brace(), ConstantEvaluationFailure::InvalidOperands,
            "The constexpr fixed-array element count differs from its "
            "declared bound.");
      }
      for (const ExprPtr &element : array->elements()) {
        ConstexprEvaluation value =
            evaluateConstexprExpression(element, context);
        if (!value) {
          return value;
        }
        ConstantEvaluation converted =
            convertConstantToType(*value.value, sourceType.arguments.front());
        if (!converted) {
          return constexprFailure(expressionToken(element), converted.failure);
        }
        aggregate.elements.push_back(*converted.value);
      }
    }
    return constexprSuccess(
        source, ConstantValue{makeConstantAggregate(std::move(aggregate))},
        context);
  }
  if (const auto *initializer = exprAs<DirectInitializer>(&source)) {
    if (const ResolvedConstructionInfo *construction =
            semanticModel.findConstruction(*initializer)) {
      return evaluateConstexprConstruction(source, *construction,
                                           initializer->arguments(),
                                           initializer->brace(), context);
    }
    if (initializer->arguments().empty() &&
        sourceType.kind == SemanticType::Expected) {
      const auto result = defaultConstexprValue(sourceType, context);
      return result ? constexprSuccess(source, *result.value, context)
                    : constexprFailure(initializer->brace(), result.failure);
    }
    if (initializer->arguments().size() != 1) {
      return constexprFailure(initializer->brace(),
                              ConstantEvaluationFailure::UnsupportedExpression);
    }
    ConstexprEvaluation operand =
        evaluateConstexprExpression(initializer->arguments().front(), context);
    if (!operand) {
      return operand;
    }
    const ConstantEvaluation evaluated =
        convertConstantToType(*operand.value, sourceType);
    return evaluated
               ? constexprSuccess(source, *evaluated.value, context)
               : constexprFailure(initializer->brace(), evaluated.failure);
  }
  if (const auto *call = exprAs<Call>(&source)) {
    if (const ResolvedConstructionInfo *construction =
            semanticModel.findConstruction(*call)) {
      return evaluateConstexprConstruction(
          source, *construction, call->arguments(), call->paren(), context);
    }
    if (const auto *member = exprAs<Get>(call->callee().get())) {
      const SemanticType objectType = semanticModel.typeOf(*member->object());
      if (objectType.kind == SemanticType::Expected &&
          objectType.arguments.size() == 2) {
        ConstexprEvaluation object =
            evaluateConstexprExpression(member->object(), context);
        if (!object) {
          return object;
        }
        const auto *selected = semanticModel.findCall(*call);
        const auto intrinsic =
            selected ? selected->intrinsic : IntrinsicKind::None;
        const auto state = constantResultHasValue(*object.value);
        if (!state)
          return constexprFailure(member->name(), state.failure);
        if (intrinsic == IntrinsicKind::ExpectedHasValue &&
            call->arguments().empty())
          return constexprSuccess(source, *state.value, context);
        if ((intrinsic == IntrinsicKind::ExpectedValue ||
             intrinsic == IntrinsicKind::ExpectedError) &&
            call->arguments().empty()) {
          const auto payload = constantResultPayload(
              *object.value, intrinsic == IntrinsicKind::ExpectedValue);
          return payload ? constexprSuccess(source, *payload.value, context)
                         : constexprFailure(
                               member->name(), payload.failure,
                               "Constant evaluation cannot extract the "
                               "inactive or void result alternative.");
        }
        if (intrinsic == IntrinsicKind::ExpectedValueOr &&
            call->arguments().size() == 1) {
          // Ordinary call arguments are eager even when the success is used.
          ConstexprEvaluation fallback =
              evaluateConstexprExpression(call->arguments().front(), context);
          if (!fallback) {
            return fallback;
          }
          const ConstantEvaluation converted =
              std::get<bool>(*state.value)
                  ? constantResultPayload(*object.value, true)
                  : convertConstantToType(*fallback.value,
                                          objectType.arguments[0]);
          return converted ? constexprSuccess(source, *converted.value, context)
                           : constexprFailure(
                                 expressionToken(call->arguments().front()),
                                 converted.failure);
        }
        return constexprFailure(
            member->name(), ConstantEvaluationFailure::UnsupportedExpression,
            "The bounded constexpr evaluator supports has_value(), "
            "value(), error(), and value_or() for passive results.");
      }
    }
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    if (resolved == nullptr) {
      return constexprFailure(call->paren(),
                              ConstantEvaluationFailure::UnsupportedExpression);
    }
    if (resolved->intrinsic == IntrinsicKind::StringViewSize ||
        resolved->intrinsic == IntrinsicKind::StringViewEmpty) {
      const auto *member = exprAs<Get>(call->callee().get());
      if (member == nullptr || !call->arguments().empty() ||
          resolved->dispatchOwner != SemanticType::StringView) {
        return constexprFailure(call->paren(),
                                ConstantEvaluationFailure::InvalidOperands);
      }
      ConstexprEvaluation object =
          evaluateConstexprExpression(member->object(), context);
      if (!object) {
        return object;
      }
      const ConstantEvaluation evaluated =
          resolved->intrinsic == IntrinsicKind::StringViewSize
              ? evaluateConstantTextSize(*object.value)
              : evaluateConstantTextEmpty(*object.value);
      return evaluated ? constexprSuccess(source, *evaluated.value, context)
                       : constexprFailure(member->name(), evaluated.failure);
    }
    if ((resolved->intrinsic == IntrinsicKind::NumericAliasConversion ||
         resolved->intrinsic ==
             IntrinsicKind::NumericTypeParameterConversion) &&
        call->arguments().size() == 1) {
      ConstexprEvaluation operand =
          evaluateConstexprExpression(call->arguments().front(), context);
      if (!operand) {
        return operand;
      }
      const ConstantEvaluation evaluated = convertConstantToType(
          *operand.value, constexprType(resolved->returnType, context));
      return evaluated ? constexprSuccess(source, *evaluated.value, context)
                       : constexprFailure(call->paren(), evaluated.failure);
    }
    if (resolved->intrinsic ==
            IntrinsicKind::DefaultTypeParameterConstruction &&
        call->arguments().size() == 1) {
      ConstexprEvaluation operand =
          evaluateConstexprExpression(call->arguments().front(), context);
      if (!operand) {
        return operand;
      }
      const ConstantEvaluation evaluated = convertConstantToType(
          *operand.value, constexprType(resolved->returnType, context));
      return evaluated ? constexprSuccess(source, *evaluated.value, context)
                       : constexprFailure(call->paren(), evaluated.failure);
    }
    if (const std::optional<IntegerArithmeticIntrinsic> arithmetic =
            integerArithmeticIntrinsic(resolved->intrinsic)) {
      const bool checkedResult =
          arithmetic->mode == IntegerArithmeticMode::CheckedResult;
      const SemanticType &valueType =
          checkedResult &&
                  resolved->returnType.kind == SemanticType::Expected &&
                  resolved->returnType.arguments.size() == 2
              ? resolved->returnType.arguments[0]
              : resolved->returnType;
      const std::optional<CheckedIntegerDomain> domain =
          constantIntegerDomain(valueType);
      if (!domain || call->arguments().size() != 2 ||
          resolved->parameterTypes.size() != 2) {
        return constexprFailure(call->paren(),
                                ConstantEvaluationFailure::InvalidOperands);
      }

      std::array<CheckedIntegerValue, 2> operands;
      for (std::size_t index = 0; index < operands.size(); ++index) {
        ConstexprEvaluation argument =
            evaluateConstexprExpression(call->arguments()[index], context);
        if (!argument) {
          return argument;
        }
        const ConstantEvaluation converted = convertConstantToType(
            *argument.value, resolved->parameterTypes[index]);
        const auto *integer =
            converted ? std::get_if<ConstantInteger>(&*converted.value)
                      : nullptr;
        if (integer == nullptr || integer->domain != *domain) {
          return constexprFailure(
              expressionToken(call->arguments()[index]),
              converted ? ConstantEvaluationFailure::InvalidOperands
                        : converted.failure);
        }
        operands[index] = checkedIntegerValue(*integer);
      }

      if (checkedResult) {
        const std::optional<CheckedIntegerOutcome> evaluated =
            evaluateCheckedIntegerBinary(arithmetic->operation, operands[0],
                                         operands[1], *domain);
        if (!evaluated) {
          return constexprFailure(call->paren(),
                                  ConstantEvaluationFailure::InvalidOperands);
        }
        ConstantEvaluation result;
        if (const auto *integer =
                std::get_if<CheckedIntegerValue>(&*evaluated)) {
          result = makeConstantResult(resolved->returnType, true,
                                      makeConstantInteger(*integer, *domain));
        } else if (std::get<CheckedIntegerFailure>(*evaluated) !=
                   CheckedIntegerFailure::Overflow) {
          return constexprFailure(call->paren(),
                                  ConstantEvaluationFailure::InvalidOperands);
        } else {
          const auto &errorType = resolved->returnType.arguments[1];
          const auto *enumeration =
              semanticModel.findEnumType(errorType.enumId);
          const auto errorDomain =
              enumeration ? constantIntegerDomain(enumeration->underlyingType)
                          : std::nullopt;
          if (!errorDomain)
            return constexprFailure(call->paren(),
                                    ConstantEvaluationFailure::InvalidOperands);
          result = makeConstantResult(
              resolved->returnType, false,
              ConstantEnum{.enumId = errorType.enumId,
                           .value = makeConstantInteger({}, *errorDomain)});
        }
        return result ? constexprSuccess(source, *result.value, context)
                      : constexprFailure(call->paren(), result.failure);
      }
      const std::optional<CheckedIntegerValue> evaluated =
          evaluateDefinedIntegerBinary(arithmetic->operation, operands[0],
                                       operands[1], *domain, arithmetic->mode);
      if (!evaluated) {
        return constexprFailure(call->paren(),
                                ConstantEvaluationFailure::InvalidOperands);
      }
      return constexprSuccess(
          source, ConstantValue{makeConstantInteger(*evaluated, *domain)},
          context);
    }

    const auto *member = exprAs<Get>(call->callee().get());
    std::vector<const Expr *> explicitArguments;
    for (const ExprPtr &argument : call->arguments()) {
      explicitArguments.push_back(argument.get());
    }
    return evaluateConstexprCall(
        source, *resolved, member == nullptr ? nullptr : member->object().get(),
        explicitArguments, call->paren(), context);
  }

  return constexprFailure(expressionToken(source),
                          ConstantEvaluationFailure::UnsupportedExpression);
}

auto SemanticVisitor::Impl::evaluateConstexprCall(
    const Expr &source, const ResolvedCallInfo &call,
    const Expr *receiverExpression,
    const std::vector<const Expr *> &explicitArguments, const Token &callSite,
    ConstexprInterpreter &context) -> ConstexprEvaluation {
  const ResolvedCallInfo *resolved = &call;
  if (const std::optional<ConstantValue> reflected =
          metaIntrinsicConstant(*resolved, context)) {
    return constexprSuccess(source, *reflected, context);
  }
  if ((resolved->intrinsic == IntrinsicKind::DefaultTypeParameterConstruction ||
       resolved->intrinsic == IntrinsicKind::ExactTypeParameterConstruction) &&
      explicitArguments.empty()) {
    // `T()` for a scalar: the zero value of the substituted type.
    const SemanticType type = constexprType(resolved->returnType, context);
    std::optional<ConstantValue> zero;
    switch (type.kind) {
    case SemanticType::Int8:
    case SemanticType::Int16:
    case SemanticType::Int32:
    case SemanticType::Int64:
    case SemanticType::UInt8:
    case SemanticType::UInt16:
    case SemanticType::UInt32:
    case SemanticType::UInt64: {
      const bool signedValue =
          type.kind == SemanticType::Int8 || type.kind == SemanticType::Int16 ||
          type.kind == SemanticType::Int32 || type.kind == SemanticType::Int64;
      const std::uint8_t width =
          type.kind == SemanticType::Int8 || type.kind == SemanticType::UInt8
              ? 8
          : type.kind == SemanticType::Int16 ||
                  type.kind == SemanticType::UInt16
              ? 16
          : type.kind == SemanticType::Int32 ||
                  type.kind == SemanticType::UInt32
              ? 32
              : 64;
      zero = ConstantValue{
          ConstantInteger{.magnitude = 0,
                          .domain = CheckedIntegerDomain{
                              .width = width, .signedValue = signedValue}}};
      break;
    }
    case SemanticType::Float:
      zero = ConstantValue{
          BinaryFloat{.bits = 0, .format = BinaryFloatFormat::Binary32}};
      break;
    case SemanticType::Double:
      zero = ConstantValue{
          BinaryFloat{.bits = 0, .format = BinaryFloatFormat::Binary64}};
      break;
    case SemanticType::Bool:
      zero = ConstantValue{false};
      break;
    case SemanticType::Char:
      zero = ConstantValue{CharacterLiteral{}};
      break;
    default:
      break;
    }
    if (zero) {
      return constexprSuccess(source, *zero, context);
    }
  }
  const FunctionInfo *functionInfo =
      semanticModel.findFunction(resolved->function);
  const FunctionDecl *declaration = functionInfo != nullptr
                                        ? functionInfo->bodyDeclaration()
                                        : resolved->declaration;
  if (declaration == nullptr || !declaration->isConstexpr()) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The selected call does not name a constexpr function.");
  }
  if (!constexprDefinitionsAnalyzed.contains(declaration) ||
      !declaredBeforeCurrent(declaration)) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The constexpr definition of '" + declaration->name().lexeme +
            "' must be available before this constant-evaluated call.");
  }
  if (functionInfo == nullptr) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The constexpr call lost its canonical function identity.");
  }

  GenericSubstitution substitution;
  std::optional<ConstantValue> receiver;
  if (functionInfo->ownerClass != 0 && !functionInfo->staticMember) {
    if (receiverExpression == nullptr ||
        resolved->dispatch != CallDispatch::Static) {
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "Polymorphic constexpr dispatch is not supported.");
    }
    ConstexprEvaluation object =
        evaluateConstexprExpression(*receiverExpression, context);
    if (!object) {
      return object;
    }
    const auto *aggregate = std::get_if<ConstantAggregateValue>(&*object.value);
    if (aggregate == nullptr || aggregate->value == nullptr ||
        aggregate->value->kind != ConstantAggregateKind::Object) {
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedType,
          "A constexpr instance call requires a structural aggregate "
          "receiver.");
    }
    receiver = *object.value;
    const SemanticType dispatchOwner =
        constexprType(resolved->dispatchOwner, context);
    substitution = classSubstitution(dispatchOwner);
  } else if (functionInfo->ownerClass != 0) {
    substitution =
        classSubstitution(constexprType(resolved->dispatchOwner, context));
  }

  std::size_t typeArgument = 0;
  std::size_t valueArgument = 0;
  for (const GenericParameterInfo &parameter :
       functionInfo->genericParameters) {
    if (parameter.pack) {
      std::vector<SemanticType> elements;
      while (typeArgument < resolved->typeArguments.size())
        SemanticType::appendExpandedArgument(
            elements,
            constexprType(resolved->typeArguments[typeArgument++], context));
      substitution.types.insert_or_assign(
          parameter.id,
          SemanticType::concreteTypePack(parameter.id, std::move(elements)));
    } else if (parameter.value) {
      if (valueArgument >= resolved->valueArguments.size()) {
        return constexprFailure(
            callSite, ConstantEvaluationFailure::UnsupportedExpression,
            "The constexpr call is missing a concrete generic value "
            "argument.");
      }
      substitution.values.insert_or_assign(
          parameter.id,
          constexprValue(resolved->valueArguments[valueArgument++], context));
    } else {
      if (typeArgument >= resolved->typeArguments.size()) {
        return constexprFailure(
            callSite, ConstantEvaluationFailure::UnsupportedExpression,
            "The constexpr call is missing a concrete generic type "
            "argument.");
      }
      substitution.types.insert_or_assign(
          parameter.id,
          constexprType(resolved->typeArguments[typeArgument++], context));
    }
  }
  if (explicitArguments.size() + resolved->defaultArguments.size() !=
      resolved->parameterTypes.size()) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The resolved constexpr call has an invalid argument count.");
  }

  std::vector<const Expr *> argumentExpressions;
  argumentExpressions.reserve(resolved->parameterTypes.size());
  argumentExpressions.insert(argumentExpressions.end(),
                             explicitArguments.begin(),
                             explicitArguments.end());
  argumentExpressions.insert(argumentExpressions.end(),
                             resolved->defaultArguments.begin(),
                             resolved->defaultArguments.end());
  std::vector<ConstantValue> arguments;
  arguments.reserve(argumentExpressions.size());
  for (std::size_t index = 0; index < argumentExpressions.size(); ++index) {
    if (argumentExpressions[index] == nullptr) {
      return constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "The resolved constexpr call has a missing default argument.");
    }
    ConstexprEvaluation argument =
        evaluateConstexprExpression(*argumentExpressions[index], context);
    if (!argument) {
      return argument;
    }
    SemanticType parameterType =
        constexprType(resolved->parameterTypes[index], context);
    if (parameterType.kind == SemanticType::Reference &&
        parameterType.arguments.size() == 1) {
      const SemanticType referent = parameterType.arguments.front();
      parameterType = referent;
    }
    const ConstantEvaluation converted =
        convertConstantToType(*argument.value, parameterType);
    if (!converted) {
      return constexprFailure(expressionToken(*argumentExpressions[index]),
                              converted.failure);
    }
    arguments.push_back(*converted.value);
  }

  ConstexprEvaluation evaluated = executeConstexprFunction(
      *declaration, constexprType(resolved->returnType, context), arguments,
      substitution, receiver, callSite, context);
  return evaluated ? constexprSuccess(source, *evaluated.value, context)
                   : evaluated;
}

auto SemanticVisitor::Impl::executeConstexprStatements(
    const StmtList &statements, ConstexprInterpreter &context)
    -> ConstexprStatementEvaluation {
  for (const StmtPtr &statement : statements) {
    ConstexprStatementEvaluation evaluated =
        executeConstexprStatement(statement.get(), context);
    if (evaluated.control != ConstexprControlFlow::Next) {
      return evaluated;
    }
  }
  return {};
}

auto SemanticVisitor::Impl::executeConstexprStatement(
    const Stmt *statement, ConstexprInterpreter &context)
    -> ConstexprStatementEvaluation {
  if (statement == nullptr) {
    return {};
  }
  if (context.work->steps++ >= constexprExpressionStepLimit) {
    return constexprStatementFailure(
        constexprFailure({}, ConstantEvaluationFailure::ResourceLimit));
  }
  if (const auto *block = stmtAs<BlockStmt>(statement)) {
    context.scopes.emplace_back();
    ConstexprStatementEvaluation evaluated =
        executeConstexprStatements(block->statements(), context);
    context.scopes.pop_back();
    return evaluated;
  }
  if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
    const StmtList *branch = conditional->activeBranch(target);
    return branch == nullptr ? ConstexprStatementEvaluation{}
                             : executeConstexprStatements(*branch, context);
  }
  if (stmtAs<EmptyStmt>(statement) != nullptr) {
    return {};
  }
  if (stmtAs<StaticAssertDecl>(statement) != nullptr) {
    return {};
  }
  if (const auto *declaration = stmtAs<VariableDecl>(statement)) {
    const BindingInfo *binding = semanticModel.findBinding(*declaration);
    if (binding == nullptr || binding->symbol == 0) {
      return constexprStatementFailure(constexprFailure(
          declaration->name(), ConstantEvaluationFailure::UnsupportedExpression,
          "The constexpr evaluator could not resolve a local binding."));
    }
    const SemanticType bindingType = constexprType(binding->type, context);
    if (declaration->isStatic() || !isSupportedConstexprType(bindingType)) {
      return constexprStatementFailure(constexprFailure(
          declaration->name(), ConstantEvaluationFailure::UnsupportedType,
          "Executed constexpr function locals must be automatic supported "
          "value bindings."));
    }

    ConstexprLocalValue value;
    if (binding->constant) {
      value = *binding->constant;
    } else if (declaration->initializer()) {
      ConstexprEvaluation initializer =
          evaluateConstexprExpression(declaration->initializer(), context);
      if (!initializer) {
        return constexprStatementFailure(std::move(initializer));
      }
      const ConstantEvaluation converted =
          convertConstantToType(*initializer.value, bindingType);
      if (!converted) {
        return constexprStatementFailure(constexprFailure(
            expressionToken(declaration->initializer()), converted.failure));
      }
      value = *converted.value;
    }
    if (context.scopes.empty()) {
      return constexprStatementFailure(
          constexprFailure(declaration->name(),
                           ConstantEvaluationFailure::UnsupportedExpression));
    }
    context.scopes.back().insert_or_assign(binding->symbol, std::move(value));
    return {};
  }
  if (const auto *expression = stmtAs<ExpressionStmt>(statement)) {
    ConstexprEvaluation evaluated =
        evaluateConstexprExpression(expression->expression(), context);
    return evaluated ? ConstexprStatementEvaluation{}
                     : constexprStatementFailure(std::move(evaluated));
  }
  if (const auto *returnStatement = stmtAs<ReturnStmt>(statement)) {
    if (!returnStatement->value()) {
      return constexprStatementFailure(
          constexprFailure(returnStatement->keyword(),
                           ConstantEvaluationFailure::UnsupportedExpression,
                           "A scalar constexpr function must return a value."));
    }
    ConstexprEvaluation value =
        evaluateConstexprExpression(returnStatement->value(), context);
    if (!value) {
      return constexprStatementFailure(std::move(value));
    }
    return {.control = ConstexprControlFlow::Return,
            .value = std::move(value.value)};
  }
  if (const auto *ifStatement = stmtAs<IfStmt>(statement)) {
    const StmtPtr *selectedBranch = nullptr;
    if (ifStatement->isConstexpr()) {
      const std::optional<bool> selected =
          semanticModel.findConstexprBranch(*ifStatement);
      if (!selected) {
        return constexprStatementFailure(constexprFailure(
            *ifStatement->constexprKeyword(),
            ConstantEvaluationFailure::UnsupportedExpression,
            "The if constexpr branch was not resolved by semantic "
            "analysis."));
      }
      selectedBranch =
          *selected ? &ifStatement->thenBranch() : &ifStatement->elseBranch();
    } else {
      ConstexprEvaluation condition =
          evaluateConstexprExpression(ifStatement->condition(), context);
      if (!condition) {
        return constexprStatementFailure(std::move(condition));
      }
      const bool *selected = std::get_if<bool>(&*condition.value);
      if (selected == nullptr) {
        return constexprStatementFailure(
            constexprFailure(expressionToken(ifStatement->condition()),
                             ConstantEvaluationFailure::InvalidOperands));
      }
      selectedBranch =
          *selected ? &ifStatement->thenBranch() : &ifStatement->elseBranch();
    }
    return selectedBranch == nullptr || !*selectedBranch
               ? ConstexprStatementEvaluation{}
               : executeConstexprStatement(selectedBranch->get(), context);
  }
  if (const auto *loopControl = stmtAs<LoopControlStmt>(statement)) {
    return {.control = loopControl->keyword().kind == TokenKind::BREAK
                           ? ConstexprControlFlow::Break
                           : ConstexprControlFlow::Continue};
  }
  if (const auto *whileStatement = stmtAs<WhileStmt>(statement)) {
    while (true) {
      ConstexprEvaluation condition =
          evaluateConstexprExpression(whileStatement->condition(), context);
      if (!condition) {
        return constexprStatementFailure(std::move(condition));
      }
      const bool *continueLoop = std::get_if<bool>(&*condition.value);
      if (continueLoop == nullptr) {
        return constexprStatementFailure(
            constexprFailure(expressionToken(whileStatement->condition()),
                             ConstantEvaluationFailure::InvalidOperands));
      }
      if (!*continueLoop) {
        return {};
      }
      ConstexprStatementEvaluation body =
          executeConstexprStatement(whileStatement->body().get(), context);
      if (body.control == ConstexprControlFlow::Break) {
        return {};
      }
      if (body.control != ConstexprControlFlow::Next &&
          body.control != ConstexprControlFlow::Continue) {
        return body;
      }
    }
  }
  if (const auto *doWhile = stmtAs<DoWhileStmt>(statement)) {
    while (true) {
      ConstexprStatementEvaluation body =
          executeConstexprStatement(doWhile->body().get(), context);
      if (body.control == ConstexprControlFlow::Break) {
        return {};
      }
      if (body.control != ConstexprControlFlow::Next &&
          body.control != ConstexprControlFlow::Continue) {
        return body;
      }
      ConstexprEvaluation condition =
          evaluateConstexprExpression(doWhile->condition(), context);
      if (!condition) {
        return constexprStatementFailure(std::move(condition));
      }
      const bool *continueLoop = std::get_if<bool>(&*condition.value);
      if (continueLoop == nullptr) {
        return constexprStatementFailure(
            constexprFailure(expressionToken(doWhile->condition()),
                             ConstantEvaluationFailure::InvalidOperands));
      }
      if (!*continueLoop) {
        return {};
      }
    }
  }
  if (const auto *forStatement = stmtAs<ForStmt>(statement)) {
    context.scopes.emplace_back();
    ConstexprStatementEvaluation result =
        executeConstexprStatement(forStatement->initializer().get(), context);
    while (result.control == ConstexprControlFlow::Next) {
      if (forStatement->condition()) {
        ConstexprEvaluation condition =
            evaluateConstexprExpression(forStatement->condition(), context);
        if (!condition) {
          result = constexprStatementFailure(std::move(condition));
          break;
        }
        const bool *continueLoop = std::get_if<bool>(&*condition.value);
        if (continueLoop == nullptr) {
          result = constexprStatementFailure(
              constexprFailure(expressionToken(forStatement->condition()),
                               ConstantEvaluationFailure::InvalidOperands));
          break;
        }
        if (!*continueLoop) {
          break;
        }
      }
      ConstexprStatementEvaluation body =
          executeConstexprStatement(forStatement->body().get(), context);
      if (body.control == ConstexprControlFlow::Break) {
        break;
      }
      if (body.control != ConstexprControlFlow::Next &&
          body.control != ConstexprControlFlow::Continue) {
        result = std::move(body);
        break;
      }
      if (forStatement->increment()) {
        ConstexprEvaluation increment =
            evaluateConstexprExpression(forStatement->increment(), context);
        if (!increment) {
          result = constexprStatementFailure(std::move(increment));
          break;
        }
      }
    }
    context.scopes.pop_back();
    return result;
  }
  if (const auto *switchStatement = stmtAs<SwitchStmt>(statement)) {
    ConstexprEvaluation subject =
        evaluateConstexprExpression(switchStatement->expression(), context);
    if (!subject) {
      return constexprStatementFailure(std::move(subject));
    }
    const SwitchArm *selected = nullptr;
    const SwitchArm *defaultArm = nullptr;
    for (const SwitchArm &arm : switchStatement->arms()) {
      for (const SwitchLabel &label : arm.labels) {
        if (label.isDefault()) {
          defaultArm = &arm;
          continue;
        }
        ConstexprEvaluation labelValue =
            evaluateConstexprExpression(label.value, context);
        if (!labelValue) {
          return constexprStatementFailure(std::move(labelValue));
        }
        const ConstantEvaluation equal = evaluateConstantComparison(
            TokenKind::EQUAL_EQUAL, *subject.value, *labelValue.value);
        if (!equal) {
          return constexprStatementFailure(
              constexprFailure(label.keyword, equal.failure));
        }
        if (std::get<bool>(*equal.value)) {
          selected = &arm;
          break;
        }
      }
      if (selected != nullptr) {
        break;
      }
    }
    selected = selected == nullptr ? defaultArm : selected;
    if (selected == nullptr) {
      return {};
    }
    context.scopes.emplace_back();
    ConstexprStatementEvaluation result =
        executeConstexprStatements(selected->statements, context);
    context.scopes.pop_back();
    return result.control == ConstexprControlFlow::Break
               ? ConstexprStatementEvaluation{}
               : result;
  }

  return constexprStatementFailure(constexprFailure(
      {}, ConstantEvaluationFailure::UnsupportedExpression,
      "The executed constexpr path contains a statement that is not "
      "supported by the bounded evaluator."));
}

auto SemanticVisitor::Impl::executeConstexprFunction(
    const FunctionDecl &function, const SemanticType &returnType,
    const std::vector<ConstantValue> &arguments,
    const GenericSubstitution &substitution,
    const std::optional<ConstantValue> &receiver, const Token &callSite,
    ConstexprInterpreter &context) -> ConstexprEvaluation {
  if (const auto *info = semanticModel.findFunction(function);
      info != nullptr && info->bodyDeclaration() != &function) {
    return executeConstexprFunction(*info->bodyDeclaration(), returnType,
                                    arguments, substitution, receiver, callSite,
                                    context);
  }
  if (context.callDepth >= constexprCallDepthLimit) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::ResourceLimit,
        "The constexpr call depth exceeded the compiler limit of " +
            std::to_string(constexprCallDepthLimit) + ".");
  }
  const bool hasPack = std::any_of(
      function.parameters().begin(), function.parameters().end(),
      [](const auto &parameter) { return parameter.pack.has_value(); });
  if (!function.body() ||
      (!hasPack && function.parameters().size() != arguments.size())) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "The constexpr function definition is unavailable or has an "
        "invalid parameter list.");
  }

  // An AST compatibility frame owns value snapshots, not MIR constant-cell
  // origins. Staging only its mutable callee would lose writes to that caller.
  // Execute the whole enclosing body as MIR or reject; a successful isolated
  // callee evaluation is not permission to copy-interpret a checked mutation.
  const bool mutableOrigin =
      function.receiverMutability() == ReceiverMutability::Mutable ||
      std::any_of(function.parameters().begin(), function.parameters().end(),
                  [&](const Parameter &parameter) {
                    const auto type = typeOf(parameter);
                    return type.kind == SemanticType::Reference &&
                           type.referenceAccess == AccessMode::Mutable;
                  });
  if (context.callDepth != 0 && mutableOrigin) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "Mutable constexpr calls require their enclosing body to execute as "
        "verified MIR, not an AST compatibility frame.");
  }

  if (auto staged = executeStagedConstexpr(&function, arguments, substitution,
                                           receiver, callSite, context)) {
    return std::move(*staged);
  }

  if (function.operatorName() || hasPack) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "Compile-time operator and parameter-pack calls require an available "
        "verified MIR body.");
  }

  if (returnType.kind == SemanticType::Reference ||
      returnType == SemanticType::Void ||
      function.receiverMutability() == ReceiverMutability::Mutable ||
      std::any_of(function.parameters().begin(), function.parameters().end(),
                  [](const Parameter &parameter) {
                    return parameter.type.reference.has_value();
                  })) {
    return constexprFailure(
        callSite, ConstantEvaluationFailure::UnsupportedExpression,
        "Compile-time reference calls require an available verified MIR body "
        "with checked loans to supported values.");
  }

  const std::size_t enclosingScopeCount = context.scopes.size();
  const std::size_t enclosingSubstitutionCount = context.substitutions.size();
  const std::size_t enclosingReceiverCount = context.receivers.size();
  context.scopes.emplace_back();
  context.substitutions.push_back(substitution);
  if (receiver) {
    context.receivers.push_back(*receiver);
  }
  ++context.callDepth;
  ConstexprEvaluation setupFailure;
  if (const FunctionInfo *info = semanticModel.findFunction(function)) {
    if (info->ownerClass != 0) {
      seedConstexprValueParameters(
          classInfo(info->ownerClass).genericParameters, substitution,
          context.scopes.back());
    }
    seedConstexprValueParameters(info->genericParameters, substitution,
                                 context.scopes.back());
  }
  for (std::size_t index = 0; index < function.parameters().size(); ++index) {
    const BindingInfo *binding =
        semanticModel.findBinding(function.parameters()[index]);
    if (binding == nullptr || binding->symbol == 0) {
      setupFailure = constexprFailure(
          callSite, ConstantEvaluationFailure::UnsupportedExpression,
          "The constexpr evaluator could not resolve a function "
          "parameter.");
      break;
    }
    context.scopes.back().insert_or_assign(binding->symbol, arguments[index]);
  }

  ConstexprStatementEvaluation execution;
  if (setupFailure.failure == ConstantEvaluationFailure::None) {
    execution =
        executeConstexprStatements(function.body()->statements(), context);
  }
  --context.callDepth;
  context.scopes.resize(enclosingScopeCount);
  context.substitutions.resize(enclosingSubstitutionCount);
  context.receivers.resize(enclosingReceiverCount);

  if (setupFailure.failure != ConstantEvaluationFailure::None) {
    return setupFailure;
  }
  if (execution.control == ConstexprControlFlow::Failure) {
    return std::move(execution.failure);
  }
  if (execution.control != ConstexprControlFlow::Return || !execution.value) {
    return constexprFailure(callSite,
                            ConstantEvaluationFailure::UnsupportedExpression,
                            "Constexpr function '" + function.name().lexeme +
                                "' completed without returning a value.");
  }
  const ConstantEvaluation converted =
      convertConstantToType(*execution.value, returnType);
  return converted ? ConstexprEvaluation{.value = *converted.value,
                                         .location = callSite}
                   : constexprFailure(callSite, converted.failure);
}

auto SemanticVisitor::Impl::staticAssertConditionSpan(
    const StaticAssertDecl &declaration) -> SourceSpan {
  const Token &start = declaration.conditionStart();
  const Token &delimiter =
      declaration.comma() ? *declaration.comma() : declaration.rightParen();
  return {.source = start.source,
          .start = start.position,
          .end = std::max(start.position, delimiter.position),
          .line = start.line};
}

auto SemanticVisitor::Impl::appendStaticAssertSubstitutions(
    Diagnostic &diagnostic) const -> void {
  struct Substitution {
    GenericParameterId id = 0;
    std::string value;
  };
  std::vector<Substitution> substitutions;
  substitutions.reserve(instanceTypeSubstitution.size() +
                        instanceValueSubstitution.size());
  for (const auto &[id, type] : instanceTypeSubstitution) {
    if (type.kind != SemanticType::TypeParameter) {
      substitutions.push_back({id, typeSpelling(type)});
    }
  }
  for (const auto &[id, value] : instanceValueSubstitution) {
    if (value.kind == CompileTimeValue::UInt64) {
      substitutions.push_back({id, std::to_string(value.value)});
    }
  }
  std::sort(substitutions.begin(), substitutions.end(),
            [](const Substitution &left, const Substitution &right) {
              return left.id < right.id;
            });
  for (const Substitution &substitution : substitutions) {
    const GenericParameterInfo *parameter =
        genericParameterInfo(substitution.id);
    if (parameter == nullptr) {
      continue;
    }
    diagnostic.related.push_back(
        {tokenSpan(parameter->name), "Concrete substitution: '" +
                                         parameter->name.lexeme + "' = '" +
                                         substitution.value + "'."});
  }
}

auto SemanticVisitor::Impl::reportConstexprFailure(
    const VariableDecl &declaration, const SemanticType &type,
    const ConstexprEvaluation &evaluation) -> void {
  reporter.reportConstexprFailure(declaration, type, evaluation.failure,
                                  evaluation.detail, evaluation.location);
}

auto SemanticVisitor::Impl::reportConstexprConditionFailure(
    const IfStmt &statement, const ConstexprEvaluation &evaluation) -> void {
  reporter.reportConstexprConditionFailure(
      statement, evaluation.failure, evaluation.detail, evaluation.location);
}

auto SemanticVisitor::Impl::constexprBindingValue(
    const VariableDecl &declaration, const SemanticType &type,
    bool initializerSemanticallyValid) -> std::optional<ConstantValue> {
  if (!declaration.isConstexpr()) {
    return std::nullopt;
  }

  bool valid = initializerSemanticallyValid;
  if (declaration.isMutable()) {
    report(*declaration.constexprKeyword(),
           "A constexpr binding is immutable and cannot be declared mut.",
           "GTI-S2057");
    valid = false;
  }
  if (currentClass && functionDepth == 0 && !declaration.isStatic()) {
    report(*declaration.constexprKeyword(),
           "A constexpr class or struct field must be static.", "GTI-S2057");
    diagnostics.back().hints.emplace_back(
        "Declare the member 'static constexpr' or use an ordinary instance "
        "field.");
    valid = false;
  }
  if (!declaration.initializer()) {
    report(declaration.name(), "A constexpr binding requires an initializer.",
           "GTI-S2057");
    valid = false;
  }
  if (!isSupportedConstexprType(type)) {
    report(
        *declaration.constexprKeyword(),
        "The bounded constexpr evaluator supports fixed-width integers, "
        "float, double, bool, char, string_view, and nullptr_t values, "
        "plus scoped enums, concrete fixed arrays, structural "
        "aggregates, and expected results with supported alternatives; type '" +
            typeSpelling(type) + "' is not supported yet.",
        "GTI-S2057");
    valid = false;
  }
  if (!valid) {
    return std::nullopt;
  }

  ConstexprInterpreter context = makeConstexprInterpreter();
  ConstexprEvaluation evaluated =
      evaluateConstexprExpression(declaration.initializer(), context);
  if (!evaluated) {
    reportConstexprFailure(declaration, type, evaluated);
    return std::nullopt;
  }
  const ConstantEvaluation converted =
      convertConstantToType(*evaluated.value, type);
  if (!converted) {
    evaluated.failure = converted.failure;
    evaluated.location = expressionToken(declaration.initializer());
    reportConstexprFailure(declaration, type, evaluated);
    return std::nullopt;
  }
  if (currentProgramEffectOwner.valid()) {
    programEffects.erase(currentProgramEffectOwner);
  }
  return converted.value;
}

auto SemanticVisitor::Impl::constantBoolean(const ExprPtr &expression)
    -> std::optional<bool> {
  if (const auto *literal = exprAs<LiteralExpr>(expression.get())) {
    if (const bool *value = std::get_if<bool>(&literal->value())) {
      return *value;
    }
    return std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return constantBoolean(grouping->expression());
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::expressionNeverReturns(const Expr *expression) const
    -> bool {
  // Only evaluated subexpressions contribute. In particular, a lambda body,
  // type query, or an unselected short-circuit operand cannot terminate its
  // enclosing expression. The resolved declaration, never its spelling, owns
  // the terminal-call fact.
  const auto anyTerminal = [&](const ExprList &values) {
    return std::any_of(values.begin(), values.end(), [&](const ExprPtr &value) {
      return expressionNeverReturns(value.get());
    });
  };
  if (const auto *group = exprAs<Grouping>(expression)) {
    return expressionNeverReturns(group->expression().get());
  }
  if (const auto *call = exprAs<Call>(expression)) {
    const auto *resolved = semanticModel.findCall(*call);
    return (resolved != nullptr && resolved->neverReturns) ||
           expressionNeverReturns(call->callee().get()) ||
           anyTerminal(call->arguments());
  }
  if (const auto *binary = exprAs<Binary>(expression)) {
    if (expressionNeverReturns(binary->left().get())) {
      return true;
    }
    if (binary->oper().kind == TokenKind::AND ||
        binary->oper().kind == TokenKind::OR) {
      const auto left = constantBoolean(binary->left());
      const bool evaluatesRight =
          (binary->oper().kind == TokenKind::AND && left == true) ||
          (binary->oper().kind == TokenKind::OR && left == false);
      return evaluatesRight && expressionNeverReturns(binary->right().get());
    }
    return expressionNeverReturns(binary->right().get());
  }
  if (const auto *conditional = exprAs<ConditionalExpr>(expression)) {
    if (expressionNeverReturns(conditional->condition().get())) {
      return true;
    }
    if (const auto selected = constantBoolean(conditional->condition())) {
      return expressionNeverReturns(*selected
                                        ? conditional->thenExpression().get()
                                        : conditional->elseExpression().get());
    }
    return expressionNeverReturns(conditional->thenExpression().get()) &&
           expressionNeverReturns(conditional->elseExpression().get());
  }
  if (const auto *unary = exprAs<Unary>(expression)) {
    return expressionNeverReturns(unary->right().get());
  }
  if (const auto *conversion = exprAs<Conversion>(expression)) {
    return expressionNeverReturns(conversion->value().get());
  }
  if (const auto *assignment = exprAs<Assign>(expression)) {
    return expressionNeverReturns(assignment->value().get());
  }
  if (const auto *member = exprAs<Get>(expression)) {
    return expressionNeverReturns(member->object().get());
  }
  if (const auto *member = exprAs<Set>(expression)) {
    return expressionNeverReturns(member->object().get()) ||
           expressionNeverReturns(member->value().get());
  }
  if (const auto *index = exprAs<Index>(expression)) {
    return expressionNeverReturns(index->object().get()) ||
           expressionNeverReturns(index->index().get());
  }
  if (const auto *index = exprAs<IndexSet>(expression)) {
    return expressionNeverReturns(index->object().get()) ||
           expressionNeverReturns(index->index().get()) ||
           expressionNeverReturns(index->value().get());
  }
  if (const auto *assignment = exprAs<DereferenceSet>(expression)) {
    return expressionNeverReturns(assignment->object().get()) ||
           expressionNeverReturns(assignment->value().get());
  }
  if (const auto *assignment = exprAs<PlaceSet>(expression)) {
    return expressionNeverReturns(assignment->target().get()) ||
           expressionNeverReturns(assignment->value().get());
  }
  if (const auto *initializer = exprAs<DirectInitializer>(expression)) {
    return anyTerminal(initializer->arguments());
  }
  if (const auto *initializer = exprAs<ArrayInitializer>(expression)) {
    return anyTerminal(initializer->elements());
  }
  if (const auto *unexpected = exprAs<Unexpected>(expression)) {
    return expressionNeverReturns(unexpected->error().get());
  }
  return false;
}

auto SemanticVisitor::Impl::summarizeFlow(const StmtList &statements) const
    -> FlowSummary {
  FlowSummary result;
  for (const StmtPtr &statement : statements) {
    if (!result.canFallThrough) {
      break;
    }
    const FlowSummary next = summarizeFlow(statement.get());
    result.canFallThrough = next.canFallThrough;
    result.breaksEnclosingControl =
        result.breaksEnclosingControl || next.breaksEnclosingControl;
    result.continuesEnclosingLoop =
        result.continuesEnclosingLoop || next.continuesEnclosingLoop;
    if (result.normalReturn == nullptr) {
      result.normalReturn = next.normalReturn;
    }
  }
  return result;
}

auto SemanticVisitor::Impl::summarizeFlow(const Stmt *statement) const
    -> FlowSummary {
  if (statement == nullptr) {
    return {};
  }
  if (const auto *returned = stmtAs<ReturnStmt>(statement)) {
    return {.canFallThrough = false,
            .normalReturn = expressionNeverReturns(returned->value().get())
                                ? nullptr
                                : returned};
  }
  if (const auto *expression = stmtAs<ExpressionStmt>(statement)) {
    return {.canFallThrough =
                !expressionNeverReturns(expression->expression().get())};
  }
  if (const auto *declaration = stmtAs<VariableDecl>(statement)) {
    return {.canFallThrough =
                !expressionNeverReturns(declaration->initializer().get())};
  }
  if (const auto *control = stmtAs<LoopControlStmt>(statement)) {
    return {.canFallThrough = false,
            .breaksEnclosingControl =
                control->keyword().kind == TokenKind::BREAK,
            .continuesEnclosingLoop =
                control->keyword().kind == TokenKind::CONTINUE};
  }
  if (const auto *block = stmtAs<BlockStmt>(statement)) {
    return summarizeFlow(block->statements());
  }
  if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
    const StmtList *branch = conditional->activeBranch(target);
    return branch == nullptr ? FlowSummary{} : summarizeFlow(*branch);
  }
  if (const auto *ifStatement = stmtAs<IfStmt>(statement)) {
    if (expressionNeverReturns(ifStatement->condition().get())) {
      return {.canFallThrough = false};
    }
    if (ifStatement->isConstexpr()) {
      if (const std::optional<bool> selected =
              semanticModel.findConstexprBranch(*ifStatement)) {
        return *selected ? summarizeFlow(ifStatement->thenBranch().get())
                         : summarizeFlow(ifStatement->elseBranch().get());
      }
      return {};
    }
    if (const std::optional<bool> condition =
            constantBoolean(ifStatement->condition())) {
      return *condition ? summarizeFlow(ifStatement->thenBranch().get())
                        : summarizeFlow(ifStatement->elseBranch().get());
    }
    const FlowSummary thenFlow = summarizeFlow(ifStatement->thenBranch().get());
    const FlowSummary elseFlow = summarizeFlow(ifStatement->elseBranch().get());
    return {.canFallThrough =
                thenFlow.canFallThrough || elseFlow.canFallThrough,
            .breaksEnclosingControl = thenFlow.breaksEnclosingControl ||
                                      elseFlow.breaksEnclosingControl,
            .continuesEnclosingLoop = thenFlow.continuesEnclosingLoop ||
                                      elseFlow.continuesEnclosingLoop,
            .normalReturn = thenFlow.normalReturn != nullptr
                                ? thenFlow.normalReturn
                                : elseFlow.normalReturn};
  }
  if (const auto *switchStatement = stmtAs<SwitchStmt>(statement)) {
    if (expressionNeverReturns(switchStatement->expression().get())) {
      return {.canFallThrough = false};
    }
    bool coversEveryValue =
        semanticModel.isExhaustiveSwitch(*switchStatement) ||
        requestSession.recoveredExhaustiveSwitches.contains(switchStatement);
    bool reachesAfterSwitch = false;
    bool continuesEnclosingLoop = false;
    const ReturnStmt *normalReturn = nullptr;
    for (const SwitchArm &arm : switchStatement->arms()) {
      for (const SwitchLabel &label : arm.labels) {
        coversEveryValue = coversEveryValue || label.isDefault();
      }
      const FlowSummary armFlow = summarizeFlow(arm.statements);
      if (normalReturn == nullptr) {
        normalReturn = armFlow.normalReturn;
      }
      reachesAfterSwitch = reachesAfterSwitch || armFlow.canFallThrough ||
                           armFlow.breaksEnclosingControl;
      continuesEnclosingLoop =
          continuesEnclosingLoop || armFlow.continuesEnclosingLoop;
    }
    return {.canFallThrough = !coversEveryValue || reachesAfterSwitch,
            .continuesEnclosingLoop = continuesEnclosingLoop,
            .normalReturn = normalReturn};
  }
  if (const auto *forStatement = stmtAs<ForStmt>(statement)) {
    const FlowSummary initializer =
        summarizeFlow(forStatement->initializer().get());
    if (!initializer.canFallThrough) {
      return initializer;
    }
    if (expressionNeverReturns(forStatement->condition().get())) {
      return {.canFallThrough = false};
    }
    const FlowSummary body = summarizeFlow(forStatement->body().get());
    const bool repeatsForever =
        !forStatement->condition() ||
        constantBoolean(forStatement->condition()) == true;
    return {.canFallThrough = !repeatsForever || body.breaksEnclosingControl,
            .normalReturn = constantBoolean(forStatement->condition()) == false
                                ? nullptr
                                : body.normalReturn};
  }
  if (const auto *rangeFor = stmtAs<RangeForStmt>(statement)) {
    return summarizeFlow(rangeFor->lowered().get());
  }
  if (const auto *loop = stmtAs<ConstexprForStmt>(statement)) {
    const ConstexprForExpansion *expansion =
        semanticModel.findConstexprForExpansion(*loop);
    if (expansion == nullptr) {
      // Deferred to concrete instances; the symbolic primary is not a flow
      // proof, like a deferred `if constexpr`.
      return {};
    }
    StmtList blocks;
    blocks.reserve(expansion->iterations.size());
    for (const ConstexprForIteration &iteration : expansion->iterations) {
      blocks.push_back(iteration.block);
    }
    return summarizeFlow(blocks);
  }
  if (const auto *doWhile = stmtAs<DoWhileStmt>(statement)) {
    const FlowSummary body = summarizeFlow(doWhile->body().get());
    const bool reachesCondition =
        body.canFallThrough || body.continuesEnclosingLoop;
    return {.canFallThrough =
                body.breaksEnclosingControl ||
                (reachesCondition &&
                 !expressionNeverReturns(doWhile->condition().get()) &&
                 constantBoolean(doWhile->condition()) != true),
            .normalReturn = body.normalReturn};
  }
  if (const auto *whileStatement = stmtAs<WhileStmt>(statement)) {
    if (expressionNeverReturns(whileStatement->condition().get())) {
      return {.canFallThrough = false};
    }
    const FlowSummary body = summarizeFlow(whileStatement->body().get());
    return {.canFallThrough =
                constantBoolean(whileStatement->condition()) != true ||
                body.breaksEnclosingControl,
            .normalReturn =
                constantBoolean(whileStatement->condition()) == false
                    ? nullptr
                    : body.normalReturn};
  }
  return {};
}

auto SemanticVisitor::Impl::summarizeLoanFlow(const StmtList &statements) const
    -> FlowSummary {
  FlowSummary result;
  for (const StmtPtr &statement : statements) {
    if (!result.canFallThrough) {
      break;
    }
    const FlowSummary next = summarizeLoanFlow(statement.get());
    result.canFallThrough = next.canFallThrough;
    result.breaksEnclosingControl =
        result.breaksEnclosingControl || next.breaksEnclosingControl;
    result.continuesEnclosingLoop =
        result.continuesEnclosingLoop || next.continuesEnclosingLoop;
  }
  return result;
}

auto SemanticVisitor::Impl::summarizeLoanFlow(const Stmt *statement) const
    -> FlowSummary {
  if (statement == nullptr) {
    return {};
  }
  if (const auto *block = stmtAs<BlockStmt>(statement)) {
    return summarizeLoanFlow(block->statements());
  }
  if (const auto *conditional = stmtAs<ConditionalStmt>(statement)) {
    const StmtList *branch = conditional->activeBranch(target);
    return branch == nullptr ? FlowSummary{} : summarizeLoanFlow(*branch);
  }
  if (const auto *ifStatement = stmtAs<IfStmt>(statement)) {
    if (ifStatement->isConstexpr()) {
      if (const std::optional<bool> selected =
              semanticModel.findConstexprBranch(*ifStatement)) {
        return *selected ? summarizeLoanFlow(ifStatement->thenBranch().get())
                         : summarizeLoanFlow(ifStatement->elseBranch().get());
      }
      return {};
    }
    const FlowSummary thenFlow =
        summarizeLoanFlow(ifStatement->thenBranch().get());
    const FlowSummary elseFlow =
        summarizeLoanFlow(ifStatement->elseBranch().get());
    return {.canFallThrough =
                thenFlow.canFallThrough || elseFlow.canFallThrough,
            .breaksEnclosingControl = thenFlow.breaksEnclosingControl ||
                                      elseFlow.breaksEnclosingControl,
            .continuesEnclosingLoop = thenFlow.continuesEnclosingLoop ||
                                      elseFlow.continuesEnclosingLoop};
  }
  return summarizeFlow(statement);
}

auto SemanticVisitor::Impl::analyzeInferredVariable(
    const VariableDecl &declaration) -> void {
  const std::size_t declarationDiagnosticStart = diagnostics.size();
  const TypeRef &type = declaration.type();
  const bool local = functionDepth > 0;
  const bool ordinaryReference = type.reference &&
                                 !declaration.isRangeBinding() &&
                                 !type.name.last().generated;
  if (!local) {
    report(type.name.last(), "'auto' inference is limited to local bindings.",
           "GTI-S2028");
  }
  if (!type.arguments.empty() || !type.arrayExtents.empty()) {
    report(type.name.last(),
           "'auto' cannot have generic arguments or array extents.",
           "GTI-S2028");
  }
  if (type.reference && type.reference->kind == TokenKind::AND) {
    report(*type.reference,
           "'&&' is currently confined to a class or struct's exact "
           "move constructor policy.",
           "GTI-S2020");
  }
  if (!declaration.initializer()) {
    report(declaration.name(), "An 'auto' binding requires an initializer.",
           "GTI-S2028");
  }

  const Expr *enclosingExpectedPayloadRangeCandidate =
      expectedPayloadRangeCandidate;
  if (declaration.isRangeOwner() && !declaration.isMutable() &&
      declaration.initializer()) {
    expectedPayloadRangeCandidate = declaration.initializer().get();
  }
  // A written reference establishes a loan, just like an explicit T& local.
  // Do not treat its initializer as an independent value read before ordinary
  // reference/reborrow validation records the actual access and dependency.
  if (ordinaryReference && declaration.initializer()) {
    ++suppressLoanPlaceAccessDepth;
    ++programStoragePlaceUseDepth;
  }
  const Expr *inferredExpression = declaration.initializer().get();
  while (const auto *grouping = exprAs<Grouping>(inferredExpression)) {
    inferredExpression = grouping->expression().get();
  }
  // Only a conditional value join needs an unknown initializer context.
  // In particular, auto x{...} must retain its missing-constructed-type error;
  // Unknown there means recovery from an already-invalid explicit type.
  const SemanticType initializerType =
      !declaration.initializer() ? SemanticType::Unknown
      : exprAs<ConditionalExpr>(inferredExpression)
          ? analyzeInitializer(declaration.initializer(), SemanticType::Unknown)
          : analyzeExpectedCallableResult(declaration.initializer(),
                                          SemanticType::Unknown);
  if (ordinaryReference && declaration.initializer()) {
    if (declaration.isMutable())
      recordMutableBindingUse(declaration.initializer());
    --suppressLoanPlaceAccessDepth;
    --programStoragePlaceUseDepth;
  }
  expectedPayloadRangeCandidate = enclosingExpectedPayloadRangeCandidate;
  SemanticType inferredType = initializerType;
  const ExpressionInfo *initializerInfo =
      declaration.initializer()
          ? semanticModel.findExpression(*declaration.initializer())
          : nullptr;
  const bool rangeOwnerReference =
      (declaration.isRangeOwner() || declaration.isRangeElementOwner()) &&
      initializerInfo != nullptr &&
      initializerInfo->category == ValueCategory::Place &&
      initializerType != SemanticType::Unknown;
  if (declaration.isRangeElementOwner() && initializerInfo != nullptr &&
      initializerInfo->category == ValueCategory::Value &&
      initializerType != SemanticType::Unknown &&
      initializerType.kind != SemanticType::TypeParameter &&
      (!readableIteratorValue(initializerType) ||
       initializerInfo->traits.containsBorrowedState)) {
    report(expressionToken(declaration.initializer()),
           "A value-yielding range requires an independent copyable element "
           "with no borrowed state.",
           "GTI-S2083");
  }
  const bool ownerDependentText =
      (initializerType.kind == SemanticType::StringView ||
       initializerType.kind == SemanticType::CString) &&
      initializerInfo != nullptr &&
      initializerInfo->traits.containsBorrowedState;
  if (rangeOwnerReference) {
    inferredType = SemanticType::referenceTo(
        initializerType,
        declaration.isMutable() ? AccessMode::Mutable : AccessMode::ReadOnly);
  }
  if (initializerType == SemanticType::Void ||
      initializerType == SemanticType::Function ||
      initializerType.kind == SemanticType::TypeName ||
      initializerType.kind == SemanticType::Unexpected) {
    report(declaration.name(),
           "'auto' requires an initializer with a complete value type.",
           "GTI-S2028");
    inferredType = SemanticType::Unknown;
  }
  if (!type.reference && !declaration.isRangeOwner() &&
      !declaration.isRangeElementOwner() &&
      inferredType.kind == SemanticType::Reference) {
    report(declaration.name(),
           "'auto' does not infer references; declare the reference type "
           "explicitly.",
           "GTI-S2028");
    inferredType = SemanticType::Unknown;
  }
  if (type.reference && inferredType != SemanticType::Unknown) {
    inferredType = SemanticType::referenceTo(
        initializerType,
        declaration.isMutable() ? AccessMode::Mutable : AccessMode::ReadOnly);
    if (ordinaryReference)
      validateReferenceReferent(inferredType, *type.reference);
    if (declaration.initializer()) {
      validateReferenceBinding(inferredType, initializerType,
                               declaration.initializer());
    }
  }
  if (rangeOwnerReference && declaration.initializer()) {
    validateReferenceBinding(inferredType, initializerType,
                             declaration.initializer());
  }
  if (inferredType.kind == SemanticType::Lambda && declaration.isMutable()) {
    report(declaration.name(),
           "Lambda bindings are immutable; captured snapshots cannot be "
           "made mutable with 'mut auto'.",
           "GTI-S2027");
  }
  const bool initializerAssignable =
      type.reference || rangeOwnerReference || !declaration.initializer() ||
      inferredType == SemanticType::Unknown ||
      initializerType == SemanticType::Unknown ||
      isOwnershipAssignable(inferredType, initializerType,
                            declaration.initializer());
  if (!initializerAssignable) {
    report(expressionToken(declaration.initializer()),
           "Cannot initialize inferred binding '" + declaration.name().lexeme +
               "' by copying move-only type '" + typeSpelling(inferredType) +
               "'.",
           "GTI-S2003");
    Diagnostic &diagnostic = diagnostics.back();
    if (initializerInfo != nullptr &&
        initializerInfo->category == ValueCategory::Place &&
        hasStableBorrowStorage(declaration.initializer())) {
      diagnostic.hints.emplace_back(
          "Plain 'auto' creates a value, not a reference. To retain a checked "
          "borrow instead, write 'auto& " +
          declaration.name().lexeme + " = ...;' and keep its owner alive.");
    }
    // Moving a borrowed result (for example a checked accessor's result) is
    // not a repair for an attempted copy. Offer movement only for an available
    // whole local/parameter owner for which the ordinary move checks permit it.
    const SemanticPlace source = semanticPlace(declaration.initializer());
    if (movedVariable(declaration.initializer()) != nullptr &&
        source.root != nullptr && !source.receiver &&
        source.projections.empty() &&
        (source.root->bindingKind == SemanticBindingKind::LocalVariable ||
         source.root->bindingKind == SemanticBindingKind::Parameter) &&
        source.root->type.kind != SemanticType::Reference &&
        source.root->valueState == ValueState::Available &&
        typeTraits(source.root->type).movable &&
        (source.root->variableDeclaration == nullptr ||
         !structuredBindingElements.contains(
             source.root->variableDeclaration)) &&
        unavailableProjectedState(source, true) == nullptr &&
        conflictingLoans(loanPlace(source), AccessMode::Mutable).empty()) {
      diagnostic.hints.emplace_back(
          "Transfer ownership explicitly with std::move(owner) if the "
          "original binding is no longer needed.");
    }
    diagnostic.hints.emplace_back(
        "For an independent value, use an explicit clone operation if the "
        "type provides one.");
  }
  if (!type.reference && !rangeOwnerReference && declaration.initializer() &&
      (typeTraits(inferredType).containsBorrowedState || ownerDependentText)) {
    validateStoredBorrowInitialization(inferredType, declaration.initializer());
  }

  const AccessMode access =
      declaration.isMutable() ? AccessMode::Mutable : AccessMode::ReadOnly;
  const bool instanceDependentType =
      local && !instanceAnalysisActive &&
      currentFunctionDeclaration != nullptr &&
      !genericParametersFor(*currentFunctionDeclaration).empty() &&
      declaration.initializer() && inferredType == SemanticType::Unknown &&
      diagnostics.size() == declarationDiagnosticStart;
  const SymbolId symbol = recordBindingOccurrence(
      declaration.name(), inferredType, declaration.isMutable(),
      local ? SemanticBindingKind::LocalVariable
            : SemanticBindingKind::GlobalVariable,
      false, false, instanceDependentType);
  attachDeclarationDocumentation(symbol, declaration);
  const std::optional<ConstantValue> constant =
      constexprBindingValue(declaration, inferredType,
                            diagnostics.size() == declarationDiagnosticStart &&
                                initializerAssignable);
  BindingInfo info = bindingInfo(inferredType, access);
  if (ownerDependentText) {
    info.traits = initializerInfo->traits;
  }
  info.symbol = symbol;
  info.constant = constant;
  semanticModel.record(declaration, std::move(info));
  if (!type.name.last().generated) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(type.name.last()),
         .kind = SemanticOccurrenceKind::InferredType,
         .symbol = symbol,
         .name = type.name.last().lexeme,
         .type = inferredType,
         .traits = ownerDependentText ? initializerInfo->traits
                                      : typeTraits(inferredType),
         .access = access,
         .mutableBinding = declaration.isMutable()});
  }
  const auto rawOrigins = inferredType.kind == SemanticType::RawPointer
                              ? rawPointerOrigins(declaration.initializer())
                              : std::vector<RawPointerOrigin>{};
  const auto rawFields = inferredType.kind != SemanticType::RawPointer
                             ? rawPointerFields(declaration.initializer().get())
                             : std::vector<ProjectedRawPointerOrigins>{};
  if (!predeclaredVariables.contains(&declaration)) {
    if (local) {
      declare(
          declaration.name(), inferredType,
          declaration.isMutable() && inferredType.kind != SemanticType::Lambda,
          SemanticBindingKind::LocalVariable, &declaration, nullptr, constant);
    } else {
      declareNamespaceSymbol(currentNamespace, declaration.name(), inferredType,
                             declaration.isMutable(), false, 0, &declaration,
                             constant);
    }
  }
  if (auto *target = resolveMutable(declaration.name())) {
    target->rawPointerOrigins = rawOrigins;
    target->rawPointerFields = rawFields;
  }
  const bool retainedReference = (type.reference || rangeOwnerReference) &&
                                 !isDirectOwnerType(initializerType);
  const bool retainedStoredValue =
      !type.reference && !rangeOwnerReference &&
      (typeTraits(inferredType).containsBorrowedState || ownerDependentText);
  const auto *rangeIteratorCall =
      declaration.isRangeIterator() && declaration.initializer()
          ? exprAs<Call>(declaration.initializer().get())
          : nullptr;
  const ResolvedCallInfo *rangeIteratorResolution =
      rangeIteratorCall == nullptr ? nullptr
                                   : semanticModel.findCall(*rangeIteratorCall);
  const bool retainedSymbolicRangeIterator =
      inferredType.kind == SemanticType::TypeParameter &&
      rangeIteratorResolution != nullptr &&
      rangeIteratorResolution->borrowOrigin != BorrowOriginKind::None;
  if (local && declaration.initializer() &&
      (retainedReference || retainedStoredValue ||
       retainedSymbolicRangeIterator)) {
    recordRetainedBorrow(declaration, declaration.initializer());
  }
}

} // namespace lang
