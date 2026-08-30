#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::canDataInitializeProgramConstant(
    const ConstantValue &value) -> bool {
  return !std::holds_alternative<ConstantCheckedIntegerResult>(value);
}

auto SemanticVisitor::Impl::recordProgramStorageEffect(
    const Expr &expression, SemanticBindingKind bindingKind, SymbolId symbol,
    OccurrenceRole roles) -> void {
  if (!currentProgramEffectOwner.valid() || symbol == 0 ||
      (bindingKind != SemanticBindingKind::GlobalVariable &&
       bindingKind != SemanticBindingKind::StaticField)) {
    return;
  }
  const std::optional<ConstantValue> constant =
      semanticModel.findConstant(expression);
  const bool valueSubstitutionCandidate =
      programStoragePlaceUseDepth == 0 &&
      !hasRole(roles, OccurrenceRole::Write) &&
      semanticModel.unsafeOperation(expression) !=
          UnsafeOperationKind::AddressOf;
  const bool substitution = constant && valueSubstitutionCandidate &&
                            canMaterializeProgramConstant(*constant);
  if (substitution) {
    semanticModel.recordProgramConstantSubstitution(expression);
    return;
  }
  programEffects[currentProgramEffectOwner].uses.push_back(
      {.symbol = symbol,
       .span = tokenSpan(expressionToken(expression)),
       .expression = &expression,
       .deferredConstantCandidate = valueSubstitutionCandidate});
}

auto SemanticVisitor::Impl::addProgramEffectEdge(ProgramEffectOwner target,
                                                 const Expr &expression)
    -> void {
  addProgramEffectEdge(target, tokenSpan(expressionToken(expression)));
}

auto SemanticVisitor::Impl::clearProgramConstantSubstitutionsInBoundary(
    const Expr &expression, SymbolId symbol) -> void {
  ProgramEffectSummary &summary = programEffects[currentProgramEffectOwner];
  for (ProgramStorageEffectUse &use : summary.uses) {
    if (use.symbol == symbol && use.expression == &expression) {
      use.deferredConstantCandidate = false;
    }
  }
  if (semanticModel.findResolvedSymbol(expression) == symbol &&
      semanticModel.isProgramConstantSubstitution(expression)) {
    semanticModel.clearProgramConstantSubstitution(expression);
  }
  if (const auto *grouping = exprAs<Grouping>(&expression)) {
    if (grouping->expression()) {
      clearProgramConstantSubstitutionsInBoundary(*grouping->expression(),
                                                  symbol);
    }
  } else if (const auto *conversion = exprAs<Conversion>(&expression)) {
    if (conversion->value()) {
      clearProgramConstantSubstitutionsInBoundary(*conversion->value(), symbol);
    }
  } else if (const auto *member = exprAs<Get>(&expression)) {
    if (member->object()) {
      clearProgramConstantSubstitutionsInBoundary(*member->object(), symbol);
    }
  } else if (const auto *index = exprAs<Index>(&expression)) {
    if (index->object()) {
      clearProgramConstantSubstitutionsInBoundary(*index->object(), symbol);
    }
  } else if (const auto *unary = exprAs<Unary>(&expression)) {
    if (unary->right()) {
      clearProgramConstantSubstitutionsInBoundary(*unary->right(), symbol);
    }
  }
}

auto SemanticVisitor::Impl::forceProgramStorageBoundaryAccess(
    const ExprPtr &argument) -> void {
  if (!currentProgramEffectOwner.valid() || !argument) {
    return;
  }
  const SemanticPlace place = semanticPlace(argument);
  if (place.root == nullptr ||
      (place.root->bindingKind != SemanticBindingKind::GlobalVariable &&
       place.root->bindingKind != SemanticBindingKind::StaticField)) {
    return;
  }
  const SymbolId symbol = toolingSymbolFor(*place.root);
  if (symbol == 0) {
    return;
  }
  clearProgramConstantSubstitutionsInBoundary(*argument, symbol);
  ProgramEffectSummary &summary = programEffects[currentProgramEffectOwner];
  const SourceSpan span = tokenSpan(expressionToken(*argument));
  if (std::none_of(summary.uses.begin(), summary.uses.end(),
                   [&](const ProgramStorageEffectUse &use) {
                     return use.symbol == symbol &&
                            use.span.source == span.source &&
                            use.span.start == span.start &&
                            use.span.end == span.end;
                   })) {
    summary.uses.push_back({.symbol = symbol,
                            .span = span,
                            .expression = argument.get(),
                            .deferredConstantCandidate = false});
  }
}

auto SemanticVisitor::Impl::recordProgramArgumentBoundaryEffects(
    std::span<const SemanticType> parameterTypes,
    std::span<const ExprPtr> arguments) -> void {
  const std::size_t count = std::min(parameterTypes.size(), arguments.size());
  for (std::size_t index = 0; index < count; ++index) {
    if (parameterTypes[index].kind == SemanticType::Reference ||
        parameterTypes[index].kind == SemanticType::RawPointer) {
      forceProgramStorageBoundaryAccess(arguments[index]);
    }
  }
}

auto SemanticVisitor::Impl::addProgramEffectEdge(ProgramEffectOwner target,
                                                 SourceSpan span) -> void {
  if (!currentProgramEffectOwner.valid() || !target.valid()) {
    return;
  }
  programEffects[currentProgramEffectOwner].edges.push_back(
      {.target = target, .span = std::move(span)});
}

auto SemanticVisitor::Impl::addUnknownProgramEffect(const Expr &expression,
                                                    std::string reason)
    -> void {
  addUnknownProgramEffect(tokenSpan(expressionToken(expression)),
                          std::move(reason));
}

auto SemanticVisitor::Impl::addUnknownProgramEffect(SourceSpan span,
                                                    std::string reason)
    -> void {
  if (!currentProgramEffectOwner.valid()) {
    return;
  }
  programEffects[currentProgramEffectOwner].unknown.push_back(
      {.span = std::move(span), .reason = std::move(reason)});
}

auto SemanticVisitor::Impl::programConstructor(ConstructorId id) const
    -> const ConstructorInfo * {
  if (id == 0) {
    return nullptr;
  }
  for (const ClassInfo &owner : classes) {
    const auto found = std::find_if(
        owner.constructors.begin(), owner.constructors.end(),
        [id](const ConstructorInfo &candidate) { return candidate.id == id; });
    if (found != owner.constructors.end()) {
      return &*found;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::recordProgramConstructorDefaultArgumentEffects(
    const ConstructorInfo *constructor, std::size_t suppliedArguments,
    const SourceSpan &span) -> void {
  if (constructor == nullptr ||
      suppliedArguments >= constructor->parameterTypes.size()) {
    return;
  }
  for (std::size_t index = suppliedArguments;
       index < constructor->parameterTypes.size(); ++index) {
    if (index >= constructor->defaultArgumentEffectOwners.size() ||
        constructor->defaultArgumentEffectOwners[index] == 0) {
      continue;
    }
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::ConstructorDefaultArgument,
         .id = constructor->defaultArgumentEffectOwners[index]},
        span);
  }
}

auto SemanticVisitor::Impl::recordProgramDefaultConstructionEffects(
    const SemanticType &type, const SourceSpan &span,
    std::unordered_set<ClassId> &visiting) -> void {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    addUnknownProgramEffect(
        span, "default construction has no exact resolved class lifecycle");
    return;
  }
  if (!visiting.insert(type.classId).second) {
    addUnknownProgramEffect(span, "default construction has a recursive base "
                                  "lifecycle");
    return;
  }
  const ClassInfo &owner = classInfo(type.classId);
  if (hasOpenProgramCleanupShape(type)) {
    addUnknownProgramEffect(
        span, "generic default construction has no closed program-storage "
              "effect summary");
    visiting.erase(type.classId);
    return;
  }
  addProgramEffectEdge(
      {.kind = ProgramEffectOwnerKind::FieldInitializers, .id = owner.id},
      span);

  const auto recordBase = [&] {
    const ClassBaseTypeInfo *base = concreteBase(owner);
    if (base == nullptr) {
      return;
    }
    const SemanticType baseType =
        substituteType(base->type, classSubstitution(type));
    recordProgramDefaultConstructionEffects(baseType, span, visiting);
  };
  if (const ConstructorInfo *declared = defaultConstructor(owner)) {
    recordProgramConstructorDefaultArgumentEffects(declared, 0, span);
    if (declared->declaration != nullptr &&
        declared->declaration->body() != nullptr) {
      addProgramEffectEdge(
          {.kind = ProgramEffectOwnerKind::Constructor, .id = declared->id},
          span);
    } else if (declared->declaration != nullptr &&
               declared->declaration->specifier() &&
               declared->declaration->specifier()->kind ==
                   SpecialMemberSpecifierKind::Defaulted) {
      recordBase();
    } else {
      addUnknownProgramEffect(
          span, "a selected GTI constructor has no executable body or "
                "compiler-generated default definition");
    }
  } else if (classCanGenerateDefaultConstructor(owner)) {
    recordBase();
  } else {
    addUnknownProgramEffect(
        span, "default construction has no exact available constructor");
  }
  visiting.erase(type.classId);
}

auto SemanticVisitor::Impl::recordProgramDefaultConstructionEffects(
    const SemanticType &type, const SourceSpan &span) -> void {
  std::unordered_set<ClassId> visiting;
  recordProgramDefaultConstructionEffects(type, span, visiting);
}

auto SemanticVisitor::Impl::recordProgramSelectedConstructorEffect(
    const ConstructorInfo *constructor, const SemanticType &constructedType,
    const SourceSpan &span) -> void {
  if (constructor == nullptr || constructor->declaration == nullptr) {
    addUnknownProgramEffect(
        span, "a selected GTI constructor has no exact declaration");
    return;
  }
  if (constructor->declaration->body() != nullptr) {
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::Constructor, .id = constructor->id},
        span);
    return;
  }
  if (constructor->declaration->specifier() &&
      constructor->declaration->specifier()->kind ==
          SpecialMemberSpecifierKind::Defaulted) {
    const ClassInfo &owner = classInfo(constructedType.classId);
    if (const ClassBaseTypeInfo *base = concreteBase(owner)) {
      recordProgramDefaultConstructionEffects(
          substituteType(base->type, classSubstitution(constructedType)), span);
    }
    return;
  }
  addUnknownProgramEffect(span,
                          "a selected GTI constructor has no executable body");
}

auto SemanticVisitor::Impl::hasOpenProgramCleanupShape(
    const SemanticType &type) const -> bool {
  if (type.kind == SemanticType::Class && type.classId != 0 &&
      type.classId <= classes.size() &&
      (classInfo(type.classId).compilerCapability ==
           CompilerCapabilityTypeKind::AtomicStorage ||
       classInfo(type.classId).compilerCapability ==
           CompilerCapabilityTypeKind::MutexStorage)) {
    // Exact compiler-owned synchronization representations have
    // storage-independent construction. Atomic storage is trivially cleaned;
    // mutex storage relocates its payload and leaves every intermediate shell
    // inactive, so no payload destructor body can run from the generic wrapper
    // expression. The final payload cleanup occurs only with the completed
    // program object, after program initialization. Their generic element
    // therefore does not make initializer effects open-ended.
    return false;
  }
  if (type.kind == SemanticType::TypeParameter ||
      type.kind == SemanticType::TypePack) {
    return true;
  }
  if (std::any_of(type.valueArguments.begin(), type.valueArguments.end(),
                  [](const CompileTimeValue &value) {
                    return value.kind == CompileTimeValue::Parameter;
                  })) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [this](const SemanticType &argument) {
                       return hasOpenProgramCleanupShape(argument);
                     });
}

auto SemanticVisitor::Impl::recordProgramCleanupEffects(
    const SemanticType &type, const SourceSpan &span,
    std::vector<SemanticType> &visiting, bool includeDirectDestructor) -> void {
  if (!currentProgramEffectOwner.valid() || !requiresActiveCleanupFor(type)) {
    return;
  }
  if (hasOpenProgramCleanupShape(type)) {
    addUnknownProgramEffect(span,
                            "cleanup has an open generic lifecycle effect set");
    return;
  }
  if (std::find(visiting.begin(), visiting.end(), type) != visiting.end()) {
    return;
  }
  visiting.push_back(type);
  const auto finish = [&] { visiting.pop_back(); };
  switch (type.kind) {
  case SemanticType::UniqueOwner:
  case SemanticType::SharedPointer:
  case SemanticType::Array:
  case SemanticType::Expected:
  case SemanticType::Unexpected:
    for (const SemanticType &argument : type.arguments) {
      if (argument != SemanticType::Void) {
        recordProgramCleanupEffects(argument, span, visiting);
      }
    }
    finish();
    return;
  case SemanticType::Lambda:
    if (!type.hasLambdaShape()) {
      addUnknownProgramEffect(
          span, "lambda cleanup has no closed capture-type shape");
      finish();
      return;
    }
    for (const SemanticType &capture : type.lambdaCaptureTypes()) {
      recordProgramCleanupEffects(capture, span, visiting);
    }
    finish();
    return;
  case SemanticType::Storage:
  case SemanticType::PrefixStorage:
    // Compiler-private raw storage cleanup cannot execute a GTI body.
    finish();
    return;
  case SemanticType::Class:
    break;
  default:
    finish();
    return;
  }
  if (type.classId == 0 || type.classId > classes.size()) {
    addUnknownProgramEffect(span,
                            "cleanup has no exact resolved class lifecycle");
    finish();
    return;
  }
  const ClassInfo &owner = classInfo(type.classId);
  if (includeDirectDestructor && hasSourceDestructorBody(owner)) {
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::Destructor, .id = owner.id}, span);
  }
  const GenericSubstitution substitution = classSubstitution(type);
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member != owner.members.end()) {
      recordProgramCleanupEffects(
          substituteType(member->second.symbol.type, substitution), span,
          visiting);
    }
  }
  for (const ClassBaseTypeInfo &base : owner.bases) {
    if (!base.interface) {
      recordProgramCleanupEffects(substituteType(base.type, substitution), span,
                                  visiting);
    }
  }
  finish();
}

auto SemanticVisitor::Impl::recordProgramCleanupEffects(
    const SemanticType &type, const SourceSpan &span,
    bool includeDirectDestructor) -> void {
  std::vector<SemanticType> visiting;
  recordProgramCleanupEffects(type, span, visiting, includeDirectDestructor);
}

auto SemanticVisitor::Impl::recordProgramCallEffects(const Expr &expression)
    -> void {
  if (!currentProgramEffectOwner.valid()) {
    return;
  }
  if (const auto *fold = exprAs<PackFold>(&expression);
      fold != nullptr && semanticModel.findPackFold(*fold) != nullptr) {
    addUnknownProgramEffect(expression,
                            "a pack-expanded GTI call has an open effect "
                            "set");
    return;
  }
  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(expression)) {
    std::size_t suppliedArguments = 0;
    if (const auto *call = exprAs<Call>(&expression)) {
      suppliedArguments = call->arguments().size();
      recordProgramArgumentBoundaryEffects(construction->parameterTypes,
                                           call->arguments());
    } else if (const auto *initializer =
                   exprAs<DirectInitializer>(&expression)) {
      suppliedArguments = initializer->arguments().size();
      recordProgramArgumentBoundaryEffects(construction->parameterTypes,
                                           initializer->arguments());
    }
    recordProgramConstructorDefaultArgumentEffects(
        programConstructor(construction->constructor), suppliedArguments,
        tokenSpan(expressionToken(expression)));
    if (construction->constructedType.kind == SemanticType::Class &&
        construction->constructedType.classId != 0) {
      if (hasOpenProgramCleanupShape(construction->constructedType)) {
        addUnknownProgramEffect(
            expression,
            "a generic construction has no closed program-storage effect "
            "summary");
        return;
      }
      const SourceSpan span = tokenSpan(expressionToken(expression));
      if (construction->generatedDefault || construction->constructor == 0) {
        recordProgramDefaultConstructionEffects(construction->constructedType,
                                                span);
      } else {
        addProgramEffectEdge({.kind = ProgramEffectOwnerKind::FieldInitializers,
                              .id = construction->constructedType.classId},
                             span);
        recordProgramSelectedConstructorEffect(
            programConstructor(construction->constructor),
            construction->constructedType, span);
      }
    }
  }
  if (const auto *initializer = exprAs<ArrayInitializer>(&expression)) {
    if (const ResolvedConstructionInfo *construction =
            semanticModel.findArrayElementConstruction(*initializer)) {
      const SourceSpan span = tokenSpan(initializer->brace());
      recordProgramConstructorDefaultArgumentEffects(
          programConstructor(construction->constructor), 0, span);
      if (construction->generatedDefault || construction->constructor == 0) {
        recordProgramDefaultConstructionEffects(construction->constructedType,
                                                span);
      } else {
        addProgramEffectEdge({.kind = ProgramEffectOwnerKind::FieldInitializers,
                              .id = construction->constructedType.classId},
                             span);
        recordProgramSelectedConstructorEffect(
            programConstructor(construction->constructor),
            construction->constructedType, span);
      }
    }
  }
  if (const ResolvedOperatorInfo *operation =
          semanticModel.findOperator(expression)) {
    if (const auto *call = exprAs<Call>(&expression)) {
      recordProgramArgumentBoundaryEffects(operation->parameterTypes,
                                           call->arguments());
    } else if (const auto *binary = exprAs<Binary>(&expression);
               binary != nullptr && !operation->parameterTypes.empty()) {
      if (operation->parameterTypes.front().kind == SemanticType::Reference ||
          operation->parameterTypes.front().kind == SemanticType::RawPointer) {
        forceProgramStorageBoundaryAccess(binary->right());
      }
    } else if (const auto *index = exprAs<Index>(&expression);
               index != nullptr && !operation->parameterTypes.empty()) {
      if (operation->parameterTypes.front().kind == SemanticType::Reference ||
          operation->parameterTypes.front().kind == SemanticType::RawPointer) {
        forceProgramStorageBoundaryAccess(index->index());
      }
    }
    const FunctionInfo *target =
        operation->function == 0
            ? nullptr
            : semanticModel.findFunction(operation->function);
    if (target != nullptr && target->linkage != LanguageLinkage::Gti) {
      return;
    }
    if (operation->dispatch != CallDispatch::Static || target == nullptr ||
        !target->genericParameters.empty()) {
      addUnknownProgramEffect(
          expression,
          "an overloaded operation has no closed static GTI target set");
    } else if (target->declaration != nullptr &&
               target->declaration->runtimeBinding()) {
      // Trusted runtime bindings cannot execute a hidden GTI body. Their
      // explicit receiver/argument storage effects were retained above.
      return;
    } else if (target->declaration == nullptr ||
               target->declaration->body() == nullptr) {
      addUnknownProgramEffect(
          expression, "a selected GTI operation has no executable body");
    } else {
      addProgramEffectEdge(
          {.kind = ProgramEffectOwnerKind::Function, .id = target->id},
          expression);
    }
    return;
  }
  const auto *call = exprAs<Call>(&expression);
  if (call == nullptr) {
    return;
  }
  if (const ResolvedLambdaCallInfo *lambda =
          semanticModel.findLambdaCall(*call)) {
    recordProgramArgumentBoundaryEffects(lambda->parameterTypes,
                                         call->arguments());
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::Lambda, .id = lambda->lambda},
        expression);
    return;
  }
  if (semanticModel.findDeferredCallableCall(*call) != nullptr) {
    const DeferredCallableCallInfo *deferred =
        semanticModel.findDeferredCallableCall(*call);
    recordProgramArgumentBoundaryEffects(deferred->parameterTypes,
                                         call->arguments());
    addUnknownProgramEffect(
        expression, "a deferred callable invocation has no closed target set");
    return;
  }
  const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
  if (resolved == nullptr || resolved->intrinsic != IntrinsicKind::None) {
    return;
  }
  recordProgramArgumentBoundaryEffects(resolved->parameterTypes,
                                       call->arguments());
  const FunctionInfo *target =
      resolved->function == 0 ? nullptr
                              : semanticModel.findFunction(resolved->function);
  if (target != nullptr && target->linkage != LanguageLinkage::Gti) {
    return;
  }
  if (resolved->dispatch != CallDispatch::Static || target == nullptr ||
      !target->genericParameters.empty() || !resolved->typeArguments.empty() ||
      !resolved->valueArguments.empty()) {
    addUnknownProgramEffect(
        expression, "a GTI call has no closed static non-generic target");
    return;
  }
  if (target->declaration != nullptr && target->declaration->runtimeBinding()) {
    // A trusted runtime boundary contributes only its explicit argument
    // storage effects; it cannot call an unrecorded GTI body.
    return;
  }
  if (target->declaration == nullptr ||
      target->declaration->body() == nullptr) {
    addUnknownProgramEffect(expression,
                            "a selected GTI function has no executable "
                            "body");
    return;
  }
  addProgramEffectEdge(
      {.kind = ProgramEffectOwnerKind::Function, .id = target->id}, expression);
}

auto SemanticVisitor::Impl::programInitializationUnitOrder() const
    -> std::vector<SourceUnitId> {
  std::vector<SourceUnitId> result;
  if (sourceGraph == nullptr) {
    return result;
  }
  std::unordered_set<SourceUnitId> visited;
  const std::function<void(SourceUnitId)> visit = [&](SourceUnitId unit) {
    if (unit == 0 || !visited.insert(unit).second) {
      return;
    }
    std::vector<const SourceDependency *> dependencies;
    for (const SourceDependency &dependency : sourceGraph->dependencyEdges()) {
      if (dependency.source == unit &&
          dependency.kind != SourceDependencyKind::Prelude) {
        dependencies.push_back(&dependency);
      }
    }
    std::sort(dependencies.begin(), dependencies.end(),
              [](const SourceDependency *left, const SourceDependency *right) {
                if (left->includeOccurrence != right->includeOccurrence) {
                  return left->includeOccurrence < right->includeOccurrence;
                }
                const std::size_t leftStart =
                    left->directive ? left->directive->start : 0;
                const std::size_t rightStart =
                    right->directive ? right->directive->start : 0;
                return leftStart != rightStart ? leftStart < rightStart
                                               : left->target < right->target;
              });
    std::unordered_set<SourceUnitId> direct;
    for (const SourceDependency *dependency : dependencies) {
      if (direct.insert(dependency->target).second) {
        visit(dependency->target);
      }
    }
    result.push_back(unit);
  };
  for (const SourceUnitId prelude : sourceGraph->preludeRoots()) {
    visit(prelude);
  }
  visit(sourceGraph->entryUnit());
  return result;
}

auto SemanticVisitor::Impl::collectProgramInitializationStatement(
    const Stmt &statement, SourceUnitId sourceUnit,
    std::optional<ClassId> enclosingClass,
    std::vector<ProgramInitializationStep> &steps) const -> void {
  if (const auto *conditional = stmtAs<ConditionalStmt>(&statement)) {
    if (const StmtList *branch = conditional->activeBranch(target)) {
      for (const StmtPtr &child : *branch) {
        collectProgramInitializationStatement(*child, sourceUnit,
                                              enclosingClass, steps);
      }
    }
    return;
  }
  if (const auto *space = stmtAs<NamespaceDecl>(&statement)) {
    for (const StmtPtr &child : space->declarations()) {
      collectProgramInitializationStatement(*child, sourceUnit, enclosingClass,
                                            steps);
    }
    return;
  }
  if (const auto *external = stmtAs<ExternCDecl>(&statement)) {
    for (const StmtPtr &child : external->declarations()) {
      collectProgramInitializationStatement(*child, sourceUnit, enclosingClass,
                                            steps);
    }
    return;
  }
  if (const auto *owner = stmtAs<ClassDecl>(&statement)) {
    const ClassTypeInfo *info = semanticModel.findClassType(*owner);
    if (info == nullptr || !info->genericParameters.empty()) {
      return;
    }
    for (const StmtPtr &child : owner->members()) {
      collectProgramInitializationStatement(*child, sourceUnit, info->id,
                                            steps);
    }
    return;
  }
  const auto *variable = stmtAs<VariableDecl>(&statement);
  if (variable == nullptr || (enclosingClass && !variable->isStatic())) {
    return;
  }
  const BindingInfo *binding = semanticModel.findBinding(*variable);
  if (binding == nullptr || binding->symbol == 0) {
    return;
  }
  const SymbolRecord *symbol =
      semanticModel.database().findSymbol(binding->symbol);
  const SourceUnitId exactUnit =
      symbol == nullptr ? sourceUnit : symbol->sourceUnit;
  steps.push_back(
      {.sourceUnit = exactUnit,
       .kind = enclosingClass ? ProgramStorageKind::StaticField
                              : ProgramStorageKind::NamespaceGlobal,
       .role = (!variable->initializer() ||
                (variable->isConstexpr() && binding->constant &&
                 canDataInitializeProgramConstant(*binding->constant) &&
                 !requiresActiveCleanupFor(binding->type)))
                   ? ProgramInitializationStepRole::DataOnly
                   : ProgramInitializationStepRole::Initializer,
       .declaration = variable,
       .symbol = binding->symbol,
       .ownerClass = enclosingClass.value_or(0),
       .requiresActiveCleanup = requiresActiveCleanupFor(binding->type),
       .declarationSpan = tokenSpan(variable->name())});
}

auto SemanticVisitor::Impl::collectProgramEntryStatement(
    const Stmt &statement, const FunctionDecl *&entryDeclaration) const
    -> void {
  if (const auto *conditional = stmtAs<ConditionalStmt>(&statement)) {
    if (const StmtList *branch = conditional->activeBranch(target)) {
      for (const StmtPtr &child : *branch) {
        collectProgramEntryStatement(*child, entryDeclaration);
      }
    }
    return;
  }
  if (const auto *space = stmtAs<NamespaceDecl>(&statement)) {
    for (const StmtPtr &child : space->declarations()) {
      collectProgramEntryStatement(*child, entryDeclaration);
    }
    return;
  }
  if (const auto *external = stmtAs<ExternCDecl>(&statement)) {
    for (const StmtPtr &child : external->declarations()) {
      collectProgramEntryStatement(*child, entryDeclaration);
    }
    return;
  }
  if (const auto *owner = stmtAs<ClassDecl>(&statement)) {
    for (const StmtPtr &child : owner->members()) {
      collectProgramEntryStatement(*child, entryDeclaration);
    }
    return;
  }
  const auto *function = stmtAs<FunctionDecl>(&statement);
  const FunctionInfo *info =
      function == nullptr ? nullptr : semanticModel.findFunction(*function);
  if (info != nullptr && info->entryKind != ProgramEntryKind::None) {
    entryDeclaration = function;
  }
}

auto SemanticVisitor::Impl::findProgramEntryConstructor(
    const SemanticType &type, std::span<const SemanticType> parameters) const
    -> ConstructorId {
  const ClassTypeInfo *classType = semanticModel.findClassType(type.classId);
  const ClassLifecycleInfo *lifecycle =
      classType == nullptr || classType->declaration == nullptr
          ? nullptr
          : semanticModel.findClassLifecycle(*classType->declaration);
  if (lifecycle == nullptr) {
    return 0;
  }
  for (const ConstructorInfo &constructor : lifecycle->constructors) {
    if (constructor.kind == ConstructorKind::Ordinary &&
        constructor.access == AccessModifier::Public &&
        constructor.declaration != nullptr &&
        constructor.parameterTypes.size() == parameters.size() &&
        std::equal(constructor.parameterTypes.begin(),
                   constructor.parameterTypes.end(), parameters.begin())) {
      return constructor.id;
    }
  }
  return 0;
}

auto SemanticVisitor::Impl::hostedFailure(const SourceSpan &anchor,
                                          DefinedFailureCode code,
                                          DefinedFailureDetail detail)
    -> DefinedFailureOperation {
  return {.localOrigins = {{.outcomes = {{.code = code, .detail = detail}},
                            .sourceUnit = 0,
                            .start = anchor.start,
                            .end = anchor.end,
                            .line = anchor.line}}};
}

auto SemanticVisitor::Impl::finalizeHostedProgramEntry(const Program &program)
    -> void {
  const FunctionDecl *entryDeclaration = nullptr;
  for (const StmtPtr &statement : program.declarations()) {
    collectProgramEntryStatement(*statement, entryDeclaration);
  }
  const FunctionInfo *entry =
      entryDeclaration == nullptr
          ? nullptr
          : semanticModel.findFunction(*entryDeclaration);
  if (entry == nullptr || entry->entryKind == ProgramEntryKind::None) {
    semanticModel.setHostedProgramEntryPlan(std::nullopt);
    return;
  }
  HostedProgramEntryPlan plan{
      .entry = entry->id,
      .kind = entry->entryKind,
      .appendFunction = entry->entryArgumentAppendFunction,
      .sourceUnit = entry->sourceUnit,
      .mainAnchor = tokenSpan(entryDeclaration->name())};
  if (entry->entryKind == ProgramEntryKind::OwnedArguments) {
    if (entry->entryArgumentAppendFunction == 0 ||
        entry->parameterTypes.size() != 2 ||
        entry->parameterTypes[1].kind != SemanticType::Class ||
        entry->parameterTypes[1].arguments.size() != 1) {
      semanticModel.setHostedProgramEntryPlan(std::nullopt);
      return;
    }
    const SemanticType &vectorType = entry->parameterTypes[1];
    const SemanticType &stringType = vectorType.arguments.front();
    plan.vectorConstructor = findProgramEntryConstructor(vectorType, {});
    const std::array<SemanticType, 1> stringParameters{
        SemanticType::StringView};
    plan.stringConstructor =
        findProgramEntryConstructor(stringType, stringParameters);
    if (plan.vectorConstructor == 0 || plan.stringConstructor == 0) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2032", DiagnosticPhase::Semantics, plan.mainAnchor,
          "The owned-argument main entry point has no exact ordinary "
          "vector or string construction contract for hosted startup.");
      diagnostic.hints.emplace_back(
          "Use the canonical std::vector<std::string> and std::string "
          "definitions supplied by the configured standard library.");
      diagnostics.emplace_back(std::move(diagnostic));
      semanticModel.setHostedProgramEntryPlan(std::nullopt);
      return;
    }
    plan.validateCount = hostedFailure(
        plan.mainAnchor, DefinedFailureCode::HostedRuntimeContractFailure,
        DefinedFailureDetail::NegativeArgumentCount);
    plan.convertCount = hostedFailure(
        plan.mainAnchor, DefinedFailureCode::NumericConversionOutOfRange,
        DefinedFailureDetail::HostedArgumentCount);
    for (DefinedFailureOperation *operation :
         {&plan.validateCount, &plan.convertCount}) {
      operation->localOrigins.front().sourceUnit = entry->sourceUnit;
    }
  }
  semanticModel.setHostedProgramEntryPlan(std::move(plan));
}

auto SemanticVisitor::Impl::reportProgramInitializationAccess(
    const ProgramInitializationStep &initializer, const SourceSpan &primary,
    const ProgramStorageEffectUse *use, const ProgramUnknownEffect *unknown)
    -> void {
  const SymbolRecord *initializerSymbol =
      semanticModel.database().findSymbol(initializer.symbol);
  const SymbolRecord *targetSymbol =
      use == nullptr ? nullptr
                     : semanticModel.database().findSymbol(use->symbol);
  const std::string initializerName =
      initializerSymbol == nullptr ? initializer.declaration->name().lexeme
                                   : initializerSymbol->qualifiedName;
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2068", DiagnosticPhase::Semantics, primary,
      unknown != nullptr
          ? "Cannot prove that initializer for '" + initializerName +
                "' avoids uninitialized program-wide storage because " +
                unknown->reason + "."
          : "Initializer for '" + initializerName +
                "' may access program-wide storage '" +
                (targetSymbol == nullptr ? std::string("<unknown>")
                                         : targetSymbol->qualifiedName) +
                "' before its initialization step completes.");
  if (use != nullptr &&
      (use->span.source != primary.source || use->span.start != primary.start ||
       use->span.end != primary.end || use->span.line != primary.line)) {
    diagnostic.related.push_back(
        {use->span, "The transitive storage access occurs here."});
  }
  if (unknown != nullptr && (unknown->span.source != primary.source ||
                             unknown->span.start != primary.start ||
                             unknown->span.end != primary.end ||
                             unknown->span.line != primary.line)) {
    diagnostic.related.push_back(
        {unknown->span,
         "The transitive effect proof becomes incomplete here."});
  }
  if (targetSymbol != nullptr) {
    diagnostic.related.push_back(
        {targetSymbol->declarationSpan,
         "This storage is initialized at its own later program step."});
  }
  diagnostic.related.push_back({initializer.declarationSpan,
                                "The rejected initializer is declared here."});
  diagnostic.hints.emplace_back(
      "Move the dependency earlier in source-unit/declaration order or "
      "remove the access from this initializer's closed GTI call graph.");
  diagnostics.push_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::validateProgramInitializationEffects(
    const ProgramInitializationPlan &plan) -> void {
  std::unordered_map<SymbolId, ProgramInitializationStepId> stepForSymbol;
  for (const ProgramInitializationStep &step : plan.steps) {
    stepForSymbol.emplace(step.symbol, step.id);
  }
  for (const ProgramInitializationStep &initializer : plan.steps) {
    if (initializer.role == ProgramInitializationStepRole::DataOnly) {
      continue;
    }
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visiting;
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visited;
    const std::function<bool(ProgramEffectOwner, std::optional<SourceSpan>)>
        visit = [&](ProgramEffectOwner owner,
                    std::optional<SourceSpan> firstEdge) {
          if (visited.contains(owner) || !visiting.insert(owner).second) {
            return false;
          }
          const auto found = programEffects.find(owner);
          if (found != programEffects.end()) {
            const ProgramEffectSummary &summary = found->second;
            if (!summary.unknown.empty()) {
              const ProgramUnknownEffect &unknown = summary.unknown.front();
              reportProgramInitializationAccess(
                  initializer, firstEdge.value_or(unknown.span), nullptr,
                  &unknown);
              return true;
            }
            for (const ProgramStorageEffectUse &use : summary.uses) {
              const auto target = stepForSymbol.find(use.symbol);
              if (target == stepForSymbol.end() ||
                  target->second >= initializer.id) {
                reportProgramInitializationAccess(
                    initializer, firstEdge.value_or(use.span), &use, nullptr);
                return true;
              }
            }
            for (const ProgramEffectEdge &edge : summary.edges) {
              if (visit(edge.target,
                        firstEdge ? firstEdge : std::optional(edge.span))) {
                return true;
              }
            }
          }
          visiting.erase(owner);
          visited.insert(owner);
          return false;
        };
    (void)visit(
        {.kind = ProgramEffectOwnerKind::Initializer, .id = initializer.symbol},
        std::nullopt);
  }
}

auto SemanticVisitor::Impl::finalizeDeferredProgramConstantSubstitutions(
    const ProgramInitializationPlan &plan) -> void {
  std::unordered_map<SymbolId, const ConstantValue *> constants;
  for (const ProgramInitializationStep &step : plan.steps) {
    const BindingInfo *binding =
        step.declaration == nullptr
            ? nullptr
            : semanticModel.findBinding(*step.declaration);
    if (binding != nullptr && binding->constant &&
        canMaterializeProgramConstant(*binding->constant)) {
      constants.insert_or_assign(step.symbol, &*binding->constant);
    }
  }
  for (auto &[_, summary] : programEffects) {
    std::erase_if(summary.uses, [&](const ProgramStorageEffectUse &use) {
      const auto constant = constants.find(use.symbol);
      if (!use.deferredConstantCandidate || use.expression == nullptr ||
          constant == constants.end()) {
        return false;
      }
      semanticModel.recordConstant(*use.expression, *constant->second);
      semanticModel.recordProgramConstantSubstitution(*use.expression);
      return true;
    });
  }
}

auto SemanticVisitor::Impl::finalizeProgramInitialization(
    const Program &program) -> void {
  ProgramInitializationPlan plan;
  plan.unitOrder = programInitializationUnitOrder();
  if (sourceGraph == nullptr) {
    for (const StmtPtr &statement : program.declarations()) {
      collectProgramInitializationStatement(*statement, 0, std::nullopt,
                                            plan.steps);
    }
  } else {
    for (const SourceUnitId sourceUnit : plan.unitOrder) {
      const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
      if (unit == nullptr) {
        continue;
      }
      const std::size_t end =
          std::min(program.declarations().size(),
                   unit->declarationStart + unit->declarationCount);
      std::vector<ProgramInitializationStep> unitSteps;
      for (std::size_t index = unit->declarationStart; index < end; ++index) {
        collectProgramInitializationStatement(*program.declarations()[index],
                                              sourceUnit, std::nullopt,
                                              unitSteps);
      }
      std::stable_sort(unitSteps.begin(), unitSteps.end(),
                       [](const ProgramInitializationStep &left,
                          const ProgramInitializationStep &right) {
                         return left.declarationSpan.start <
                                right.declarationSpan.start;
                       });
      plan.steps.insert(plan.steps.end(), unitSteps.begin(), unitSteps.end());
    }
  }
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    plan.steps[index].id = index + 1;
  }
  finalizeDeferredProgramConstantSubstitutions(plan);
  validateProgramInitializationEffects(plan);
  semanticModel.setProgramInitializationPlan(std::move(plan));
  finalizeHostedProgramEntry(program);
}

auto SemanticVisitor::Impl::statementSourceUnit(const Stmt &statement) const
    -> SourceUnitId {
  if (const auto *conditional = stmtAs<ConditionalStmt>(&statement)) {
    return sourceUnitFor(conditional->directive());
  }
  if (const auto *classDecl = stmtAs<ClassDecl>(&statement)) {
    return sourceUnitFor(classDecl->name());
  }
  if (const auto *function = stmtAs<FunctionDecl>(&statement)) {
    return sourceUnitFor(function->name());
  }
  if (const auto *alias = stmtAs<NamespaceAliasDecl>(&statement)) {
    return sourceUnitFor(alias->name());
  }
  if (const auto *namespaceDecl = stmtAs<NamespaceDecl>(&statement)) {
    return sourceUnitFor(namespaceDecl->name());
  }
  if (const auto *alias = stmtAs<TypeAliasDecl>(&statement)) {
    return sourceUnitFor(alias->name());
  }
  if (const auto *variable = stmtAs<VariableDecl>(&statement)) {
    return sourceUnitFor(variable->name());
  }
  if (const auto *assertion = stmtAs<StaticAssertDecl>(&statement)) {
    return sourceUnitFor(assertion->keyword());
  }
  if (const auto *binding = stmtAs<StructuredBindingDecl>(&statement)) {
    return sourceUnitFor(binding->autoKeyword());
  }
  if (const auto *empty = stmtAs<EmptyStmt>(&statement)) {
    return sourceUnitFor(empty->semicolon());
  }
  if (const auto *externC = stmtAs<ExternCDecl>(&statement)) {
    return sourceUnitFor(externC->keyword());
  }
  return 0;
}

auto SemanticVisitor::Impl::analyze(const StmtList &statements) -> void {
  const std::size_t enclosingRegion = loanFlow.currentRegion;
  const std::size_t enclosingOrder = loanFlow.currentOrder;
  const Stmt *enclosingStatement = loanFlow.currentStatement;
  const std::size_t region = loanFlow.nextRegion++;
  bool reachable = true;
  bool hasUnreachableTail = false;
  ScopeStack reachableScopes;
  LoanFlowState reachableLoanFlow;
  std::vector<ValueControlFlowContext> reachableValueControlFlow;
  SemanticType reachableType = currentType;
  const bool measureStatements = measureTopLevelStatements;
  if (measureStatements) {
    measureTopLevelStatements = false;
  }
  for (std::size_t index = 0; index < statements.size(); ++index) {
    const StmtPtr &statement = statements[index];
    loanFlow.currentRegion = region;
    loanFlow.currentOrder = index + 1;
    loanFlow.currentStatement = statement.get();
    const auto statementStart = measureStatements
                                    ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    if (reachable) {
      analyze(statement);
      reachable = summarizeFlow(statement.get()).canFallThrough;
    } else {
      if (!hasUnreachableTail) {
        reachableScopes = scopes;
        reachableLoanFlow = loanFlow;
        reachableValueControlFlow = valueControlFlow;
        reachableType = currentType;
        hasUnreachableTail = true;
      }
      analyze(statement);
    }
    if (measureStatements && workMetrics != nullptr && statement != nullptr) {
      const std::size_t elapsed = static_cast<std::size_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - statementStart)
              .count());
      const SourceUnitId unit = statementSourceUnit(*statement);
      if (sourceGraph != nullptr && sourceGraph->isCompilerTrusted(unit)) {
        workMetrics->semanticTrustedBodyMicroseconds += elapsed;
      } else {
        workMetrics->semanticApplicationBodyMicroseconds += elapsed;
      }
    }
  }
  if (measureStatements) {
    measureTopLevelStatements = true;
  }
  if (hasUnreachableTail) {
    scopes = std::move(reachableScopes);
    loanFlow = std::move(reachableLoanFlow);
    valueControlFlow = std::move(reachableValueControlFlow);
    currentType = std::move(reachableType);
  }
  loanFlow.currentRegion = enclosingRegion;
  loanFlow.currentOrder = enclosingOrder;
  loanFlow.currentStatement = enclosingStatement;
}

auto SemanticVisitor::Impl::analyzeMoveUnreachable(const StmtPtr &statement)
    -> void {
  const ScopeStack reachableMoveScopes = scopes;
  const std::vector<ValueControlFlowContext> reachableValueControlFlow =
      valueControlFlow;
  const SemanticType reachableType = currentType;
  analyzeLoopBody(statement);
  applyMoveStates(reachableMoveScopes);
  valueControlFlow = reachableValueControlFlow;
  currentType = reachableType;
}

auto SemanticVisitor::Impl::analyzeMoveUnreachable(const ExprPtr &expression)
    -> SemanticType {
  const ScopeStack reachableMoveScopes = scopes;
  const std::vector<ValueControlFlowContext> reachableValueControlFlow =
      valueControlFlow;
  const SemanticType reachableType = currentType;
  const SemanticType result = analyze(expression);
  applyMoveStates(reachableMoveScopes);
  valueControlFlow = reachableValueControlFlow;
  currentType = reachableType;
  return result;
}

auto SemanticVisitor::Impl::analyzeExpectedCallableResultMoveUnreachable(
    const ExprPtr &expression, const SemanticType &expectedType)
    -> SemanticType {
  const ScopeStack reachableMoveScopes = scopes;
  const std::vector<ValueControlFlowContext> reachableValueControlFlow =
      valueControlFlow;
  const SemanticType reachableType = currentType;
  const SemanticType result =
      analyzeExpectedCallableResult(expression, expectedType);
  applyMoveStates(reachableMoveScopes);
  valueControlFlow = reachableValueControlFlow;
  currentType = reachableType;
  return result;
}

auto SemanticVisitor::Impl::analyzeConditionalArm(const StmtPtr &statement)
    -> void {
  if (!statement || stmtAs<BlockStmt>(statement.get())) {
    analyze(statement);
    return;
  }
  const std::size_t enclosingRegion = loanFlow.currentRegion;
  const std::size_t enclosingOrder = loanFlow.currentOrder;
  const Stmt *enclosingStatement = loanFlow.currentStatement;
  loanFlow.currentRegion = loanFlow.nextRegion++;
  loanFlow.currentOrder = 1;
  loanFlow.currentStatement = statement.get();
  analyze(statement);
  loanFlow.currentRegion = enclosingRegion;
  loanFlow.currentOrder = enclosingOrder;
  loanFlow.currentStatement = enclosingStatement;
}

auto SemanticVisitor::Impl::analyzeLoopBody(const StmtPtr &statement) -> void {
  if (!statement || stmtAs<BlockStmt>(statement.get())) {
    analyze(statement);
    return;
  }
  const std::size_t enclosingRegion = loanFlow.currentRegion;
  const std::size_t enclosingOrder = loanFlow.currentOrder;
  const Stmt *enclosingStatement = loanFlow.currentStatement;
  loanFlow.currentRegion = loanFlow.nextRegion++;
  loanFlow.currentOrder = 1;
  loanFlow.currentStatement = statement.get();
  analyze(statement);
  loanFlow.currentRegion = enclosingRegion;
  loanFlow.currentOrder = enclosingOrder;
  loanFlow.currentStatement = enclosingStatement;
}

auto SemanticVisitor::Impl::analyze(const StmtPtr &stmt) -> void {
  if (stmt) {
    const SourceUnitId enclosingSourceUnit = currentSourceUnit;
    const SourceUnitId statementUnit = statementSourceUnit(*stmt);
    if (statementUnit != 0) {
      currentSourceUnit = statementUnit;
    }
    stmt->accept(*this);
    currentSourceUnit = enclosingSourceUnit;
  }
}

auto SemanticVisitor::Impl::classifyExpression(const Expr &expr,
                                               SemanticType type) const
    -> ExpressionInfo {
  const ExprKind kind = expr.exprKind();
  const auto preserveCategory = [&](const Expr &source) {
    const ExpressionInfo *sourceInfo = semanticModel.findExpression(source);
    if (type.kind == SemanticType::StringView && sourceInfo != nullptr &&
        sourceInfo->traits.containsBorrowedState) {
      return ownerDependentStringViewInfo(sourceInfo->category,
                                          sourceInfo->access);
    }
    return sourceInfo == nullptr
               ? expressionInfo(std::move(type))
               : expressionInfo(std::move(type), sourceInfo->category,
                                sourceInfo->access);
  };

  if (kind == ExprKind::Grouping) {
    return preserveCategory(*static_cast<const Grouping &>(expr).expression());
  }
  if (kind == ExprKind::Binary) {
    const Binary &binary = static_cast<const Binary &>(expr);
    if (binary.oper().kind == TokenKind::COMMA) {
      return preserveCategory(*binary.right());
    }
  }
  if (kind == ExprKind::Variable) {
    const Variable &variable = static_cast<const Variable &>(expr);
    const Symbol *symbol = resolve(variable.name());
    if (symbol == nullptr && resolveValueParameter(variable.name())) {
      return expressionInfo(std::move(type));
    }
    if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                              symbol->type == SemanticType::TypeName)) {
      return expressionInfo(std::move(type));
    }
    const bool mutableAccess =
        symbol == nullptr ||
        (symbol->type.kind == SemanticType::Reference
             ? symbol->type.referenceAccess == AccessMode::Mutable
             : symbol->assignable &&
                   (symbol->ownerClass == 0 || symbol->staticMember ||
                    receiverAllowsMutation(currentReceiverMutability)));
    if (type.kind == SemanticType::StringView && symbol != nullptr &&
        symbol->retainedLoan != 0) {
      return ownerDependentStringViewInfo(ValueCategory::Place,
                                          mutableAccess ? AccessMode::Mutable
                                                        : AccessMode::ReadOnly);
    }
    return expressionInfo(std::move(type), ValueCategory::Place,
                          mutableAccess ? AccessMode::Mutable
                                        : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::QualifiedName) {
    const QualifiedName &qualified = static_cast<const QualifiedName &>(expr);
    const ResolvedEnumeratorInfo *enumerator =
        semanticModel.findEnumerator(qualified);
    if (enumerator != nullptr && enumerator->owner != 0 &&
        enumerator->owner <= enums.size() &&
        enums[enumerator->owner - 1].payload) {
      // An enumerator is a freshly produced value, not storage that must be
      // copied. This distinction is observable for a unit alternative of an
      // otherwise move-only payload enum.
      return expressionInfo(std::move(type));
    }
    const Symbol *symbol = resolveQualified(qualified.name());
    if (symbol != nullptr && (symbol->type == SemanticType::Function ||
                              symbol->type == SemanticType::TypeName)) {
      return expressionInfo(std::move(type));
    }
    return expressionInfo(std::move(type), ValueCategory::Place,
                          symbol != nullptr && symbol->assignable
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::This) {
    return expressionInfo(std::move(type), ValueCategory::Place,
                          receiverAllowsMutation(currentReceiverMutability)
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly);
  }
  const bool directOperatorResult =
      kind == ExprKind::Index || kind == ExprKind::Call ||
      (kind == ExprKind::Unary &&
       static_cast<const Unary &>(expr).oper().kind == TokenKind::STAR);
  if (directOperatorResult) {
    if (const ResolvedOperatorInfo *resolved =
            semanticModel.findOperator(expr)) {
      if (resolved->returnType.kind == SemanticType::Reference &&
          resolved->returnType.arguments.size() == 1) {
        return expressionInfo(std::move(type), ValueCategory::Place,
                              resolved->returnType.referenceAccess);
      }
      return expressionInfo(std::move(type));
    }
  }
  if (kind == ExprKind::Index) {
    const Index &index = static_cast<const Index &>(expr);
    const SemanticType *objectType = semanticModel.findType(*index.object());
    if (objectType != nullptr && objectType->kind == SemanticType::StringView) {
      return expressionInfo(std::move(type));
    }
    if (objectType != nullptr && objectType->kind == SemanticType::RawPointer) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            objectType->pointerAccess);
    }
    const ExpressionInfo *objectInfo =
        semanticModel.findExpression(*index.object());
    return expressionInfo(
        std::move(type), ValueCategory::Place,
        objectInfo != nullptr && objectInfo->category == ValueCategory::Place &&
                objectInfo->access == AccessMode::Mutable
            ? AccessMode::Mutable
            : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::Unary) {
    const Unary &unary = static_cast<const Unary &>(expr);
    if (unary.oper().kind != TokenKind::STAR) {
      return expressionInfo(std::move(type));
    }
    if (unary.rangeObject()) {
      const SemanticType *rangeType =
          semanticModel.findType(*unary.rangeObject());
      if (rangeType != nullptr && rangeType->kind == SemanticType::Array) {
        const ExpressionInfo *rangeInfo =
            semanticModel.findExpression(*unary.rangeObject());
        return expressionInfo(std::move(type), ValueCategory::Place,
                              rangeInfo != nullptr &&
                                      rangeInfo->category ==
                                          ValueCategory::Place &&
                                      rangeInfo->access == AccessMode::Mutable
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly);
      }
    }
    if (const ResolvedOperatorInfo *resolved = semanticModel.findOperator(expr);
        resolved != nullptr &&
        resolved->returnType.kind == SemanticType::Reference) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            resolved->returnType.referenceAccess);
    }
    if (const SemanticType *pointerType =
            semanticModel.findType(*unary.right());
        pointerType != nullptr &&
        pointerType->kind == SemanticType::RawPointer) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            pointerType->pointerAccess);
    }
    const ExpressionInfo *ownerInfo =
        semanticModel.findExpression(*unary.right());
    return expressionInfo(std::move(type), ValueCategory::Place,
                          ownerInfo != nullptr &&
                                  ownerInfo->access == AccessMode::Mutable
                              ? AccessMode::Mutable
                              : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::Get) {
    const Get &get = static_cast<const Get &>(expr);
    if (type == SemanticType::Function) {
      return expressionInfo(std::move(type));
    }
    const SemanticType memberObjectType = memberAccessObjectType(get);
    const MemberInfo *member = findMember(memberObjectType, get.name());
    const ExpressionInfo *objectInfo =
        semanticModel.findExpression(*get.object());
    const SemanticType *rawReceiver =
        get.access().kind == TokenKind::ARROW
            ? semanticModel.findType(*get.object())
            : nullptr;
    const bool mutableAccess =
        member == nullptr ||
        (member->symbol.type.kind == SemanticType::Reference
             ? member->symbol.type.referenceAccess == AccessMode::Mutable
             : member->symbol.assignable &&
                   (get.access().kind == TokenKind::ARROW
                        ? (rawReceiver != nullptr &&
                                   rawReceiver->kind == SemanticType::RawPointer
                               ? rawReceiver->pointerAccess ==
                                     AccessMode::Mutable
                               : memberReceiverIsMutable(get))
                        : objectInfo != nullptr &&
                              objectInfo->category == ValueCategory::Place &&
                              objectInfo->access == AccessMode::Mutable));
    return expressionInfo(std::move(type), ValueCategory::Place,
                          mutableAccess ? AccessMode::Mutable
                                        : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::Call) {
    const Call &call = static_cast<const Call &>(expr);
    if (call.callee() && call.callee()->exprKind() == ExprKind::Get) {
      const Get *member = static_cast<const Get *>(call.callee().get());
      const SemanticType *objectType =
          semanticModel.findType(*member->object());
      if (objectType != nullptr && objectType->kind == SemanticType::Expected &&
          (member->name().lexeme == "value" ||
           member->name().lexeme == "error")) {
        const ExpressionInfo *objectInfo =
            semanticModel.findExpression(*member->object());
        if (objectInfo != nullptr &&
            objectInfo->category == ValueCategory::Value) {
          // An rvalue expected owns its payload for this full expression.
          // Its value()/error() overload consumes that payload into a new
          // value instead of publishing an lvalue into the expected.
          return expressionInfo(std::move(type));
        }
        return expressionInfo(std::move(type), ValueCategory::Place,
                              objectInfo != nullptr &&
                                      objectInfo->category ==
                                          ValueCategory::Place &&
                                      objectInfo->access == AccessMode::Mutable
                                  ? AccessMode::Mutable
                                  : AccessMode::ReadOnly);
      }
    }
    const ResolvedCallInfo *resolved = semanticModel.findCall(call);
    if (resolved != nullptr &&
        resolved->returnType.kind == SemanticType::Reference &&
        resolved->returnType.arguments.size() == 1) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            resolved->returnType.referenceAccess);
    }
    if (type.kind == SemanticType::StringView && resolved != nullptr &&
        resolved->borrowOrigin != BorrowOriginKind::None) {
      if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
          resolved->borrowArgument < call.arguments().size()) {
        const ExpressionInfo *sourceInfo = semanticModel.findExpression(
            *call.arguments()[resolved->borrowArgument]);
        if (sourceInfo != nullptr &&
            sourceInfo->type.kind == SemanticType::StringView &&
            !sourceInfo->traits.containsBorrowedState) {
          return expressionInfo(std::move(type));
        }
      }
      return ownerDependentStringViewInfo();
    }
  }
  return expressionInfo(std::move(type));
}

auto SemanticVisitor::Impl::failurePropagation(CallDispatch dispatch)
    -> FailurePropagationKind {
  return dispatch == CallDispatch::Virtual ? FailurePropagationKind::VirtualCall
                                           : FailurePropagationKind::DirectCall;
}

auto SemanticVisitor::Impl::storageBoundsCheckDetail() const
    -> DefinedFailureDetail {
  if (currentClass) {
    const ClassInfo &owner = classInfo(*currentClass);
    if (isDefaultLibraryUnit(owner.sourceUnit)) {
      if (owner.name.lexeme == "vector") {
        return DefinedFailureDetail::Vector;
      }
      if (owner.name.lexeme == "string") {
        return DefinedFailureDetail::String;
      }
    }
  }
  return DefinedFailureDetail::PrivateStorage;
}

auto SemanticVisitor::Impl::addFailureOutcome(
    DefinedFailureOperation &operation, DefinedFailureCode code,
    DefinedFailureDetail detail) -> void {
  assert(!operation.localOrigins.empty());
  std::vector<DefinedFailureOutcome> &outcomes =
      operation.localOrigins.back().outcomes;
  const DefinedFailureOutcome outcome{.code = code, .detail = detail};
  if (std::find(outcomes.begin(), outcomes.end(), outcome) == outcomes.end()) {
    outcomes.push_back(outcome);
  }
}

auto SemanticVisitor::Impl::arithmeticFailureDetail(TokenKind operation)
    -> std::optional<DefinedFailureDetail> {
  switch (operation) {
  case TokenKind::PLUS:
  case TokenKind::PLUS_EQUAL:
  case TokenKind::PLUS_PLUS:
    return DefinedFailureDetail::Addition;
  case TokenKind::MINUS:
  case TokenKind::MINUS_EQUAL:
  case TokenKind::MINUS_MINUS:
    return DefinedFailureDetail::Subtraction;
  case TokenKind::STAR:
  case TokenKind::STAR_EQUAL:
    return DefinedFailureDetail::Multiplication;
  case TokenKind::SLASH:
  case TokenKind::SLASH_EQUAL:
    return DefinedFailureDetail::Division;
  default:
    return std::nullopt;
  }
}

auto SemanticVisitor::Impl::addIntegerOperationFailures(
    DefinedFailureOperation &operation, TokenKind token,
    const SemanticType &resultType, const SemanticType &countType) const
    -> void {
  if (const std::optional<DefinedFailureDetail> detail =
          arithmeticFailureDetail(token)) {
    if (token == TokenKind::SLASH || token == TokenKind::SLASH_EQUAL) {
      addFailureOutcome(operation, DefinedFailureCode::DivisionByZero,
                        DefinedFailureDetail::IntegerDivision);
      const std::optional<CheckedIntegerDomain> resultDomain =
          constantIntegerDomain(resultType);
      if (resultDomain && !resultDomain->signedValue) {
        return;
      }
    }
    addFailureOutcome(operation, DefinedFailureCode::IntegerOverflow, *detail);
    return;
  }
  if (token == TokenKind::PERCENT || token == TokenKind::PERCENT_EQUAL) {
    addFailureOutcome(operation, DefinedFailureCode::ModuloByZero,
                      DefinedFailureDetail::IntegerModulo);
    return;
  }
  if (token == TokenKind::SHIFT_LEFT || token == TokenKind::SHIFT_LEFT_EQUAL ||
      token == TokenKind::SHIFT_RIGHT ||
      token == TokenKind::SHIFT_RIGHT_EQUAL) {
    const DefinedFailureDetail detail =
        token == TokenKind::SHIFT_LEFT || token == TokenKind::SHIFT_LEFT_EQUAL
            ? DefinedFailureDetail::LeftShift
            : DefinedFailureDetail::RightShift;
    const std::optional<CheckedIntegerDomain> countDomain =
        constantIntegerDomain(countType);
    if (!countDomain || countDomain->signedValue) {
      addFailureOutcome(operation, DefinedFailureCode::NegativeShiftCount,
                        detail);
    }
    addFailureOutcome(operation, DefinedFailureCode::ShiftCountOutOfRange,
                      detail);
  }
}

auto SemanticVisitor::Impl::numericConversionMayFail(const SemanticType &source,
                                                     const SemanticType &target)
    -> bool {
  const std::optional<CheckedIntegerDomain> sourceInteger =
      constantIntegerDomain(source);
  const std::optional<CheckedIntegerDomain> targetInteger =
      constantIntegerDomain(target);
  const std::optional<BinaryFloatFormat> sourceFloat =
      semanticFloatFormat(source);
  const std::optional<BinaryFloatFormat> targetFloat =
      semanticFloatFormat(target);
  if (sourceInteger && targetInteger) {
    if (sourceInteger->signedValue == targetInteger->signedValue) {
      return sourceInteger->width > targetInteger->width;
    }
    if (!sourceInteger->signedValue && targetInteger->signedValue) {
      return sourceInteger->width >= targetInteger->width;
    }
    return true;
  }
  if (sourceFloat && targetInteger) {
    return true;
  }
  if (sourceInteger && targetFloat) {
    return false;
  }
  if (sourceFloat && targetFloat) {
    return *sourceFloat == BinaryFloatFormat::Binary64 &&
           *targetFloat == BinaryFloatFormat::Binary32;
  }
  return false;
}

auto SemanticVisitor::Impl::compoundIntermediateType(
    TokenKind operation, const SemanticType &target, const SemanticType &right,
    const Expr *leftExpression, const Expr *rightExpression) -> SemanticType {
  if (operation == TokenKind::SHIFT_LEFT_EQUAL ||
      operation == TokenKind::SHIFT_RIGHT_EQUAL) {
    return promotedInteger(target);
  }
  return numericResult(target, right, leftExpression, rightExpression);
}

auto SemanticVisitor::Impl::addCompoundConversionFailure(
    DefinedFailureOperation &operation, TokenKind token,
    const SemanticType &target, const SemanticType &right,
    const Expr *leftExpression, const Expr *rightExpression) const -> void {
  const SemanticType intermediate = compoundIntermediateType(
      token, target, right, leftExpression, rightExpression);
  if (intermediate != SemanticType::Unknown &&
      numericConversionMayFail(intermediate, target)) {
    addFailureOutcome(operation,
                      DefinedFailureCode::NumericConversionOutOfRange,
                      DefinedFailureDetail::NumericCast);
  }
}

auto SemanticVisitor::Impl::uniqueOwnerAccessDetail() const
    -> DefinedFailureDetail {
  if (currentFunctionDeclaration != nullptr &&
      currentFunctionDeclaration->operatorName() &&
      currentFunctionDeclaration->operatorName()->kind ==
          OverloadedOperator::Arrow) {
    return DefinedFailureDetail::MemberAccess;
  }
  return DefinedFailureDetail::Dereference;
}

auto SemanticVisitor::Impl::classifyDefinedFailure(
    const Expr &expr, const ExpressionInfo &info) const
    -> DefinedFailureOperation {
  DefinedFailureOperation operation;
  const ExprKind kind = expr.exprKind();
  const auto *callExpression =
      kind == ExprKind::Call ? static_cast<const Call *>(&expr) : nullptr;
  const ResolvedCallInfo *callResolution =
      callExpression == nullptr ? nullptr
                                : semanticModel.findCall(*callExpression);
  const auto anchor = [&](const Token &token) {
    const SourceSpan span = tokenSpan(token);
    operation.localOrigins.push_back(
        DefinedFailureOrigin{.sourceUnit = sourceUnitFor(token),
                             .start = span.start,
                             .end = span.end,
                             .line = span.line});
  };

  if (const ResolvedOperatorInfo *resolved = semanticModel.findOperator(expr)) {
    const bool assignmentExpression =
        kind == ExprKind::Assign || kind == ExprKind::Set ||
        kind == ExprKind::IndexSet || kind == ExprKind::DereferenceSet;
    const bool loweredAssignmentCall =
        assignmentExpression &&
        (resolved->kind == OverloadedOperator::Assignment ||
         isCompoundOperator(resolved->kind));
    if (resolved->function != 0 &&
        (!assignmentExpression || loweredAssignmentCall)) {
      operation.propagation = failurePropagation(resolved->dispatch);
    }
    return operation;
  }

  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(expr);
      construction != nullptr &&
      construction->kind == ConstructorKind::Ordinary &&
      construction->constructedType.kind == SemanticType::Class &&
      construction->constructor != 0 && !construction->generatedDefault &&
      (callResolution == nullptr ||
       callResolution->intrinsic == IntrinsicKind::None)) {
    operation.propagation = FailurePropagationKind::Constructor;
    return operation;
  }

  if (kind == ExprKind::Call) {
    const auto *call = static_cast<const Call *>(&expr);
    if (call->callee() && call->callee()->exprKind() == ExprKind::Get) {
      const auto *member = static_cast<const Get *>(call->callee().get());
      const SemanticType objectType = semanticModel.typeOf(*member->object());
      if (objectType.kind == SemanticType::Expected &&
          call->arguments().empty()) {
        if (member->name().lexeme == "value") {
          anchor(member->name());
          addFailureOutcome(operation,
                            DefinedFailureCode::InvalidExpectedAccess,
                            DefinedFailureDetail::ValueOnError);
          return operation;
        }
        if (member->name().lexeme == "error") {
          anchor(member->name());
          addFailureOutcome(operation,
                            DefinedFailureCode::InvalidExpectedAccess,
                            DefinedFailureDetail::ErrorOnValue);
          return operation;
        }
      }
    }

    if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call)) {
      const IntrinsicKind intrinsic = resolved->intrinsic;
      if (intrinsic != IntrinsicKind::None) {
        const Token token = callableToken(call->callee());
        switch (intrinsic) {
        case IntrinsicKind::NumericTypeParameterConversion:
        case IntrinsicKind::NumericAliasConversion:
          if (!call->arguments().empty() &&
              numericConversionMayFail(
                  semanticModel.typeOf(*call->arguments().front()),
                  info.type)) {
            anchor(token);
            addFailureOutcome(operation,
                              DefinedFailureCode::NumericConversionOutOfRange,
                              DefinedFailureDetail::NumericCast);
          }
          break;
        case IntrinsicKind::AllocateUniqueOwner:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::UniqueOwner);
          break;
        case IntrinsicKind::UniqueOwnerBorrow:
        case IntrinsicKind::UniqueOwnerBorrowMut:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::EmptyOwnerAccess,
                            uniqueOwnerAccessDetail());
          break;
        case IntrinsicKind::AllocateStorage:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::PrivateStorage);
          break;
        case IntrinsicKind::StorageConstruct:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::DuplicateConstruction);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::ElementConstruction);
          break;
        case IntrinsicKind::StorageRead:
        case IntrinsicKind::StorageReadMut:
        case IntrinsicKind::StorageDestroy:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::UninitializedAccess);
          break;
        case IntrinsicKind::AllocatePrefixStorage:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::PrivateStorage);
          break;
        case IntrinsicKind::PrefixStorageAppend:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::RelocationCapacity);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::ElementConstruction);
          break;
        case IntrinsicKind::PrefixStoragePop:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::UninitializedAccess);
          break;
        case IntrinsicKind::PrefixStorageRead:
        case IntrinsicKind::PrefixStorageReadMut:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          break;
        case IntrinsicKind::PrefixStorageRelocate:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::RelocationCapacity);
          addFailureOutcome(
              operation, DefinedFailureCode::InvalidStorageState,
              DefinedFailureDetail::OccupiedRelocationDestination);
          break;
        case IntrinsicKind::PrefixStorageInsert:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::RelocationCapacity);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::ElementConstruction);
          break;
        case IntrinsicKind::PrefixStorageErase:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          break;
        case IntrinsicKind::StorageTryAllocate:
        case IntrinsicKind::StorageCapacity:
        case IntrinsicKind::StorageInitialized:
        case IntrinsicKind::PrefixStorageLength:
        case IntrinsicKind::PrefixStorageTextView:
        case IntrinsicKind::StringViewSize:
        case IntrinsicKind::StringViewEmpty:
        // The expected extractions' defined-failure outcomes anchor in
        // the member-call analysis that has always owned them; the
        // resolved record only carries the spelling identity.
        case IntrinsicKind::ExpectedValue:
        case IntrinsicKind::ExpectedError:
        case IntrinsicKind::ExpectedValueOr:
        case IntrinsicKind::ArrayRangeBegin:
        case IntrinsicKind::ArraySize:
        case IntrinsicKind::ExpectedHasValue:
        case IntrinsicKind::ThreadHandleEmpty:
        case IntrinsicKind::ThreadSpawn:
        case IntrinsicKind::ThreadJoinable:
        case IntrinsicKind::AtomicLoad:
        case IntrinsicKind::AtomicStore:
        case IntrinsicKind::AtomicExchange:
        case IntrinsicKind::AtomicCompareExchange:
        case IntrinsicKind::MutexLock:
        case IntrinsicKind::MutexUnlock:
        case IntrinsicKind::MutexGuardActive:
        case IntrinsicKind::MutexGuardRead:
        case IntrinsicKind::MutexGuardReadMut:
          break;
        case IntrinsicKind::MutexLockFailure:
          anchor(token);
          addFailureOutcome(operation,
                            DefinedFailureCode::InfallibleHostOperationFailed,
                            DefinedFailureDetail::MutexLock);
          break;
        case IntrinsicKind::ThreadJoin:
          operation.propagation = FailurePropagationKind::TaskJoin;
          break;
        case IntrinsicKind::ThreadAutoJoin:
          anchor(token);
          addFailureOutcome(operation,
                            DefinedFailureCode::InfallibleHostOperationFailed,
                            DefinedFailureDetail::AutomaticJoin);
          operation.propagation = FailurePropagationKind::TaskJoin;
          break;
        case IntrinsicKind::StorageBoundsCheck:
          // The identity-bound public logical-size check (P-STORAGE-01):
          // container indexing reports the logical bound as
          // index_out_of_bounds, named for the enclosing trusted
          // container rather than the private storage invariant.
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            storageBoundsCheckDetail());
          break;
        case IntrinsicKind::StorageRelocate:
        case IntrinsicKind::StorageShiftRight:
        case IntrinsicKind::StorageShiftLeft:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::RelocationCapacity);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::InvalidRelocationSource);
          addFailureOutcome(
              operation, DefinedFailureCode::InvalidStorageState,
              DefinedFailureDetail::OccupiedRelocationDestination);
          break;
        case IntrinsicKind::DefaultTypeParameterConstruction:
        case IntrinsicKind::Move:
        case IntrinsicKind::UniqueOwnerIsNull:
        case IntrinsicKind::UniqueOwnerUpcast:
        case IntrinsicKind::IntegerWrappingAdd:
        case IntrinsicKind::IntegerWrappingSubtract:
        case IntrinsicKind::IntegerWrappingMultiply:
        case IntrinsicKind::IntegerSaturatingAdd:
        case IntrinsicKind::IntegerSaturatingSubtract:
        case IntrinsicKind::IntegerSaturatingMultiply:
        case IntrinsicKind::IntegerCheckedAdd:
        case IntrinsicKind::IntegerCheckedSubtract:
        case IntrinsicKind::IntegerCheckedMultiply:
        case IntrinsicKind::None:
        case IntrinsicKind::Count:
          break;
        }
        return operation;
      }
      const FunctionInfo *target =
          semanticModel.findFunction(resolved->function);
      if (target != nullptr && target->linkage == LanguageLinkage::Gti) {
        operation.propagation = failurePropagation(resolved->dispatch);
      }
      return operation;
    }
    if (semanticModel.findLambdaCall(*call) != nullptr ||
        semanticModel.findDeferredCallableCall(*call) != nullptr) {
      operation.propagation = FailurePropagationKind::Callable;
      return operation;
    }
  }

  if (kind == ExprKind::Conversion) {
    const auto *conversion = static_cast<const Conversion *>(&expr);
    if (numericConversionMayFail(semanticModel.typeOf(*conversion->value()),
                                 info.type)) {
      anchor(conversion->targetType().name.first());
      addFailureOutcome(operation,
                        DefinedFailureCode::NumericConversionOutOfRange,
                        DefinedFailureDetail::NumericCast);
    }
    return operation;
  }

  const auto addIndexFailure = [&](const ExprPtr &object,
                                   const Token &bracket) {
    const SemanticType objectType = semanticModel.typeOf(*object);
    if (objectType.kind == SemanticType::Array) {
      anchor(bracket);
      addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                        DefinedFailureDetail::FixedArray);
    } else if (objectType.kind == SemanticType::StringView) {
      anchor(bracket);
      addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                        DefinedFailureDetail::StringView);
    }
  };
  if (kind == ExprKind::Index) {
    const auto *index = static_cast<const Index *>(&expr);
    addIndexFailure(index->object(), index->bracket());
    return operation;
  }

  if (kind == ExprKind::Binary) {
    const auto *binary = static_cast<const Binary *>(&expr);
    const SemanticType leftType = semanticModel.typeOf(*binary->left());
    const SemanticType rightType = semanticModel.typeOf(*binary->right());
    // Raw-pointer difference is an unsafe pointer operation whose
    // same-allocation and representability preconditions belong to the
    // programmer's raw-memory proof. Its int64_t result does not turn the
    // operation into GTI checked integer subtraction.
    const bool rawPointerOperation =
        leftType.kind == SemanticType::RawPointer ||
        rightType.kind == SemanticType::RawPointer;
    if (isIntegral(info.type) && !rawPointerOperation) {
      anchor(binary->oper());
      addIntegerOperationFailures(operation, binary->oper().kind, info.type,
                                  rightType);
    }
  } else if (kind == ExprKind::Unary) {
    const auto *unary = static_cast<const Unary *>(&expr);
    if (isIntegral(info.type)) {
      if (unary->oper().kind == TokenKind::MINUS) {
        anchor(unary->oper());
        const std::optional<IntegerConstant> constant = integerConstant(unary);
        const int width = integerRank(info.type);
        const bool exactSignedMinimum =
            isSignedInteger(info.type) && width > 0 && constant &&
            constant->negative &&
            constant->magnitude ==
                (std::uint64_t{1} << (static_cast<unsigned>(width) - 1U));
        if (!exactSignedMinimum) {
          addFailureOutcome(operation, DefinedFailureCode::IntegerOverflow,
                            DefinedFailureDetail::Negation);
        }
      } else if (unary->oper().kind == TokenKind::PLUS_PLUS ||
                 unary->oper().kind == TokenKind::MINUS_MINUS) {
        anchor(unary->oper());
        const SemanticType operandType = semanticModel.typeOf(*unary->right());
        addIntegerOperationFailures(operation, unary->oper().kind,
                                    promotedInteger(operandType));
        if (numericConversionMayFail(promotedInteger(info.type), info.type)) {
          addFailureOutcome(operation,
                            DefinedFailureCode::NumericConversionOutOfRange,
                            DefinedFailureDetail::NumericCast);
        }
      }
    }
  } else if (kind == ExprKind::Postfix) {
    const auto *postfix = static_cast<const Postfix *>(&expr);
    if (isIntegral(info.type)) {
      anchor(postfix->oper());
      const SemanticType operandType =
          semanticModel.typeOf(*postfix->expression());
      addIntegerOperationFailures(operation, postfix->oper().kind,
                                  promotedInteger(operandType));
      if (numericConversionMayFail(promotedInteger(info.type), info.type)) {
        addFailureOutcome(operation,
                          DefinedFailureCode::NumericConversionOutOfRange,
                          DefinedFailureDetail::NumericCast);
      }
    }
  } else if (kind == ExprKind::Assign) {
    const auto *assign = static_cast<const Assign *>(&expr);
    if (assign->oper().kind != TokenKind::EQUAL && isIntegral(info.type)) {
      anchor(assign->oper());
      const SemanticType right = semanticModel.typeOf(*assign->value());
      const SemanticType intermediate =
          compoundIntermediateType(assign->oper().kind, info.type, right,
                                   nullptr, assign->value().get());
      addIntegerOperationFailures(operation, assign->oper().kind, intermediate,
                                  right);
      addCompoundConversionFailure(operation, assign->oper().kind, info.type,
                                   right, nullptr, assign->value().get());
    }
  } else if (kind == ExprKind::Set) {
    const auto *set = static_cast<const Set *>(&expr);
    if (set->oper().kind != TokenKind::EQUAL && isIntegral(info.type)) {
      anchor(set->oper());
      const SemanticType right = semanticModel.typeOf(*set->value());
      const SemanticType intermediate = compoundIntermediateType(
          set->oper().kind, info.type, right, nullptr, set->value().get());
      addIntegerOperationFailures(operation, set->oper().kind, intermediate,
                                  right);
      addCompoundConversionFailure(operation, set->oper().kind, info.type,
                                   right, nullptr, set->value().get());
    }
  } else if (kind == ExprKind::IndexSet) {
    const auto *set = static_cast<const IndexSet *>(&expr);
    addIndexFailure(set->object(), set->bracket());
    if (set->oper().kind != TokenKind::EQUAL && isIntegral(info.type)) {
      anchor(set->oper());
      const SemanticType right = semanticModel.typeOf(*set->value());
      const SemanticType intermediate = compoundIntermediateType(
          set->oper().kind, info.type, right, nullptr, set->value().get());
      addIntegerOperationFailures(operation, set->oper().kind, intermediate,
                                  right);
      addCompoundConversionFailure(operation, set->oper().kind, info.type,
                                   right, nullptr, set->value().get());
    }
  } else if (kind == ExprKind::DereferenceSet) {
    const auto *set = static_cast<const DereferenceSet *>(&expr);
    if (set->oper().kind != TokenKind::EQUAL && isIntegral(info.type)) {
      anchor(set->oper());
      const SemanticType right = semanticModel.typeOf(*set->value());
      const SemanticType intermediate = compoundIntermediateType(
          set->oper().kind, info.type, right, nullptr, set->value().get());
      addIntegerOperationFailures(operation, set->oper().kind, intermediate,
                                  right);
      addCompoundConversionFailure(operation, set->oper().kind, info.type,
                                   right, nullptr, set->value().get());
    }
  }

  std::erase_if(operation.localOrigins, [](const DefinedFailureOrigin &origin) {
    return origin.outcomes.empty();
  });
  for (DefinedFailureOrigin &origin : operation.localOrigins) {
    std::sort(origin.outcomes.begin(), origin.outcomes.end(),
              [](DefinedFailureOutcome left, DefinedFailureOutcome right) {
                if (left.code != right.code) {
                  return left.code < right.code;
                }
                return left.detail < right.detail;
              });
  }
  return operation;
}

auto SemanticVisitor::Impl::analyze(const Expr &expr) -> SemanticType {
  return analyze(expr, OccurrenceRole::Reference | OccurrenceRole::Read);
}

auto SemanticVisitor::Impl::analyze(const Expr &expr,
                                    OccurrenceRole requestedRoles)
    -> SemanticType {
  if (instanceAnalysisActive) {
    semanticModel.suppressInheritedOperator(expr);
  }
  expr.accept(*this);
  SemanticType result = currentType;
  ExpressionInfo info = classifyExpression(expr, result);
  semanticModel.record(expr, info);
  semanticModel.recordDefinedFailure(expr, classifyDefinedFailure(expr, info));
  if (const SemanticPlace place = semanticPlace(&expr); place.root != nullptr) {
    semanticModel.recordPlace(expr, placeKey(place));
  }
  validateProjectedPlaceRead(expr);
  validateLoanPlaceAccess(expr, requestedRoles);
  Token token = expressionToken(expr);
  const ExprKind kind = expr.exprKind();
  SemanticBindingKind bindingKind = SemanticBindingKind::None;
  bool mutableBinding = false;
  bool staticMember = false;
  SymbolId symbolId = 0;
  OccurrenceRole roles = requestedRoles;
  if (kind == ExprKind::Variable) {
    const Variable &variable = static_cast<const Variable &>(expr);
    if (const Symbol *symbol = resolve(variable.name())) {
      bindingKind = symbol->bindingKind;
      mutableBinding = symbol->assignable;
      staticMember = symbol->staticMember || symbol->internalLinkage;
      symbolId = toolingSymbolFor(*symbol);
      if (const ConstantValue *constant = constantFor(*symbol)) {
        semanticModel.recordConstant(expr, *constant);
      }
    } else if (const std::optional<CompileTimeValue> value =
                   resolveValueParameter(variable.name());
               value) {
      if (value->kind == CompileTimeValue::Parameter) {
        const GenericParameterInfo *parameter =
            genericParameterInfo(value->parameterId);
        if (parameter != nullptr) {
          symbolId = symbolForDeclaration(parameter->name);
        }
      } else if (value->kind == CompileTimeValue::UInt64) {
        semanticModel.recordConstant(
            expr, ConstantInteger{.magnitude = value->value,
                                  .domain = CheckedIntegerDomain{.width = 64}});
      }
    }
  } else if (kind == ExprKind::PackExpansion) {
    const PackExpansion &pack = static_cast<const PackExpansion &>(expr);
    token = pack.name();
    if (const Symbol *symbol = resolve(pack.name())) {
      bindingKind = symbol->bindingKind;
      mutableBinding = symbol->assignable;
      staticMember = symbol->staticMember || symbol->internalLinkage;
      symbolId = toolingSymbolFor(*symbol);
    }
  } else if (kind == ExprKind::PackFold) {
    token = static_cast<const PackFold &>(expr).ellipsis();
  } else if (kind == ExprKind::QualifiedName) {
    const QualifiedName &qualified = static_cast<const QualifiedName &>(expr);
    if (const Symbol *symbol = resolveQualified(qualified.name())) {
      bindingKind = symbol->bindingKind;
      mutableBinding = symbol->assignable;
      staticMember = symbol->staticMember || symbol->internalLinkage;
      symbolId = toolingSymbolFor(*symbol);
      if (const ConstantValue *constant = constantFor(*symbol)) {
        semanticModel.recordConstant(expr, *constant);
      }
    }
    if (const ResolvedEnumeratorInfo *enumerator =
            semanticModel.findEnumerator(qualified);
        enumerator != nullptr && enumerator->declaration != nullptr) {
      symbolId = symbolForDeclaration(enumerator->declaration->name);
      if (symbolId == 0) {
        const EnumTypeInfo *owner =
            semanticModel.findEnumType(enumerator->owner);
        const SemanticType enumType = SemanticType::enumType(enumerator->owner);
        symbolId = recordToolingSymbol(
            enumerator->declaration->name, SymbolKind::Enumerator,
            (owner == nullptr ? std::string{} : owner->qualifiedName + "::") +
                enumerator->declaration->name.lexeme,
            enumType);
      }
    }
  } else if (kind == ExprKind::Get) {
    const Get &member = static_cast<const Get &>(expr);
    if (const MemberInfo *resolved =
            findMember(memberAccessObjectType(member), member.name())) {
      const Symbol substituted =
          substituteSymbol(resolved->symbol, memberAccessObjectType(member));
      bindingKind = substituted.bindingKind;
      mutableBinding = substituted.assignable;
      staticMember = substituted.staticMember || substituted.internalLinkage;
      if (isCompilerTrustedRequester(member.name()) ||
          !substituted.compilerPrivate) {
        symbolId = toolingSymbolFor(substituted);
      }
    }
  } else if (kind == ExprKind::Assign) {
    const Assign &assignment = static_cast<const Assign &>(expr);
    token = assignment.name();
    roles = OccurrenceRole::Reference | OccurrenceRole::Write;
    if (assignment.oper().kind != TokenKind::EQUAL) {
      roles |= OccurrenceRole::Read;
    }
    const Symbol *symbol = assignment.path().segments.size() > 1
                               ? resolveQualified(assignment.path())
                               : resolve(assignment.name());
    if (symbol != nullptr) {
      bindingKind = symbol->bindingKind;
      mutableBinding = symbol->assignable;
      staticMember = symbol->staticMember || symbol->internalLinkage;
      symbolId = toolingSymbolFor(*symbol);
    }
  } else if (kind == ExprKind::Set) {
    const Set &set = static_cast<const Set &>(expr);
    roles = OccurrenceRole::Reference | OccurrenceRole::Write;
    if (set.oper().kind != TokenKind::EQUAL) {
      roles |= OccurrenceRole::Read;
    }
    if (const MemberInfo *resolved =
            findMember(memberAccessObjectType(set), set.name())) {
      bindingKind = resolved->symbol.bindingKind;
      mutableBinding = resolved->symbol.assignable;
      staticMember =
          resolved->symbol.staticMember || resolved->symbol.internalLinkage;
      symbolId = toolingSymbolFor(resolved->symbol);
    }
  }
  const FunctionDecl *functionItemDeclaration = nullptr;
  if (const FunctionItemSelectionInfo *item =
          semanticModel.findFunctionItemSelection(expr);
      item != nullptr && item->declaration != nullptr) {
    functionItemDeclaration = item->declaration;
    symbolId = symbolForDeclaration(functionItemDeclaration->name());
  }
  semanticModel.recordResolvedSymbol(expr, symbolId);
  recordProgramStorageEffect(expr, bindingKind, symbolId, roles);
  recordProgramCallEffects(expr);
  if (info.category == ValueCategory::Value &&
      requiresActiveCleanupFor(info.type)) {
    recordProgramCleanupEffects(info.type, tokenSpan(expressionToken(expr)));
  }
  recordSemanticLoanUse(symbolId);
  if (!token.generated) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(token),
         .kind = functionItemDeclaration == nullptr
                     ? SemanticOccurrenceKind::Expression
                     : SemanticOccurrenceKind::Function,
         .symbol = symbolId,
         .roles = roles,
         .name = token.lexeme,
         .type = info.type,
         .traits = info.traits,
         .access = info.access,
         .mutableBinding = mutableBinding,
         .bindingKind = bindingKind,
         .function = functionItemDeclaration,
         .staticMember = staticMember});
  }
  currentType = result;
  return result;
}

auto SemanticVisitor::Impl::analyze(const ExprPtr &expr) -> SemanticType {
  return expr ? analyze(*expr) : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::analyze(const ExprPtr &expr, OccurrenceRole roles)
    -> SemanticType {
  return expr ? analyze(*expr, roles) : SemanticType::Unknown;
}

auto SemanticVisitor::Impl::analyzeExpectedCallableResult(
    const ExprPtr &expr, const SemanticType &expectedType) -> SemanticType {
  const Call *call = directCall(expr);
  if (call == nullptr) {
    return analyze(expr);
  }
  const std::optional<ContextualCallableResult> enclosingResult =
      contextualCallableResult;
  contextualCallableResult =
      ContextualCallableResult{.call = call, .type = expectedType};
  const SemanticType result = analyze(expr);
  contextualCallableResult = enclosingResult;
  return result;
}

auto SemanticVisitor::Impl::analyzeInitializer(const ExprPtr &expr,
                                               const SemanticType &expectedType,
                                               bool allowExactNullConstruction)
    -> SemanticType {
  const std::optional<SemanticType> enclosingType = contextualInitializerType;
  const Expr *enclosingExpression = contextualInitializerExpression;
  contextualInitializerType = expectedType;
  contextualInitializerExpression = expr.get();
  SemanticType result = analyzeExpectedCallableResult(expr, expectedType);
  if (allowExactNullConstruction && expr &&
      expectedType.kind == SemanticType::Class &&
      result == SemanticType::NullPtr) {
    std::vector<AnalyzedCallArgument> arguments = {
        {.type = result, .expression = &expr}};
    analyzeConstructorCallArguments(
        *expr, expectedType.classId, expectedType.arguments,
        expectedType.valueArguments, std::move(arguments),
        expressionToken(expr), true);
    result = currentType;
    const ExpressionInfo info = expressionInfo(result);
    semanticModel.record(*expr, info);
    semanticModel.recordDefinedFailure(*expr,
                                       classifyDefinedFailure(*expr, info));
  }
  contextualInitializerType = enclosingType;
  contextualInitializerExpression = enclosingExpression;
  return result;
}

auto SemanticVisitor::Impl::isDefaultLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  return isCompilerTrustedUnit(sourceUnit);
}

auto SemanticVisitor::Impl::isPreludeUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->prelude;
}

auto SemanticVisitor::Impl::isThreadLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/thread";
}

auto SemanticVisitor::Impl::isAtomicLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/atomic";
}

auto SemanticVisitor::Impl::isMutexLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/mutex";
}

auto SemanticVisitor::Impl::isMemoryLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/memory";
}

auto SemanticVisitor::Impl::isNumericLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/numeric";
}

auto SemanticVisitor::Impl::isIteratorLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/iterator";
}

auto SemanticVisitor::Impl::isFunctionalLibraryUnit(
    SourceUnitId sourceUnit) const -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/functional";
}

auto SemanticVisitor::Impl::isDefaultLibraryStorageBorrowCarrier(
    const ClassInfo &owner) const -> bool {
  return isDefaultLibraryMutexGuardCarrier(owner) ||
         (isDefaultLibraryUnit(owner.sourceUnit) && owner.storedReference &&
          owner.storedReference->type.kind == SemanticType::Reference &&
          !owner.storedReference->type.arguments.empty() &&
          (owner.storedReference->type.arguments.front().kind ==
               SemanticType::Storage ||
           owner.storedReference->type.arguments.front().kind ==
               SemanticType::PrefixStorage));
}

auto SemanticVisitor::Impl::isDefaultLibraryMutexGuardCarrier(
    const ClassInfo &owner) const -> bool {
  if (!isMutexLibraryUnit(owner.sourceUnit) || !owner.storedReference ||
      owner.namespaceScope != std::vector<std::string>{"std"} ||
      owner.name.lexeme != "mutex_guard" ||
      owner.storedReference->type.kind != SemanticType::Reference ||
      owner.storedReference->type.arguments.size() != 1) {
    return false;
  }
  const ClassInfo *storage =
      classInfo(owner.storedReference->type.arguments.front());
  return storage != nullptr && storage->compilerCapability ==
                                   CompilerCapabilityTypeKind::MutexStorage;
}

auto SemanticVisitor::Impl::isDefaultLibraryStorageBorrowParameter(
    const ClassInfo &owner, const SemanticType &parameterType) const -> bool {
  return isDefaultLibraryStorageBorrowCarrier(owner) &&
         substituteType(owner.storedReference->type,
                        instanceTypeSubstitution) == parameterType;
}

auto SemanticVisitor::Impl::memberAccess(ClassId ownerId,
                                         const Token &declaration) const
    -> AccessModifier {
  if (ownerId == 0) {
    return AccessModifier::Public;
  }
  const ClassInfo &owner = classInfo(ownerId);
  const auto member = owner.members.find(declaration.lexeme);
  return member == owner.members.end() ? AccessModifier::Public
                                       : member->second.access;
}

auto SemanticVisitor::Impl::memberAccess(const Token &declaration) const
    -> AccessModifier {
  return memberAccess(currentClass.value_or(0), declaration);
}

auto SemanticVisitor::Impl::functionAccess(const FunctionInfo *function) const
    -> AccessModifier {
  if (function == nullptr || function->ownerClass == 0 ||
      function->declaration == nullptr) {
    return AccessModifier::Public;
  }
  const ClassInfo &owner = classInfo(function->ownerClass);
  const auto member = owner.members.find(function->declaration->name().lexeme);
  if (member == owner.members.end()) {
    return AccessModifier::Public;
  }
  const auto overload =
      std::find_if(member->second.symbol.overloads.begin(),
                   member->second.symbol.overloads.end(),
                   [function](const CallableCandidate &candidate) {
                     return candidate.declaration == function->declaration;
                   });
  return overload == member->second.symbol.overloads.end()
             ? member->second.access
             : overload->access;
}

auto SemanticVisitor::Impl::constructorAccess(
    const ClassInfo &owner, const ConstructorDecl *declaration)
    -> AccessModifier {
  const auto found =
      std::find_if(owner.constructors.begin(), owner.constructors.end(),
                   [declaration](const ConstructorInfo &candidate) {
                     return candidate.declaration == declaration;
                   });
  if (found != owner.constructors.end()) {
    return found->access;
  }
  if (owner.copyConstructor &&
      owner.copyConstructor->declaration == declaration) {
    return owner.copyConstructor->access;
  }
  if (owner.moveConstructor &&
      owner.moveConstructor->declaration == declaration) {
    return owner.moveConstructor->access;
  }
  return AccessModifier::Public;
}

auto SemanticVisitor::Impl::recordToolingSymbol(
    const Token &name, SymbolKind kind, std::string qualified,
    const SemanticType &type, bool mutableBinding, bool definition,
    AccessModifier access, bool staticMember, bool internalLinkage,
    bool compilerPrivate, bool instanceDependentType) -> SymbolId {
  const SourceUnitId sourceUnit = sourceUnitFor(name);
  const SourceSpan span = tokenSpan(name);
  const std::string resolvedQualified = qualified;
  return semanticModel.recordSymbol(
      {.kind = kind,
       .name = name.lexeme,
       .qualifiedName = std::move(qualified),
       .sourceUnit = sourceUnit,
       .nameSpan = span,
       .declarationSpan = span,
       .definitionSpan =
           definition ? std::optional<SourceSpan>(span) : std::nullopt,
       .type = type,
       .traits = typeTraits(type),
       .access = access,
       .mutableBinding = mutableBinding,
       .defaultLibrary = isDefaultLibraryUnit(sourceUnit),
       .staticMember = staticMember,
       .internalLinkage = internalLinkage,
       .generated = name.generated,
       .compilerPrivate = compilerPrivate ||
                          isCompilerPrivateQualifiedName(resolvedQualified) ||
                          semanticModel.isCompilerPrivateType(type),
       .instanceDependentType = instanceDependentType});
}

auto SemanticVisitor::Impl::symbolForDeclaration(const Token &name) const
    -> SymbolId {
  return semanticModel.symbolForDeclaration(
      sourceUnitFor(name), tokenSpan(name),
      name.generated ? std::string_view{name.lexeme} : std::string_view{});
}

auto SemanticVisitor::Impl::bindingSymbolKind(SemanticBindingKind bindingKind)
    -> SymbolKind {
  switch (bindingKind) {
  case SemanticBindingKind::GlobalVariable:
    return SymbolKind::GlobalVariable;
  case SemanticBindingKind::LocalVariable:
    return SymbolKind::LocalVariable;
  case SemanticBindingKind::Parameter:
    return SymbolKind::Parameter;
  case SemanticBindingKind::Field:
  case SemanticBindingKind::StaticField:
    return SymbolKind::Field;
  case SemanticBindingKind::LambdaCapture:
    return SymbolKind::LambdaCapture;
  case SemanticBindingKind::None:
    return SymbolKind::LocalVariable;
  }
  return SymbolKind::LocalVariable;
}

auto SemanticVisitor::Impl::recordBindingSymbol(
    const Token &name, const SemanticType &type, bool mutableBinding,
    SemanticBindingKind bindingKind, ClassId ownerClass,
    std::string_view resolvedQualifiedName, bool staticMember,
    bool internalLinkage, bool instanceDependentType) -> SymbolId {
  std::string qualified = name.lexeme;
  if (bindingKind == SemanticBindingKind::GlobalVariable) {
    qualified = resolvedQualifiedName.empty()
                    ? qualifiedName(currentNamespace, name.lexeme)
                    : std::string(resolvedQualifiedName);
  } else if (bindingKind == SemanticBindingKind::Field ||
             bindingKind == SemanticBindingKind::StaticField) {
    ownerClass = ownerClass == 0 ? currentClass.value_or(0) : ownerClass;
  }
  if ((bindingKind == SemanticBindingKind::Field ||
       bindingKind == SemanticBindingKind::StaticField) &&
      ownerClass != 0) {
    const ClassInfo &owner = classInfo(ownerClass);
    qualified = qualifiedName(owner.namespaceScope,
                              owner.name.lexeme + "::" + name.lexeme);
  }
  return recordToolingSymbol(name, bindingSymbolKind(bindingKind),
                             std::move(qualified), type, mutableBinding, true,
                             (bindingKind == SemanticBindingKind::Field ||
                              bindingKind == SemanticBindingKind::StaticField)
                                 ? memberAccess(ownerClass, name)
                                 : AccessModifier::Public,
                             staticMember, internalLinkage, false,
                             instanceDependentType);
}

auto SemanticVisitor::Impl::recordFunctionSymbol(
    const FunctionDecl &declaration) -> SymbolId {
  const FunctionInfo *info = semanticModel.findFunction(declaration);
  const Token &name = declaration.operatorName()
                          ? declaration.operatorName()->symbol
                          : declaration.name();
  const bool method = info != nullptr && info->ownerClass != 0;
  const SymbolKind kind =
      declaration.operatorName()
          ? SymbolKind::Operator
          : (method ? SymbolKind::Method : SymbolKind::Function);
  return recordToolingSymbol(
      name, kind, info == nullptr ? name.lexeme : info->qualifiedName,
      info == nullptr ? SemanticType::Unknown : info->returnType, false,
      declaration.body() != nullptr, functionAccess(info),
      info != nullptr && info->staticMember,
      info != nullptr && info->internalLinkage,
      info != nullptr && info->compilerPrivate);
}

auto SemanticVisitor::Impl::toolingSymbolFor(const Symbol &symbol) -> SymbolId {
  SymbolId id = symbolForDeclaration(symbol.declaration);
  if (id != 0) {
    return id;
  }
  if (symbol.bindingKind != SemanticBindingKind::None) {
    return recordBindingSymbol(symbol.declaration, symbol.type,
                               symbol.assignable, symbol.bindingKind,
                               symbol.ownerClass, symbol.qualifiedName,
                               symbol.staticMember, symbol.internalLinkage);
  }
  if (symbol.overloads.size() == 1 &&
      symbol.overloads.front().declaration != nullptr) {
    return recordFunctionSymbol(*symbol.overloads.front().declaration);
  }
  return 0;
}

auto SemanticVisitor::Impl::genericParameterInfo(GenericParameterId id) const
    -> const GenericParameterInfo * {
  for (const ClassInfo &owner : classes) {
    const auto found = std::find_if(
        owner.genericParameters.begin(), owner.genericParameters.end(),
        [id](const GenericParameterInfo &parameter) {
          return parameter.id == id;
        });
    if (found != owner.genericParameters.end()) {
      return &*found;
    }
  }
  for (const auto &[_, parameters] : functionGenericParameters) {
    const auto found =
        std::find_if(parameters.begin(), parameters.end(),
                     [id](const GenericParameterInfo &parameter) {
                       return parameter.id == id;
                     });
    if (found != parameters.end()) {
      return &*found;
    }
  }
  for (const auto &[_, parameters] : constructorGenericParameters) {
    const auto found =
        std::find_if(parameters.begin(), parameters.end(),
                     [id](const GenericParameterInfo &parameter) {
                       return parameter.id == id;
                     });
    if (found != parameters.end()) {
      return &*found;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::recordGenericParameterUse(
    const Token &use, const CompileTimeValue &value, OccurrenceRole role)
    -> void {
  if (value.kind != CompileTimeValue::Parameter) {
    const Symbol *binding = resolve(use);
    if (binding == nullptr || !binding->constant) {
      return;
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(use),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = toolingSymbolFor(*binding),
         .roles = OccurrenceRole::Reference | role,
         .name = use.lexeme,
         .type = binding->type,
         .traits = typeTraits(binding->type),
         .access = AccessMode::ReadOnly,
         .mutableBinding = false,
         .bindingKind = binding->bindingKind,
         .staticMember = binding->staticMember || binding->internalLinkage});
    return;
  }
  if (analyzingStaticAssertCondition) {
    staticAssertGenericDependency = true;
  }
  const GenericParameterInfo *parameter =
      genericParameterInfo(value.parameterId);
  if (parameter == nullptr) {
    return;
  }
  const SymbolId symbol = symbolForDeclaration(parameter->name);
  if (symbol == 0) {
    return;
  }
  semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                  .span = tokenSpan(use),
                                  .kind = SemanticOccurrenceKind::Symbol,
                                  .symbol = symbol,
                                  .roles = OccurrenceRole::Reference | role,
                                  .name = use.lexeme,
                                  .type = SemanticType::UInt64,
                                  .traits = typeTraits(SemanticType::UInt64)});
}

auto SemanticVisitor::Impl::recordArrayExtentUses(const ArrayExtentExpr &extent)
    -> void {
  if (extent.isAtom()) {
    if (extent.token.kind != TokenKind::IDENTIFIER) {
      return;
    }
    if (const std::optional<CompileTimeValue> parameter =
            resolveValueParameter(extent.token)) {
      recordGenericParameterUse(extent.token, *parameter, OccurrenceRole::Read);
    } else {
      recordGenericParameterUse(extent.token, CompileTimeValue::uint64(0),
                                OccurrenceRole::Read);
    }
    return;
  }
  if (extent.left) {
    recordArrayExtentUses(*extent.left);
  }
  if (extent.right) {
    recordArrayExtentUses(*extent.right);
  }
}

auto SemanticVisitor::Impl::recordTypeUse(const TypeRef &type) -> void {
  if (type.name.last().kind != TokenKind::IDENTIFIER) {
    return;
  }

  if (compilerPrivatePathDenied(type.name)) {
    return;
  }
  recordQualifiedPathUses(type.name);

  SymbolId symbol = 0;
  SemanticType resolvedType = typeOf(type);
  if (const std::optional<SemanticType> parameter =
          resolveTypeParameter(type.name)) {
    if (analyzingStaticAssertCondition &&
        parameter->kind == SemanticType::TypeParameter) {
      staticAssertGenericDependency = true;
    }
    const GenericParameterInfo *info =
        genericParameterInfo(parameter->genericParameterId);
    if (info != nullptr) {
      symbol = symbolForDeclaration(info->name);
    }
  } else if (const std::optional<ResolvedTypeAlias> alias =
                 resolveTypeAliasPath(type.name, currentNamespace)) {
    if (alias->id != 0 && alias->id <= typeAliases.size()) {
      symbol = symbolForDeclaration(alias->declaration->name());
      if (symbol == 0) {
        symbol = recordToolingSymbol(alias->declaration->name(),
                                     SymbolKind::TypeAlias,
                                     alias->qualifiedName, alias->type);
      }
    }
  } else if (const std::optional<EnumId> enumId =
                 resolveEnumPath(type.name, currentNamespace)) {
    if (*enumId != 0 && *enumId <= enums.size()) {
      const EnumInfo &owner = enums[*enumId - 1];
      symbol = symbolForDeclaration(owner.name);
      if (symbol == 0) {
        symbol = recordToolingSymbol(
            owner.name, SymbolKind::Enum,
            qualifiedName(owner.namespaceScope, owner.name.lexeme),
            SemanticType::enumType(owner.id));
      }
    }
  } else if (const std::optional<ClassId> classId =
                 resolveClassPath(type.name, currentNamespace)) {
    ClassId occurrenceClass = *classId;
    const SemanticType selectedBase = baseTypeOf(type, currentNamespace);
    if (selectedBase.kind == SemanticType::Class && selectedBase.classId != 0) {
      occurrenceClass = selectedBase.classId;
    }
    const ClassInfo &owner = classInfo(occurrenceClass);
    symbol = symbolForDeclaration(owner.name);
    if (symbol == 0) {
      symbol = recordToolingSymbol(
          owner.name,
          owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                          : SymbolKind::Class,
          owner.exactSpecializationPrimary == 0
              ? qualifiedName(owner.namespaceScope, owner.name.lexeme)
              : typeSpelling(SemanticType::classType(owner.id)),
          SemanticType::classType(owner.id));
    }
  }
  if (symbol == 0) {
    return;
  }

  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(type.name.last()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
       .name = type.name.last().lexeme,
       .type = resolvedType,
       .traits = typeTraits(resolvedType)});
}

auto SemanticVisitor::Impl::recordQualifiedPathUses(
    const NamePath &path, bool finalSegmentIsNamespace) -> void {
  if (compilerPrivatePathDenied(path)) {
    return;
  }
  if (path.segments.empty() ||
      (!finalSegmentIsNamespace && path.segments.size() == 1)) {
    return;
  }
  const std::size_t namespaceSegments =
      finalSegmentIsNamespace ? path.segments.size() : path.segments.size() - 1;
  const NamePath namespacePath(std::vector<Token>(
      path.segments.begin(), path.segments.begin() + namespaceSegments));
  const std::vector<ResolvedNamespaceSegment> resolvedSegments =
      resolveNamespaceSegments(namespacePath, currentNamespace);
  for (std::size_t index = 0; index < resolvedSegments.size(); ++index) {
    const auto symbol =
        namespaceToolingSymbols.find(resolvedSegments[index].declaration);
    if (symbol == namespaceToolingSymbols.end() || symbol->second == 0) {
      continue;
    }
    const Token &token = path.segments[index];
    semanticModel.recordOccurrence({.sourceUnit = currentSourceUnit,
                                    .span = tokenSpan(token),
                                    .kind = SemanticOccurrenceKind::Symbol,
                                    .symbol = symbol->second,
                                    .roles = OccurrenceRole::Reference,
                                    .name = token.lexeme});
  }

  if (finalSegmentIsNamespace || path.segments.size() < 2) {
    return;
  }
  const NamePath ownerPath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  if (const std::optional<ClassId> classId =
          resolveClassPath(ownerPath, currentNamespace)) {
    const ClassInfo &owner = classInfo(*classId);
    SymbolId symbol = symbolForDeclaration(owner.name);
    if (symbol == 0) {
      symbol = recordToolingSymbol(
          owner.name,
          owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                          : SymbolKind::Class,
          qualifiedName(owner.namespaceScope, owner.name.lexeme),
          SemanticType::classType(owner.id));
    }
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(ownerPath.last()),
         .kind = SemanticOccurrenceKind::Symbol,
         .symbol = symbol,
         .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
         .name = ownerPath.last().lexeme,
         .type = SemanticType::classType(owner.id),
         .traits = typeTraits(SemanticType::classType(owner.id))});
    return;
  }
  const std::optional<EnumId> enumId =
      resolveEnumPath(ownerPath, currentNamespace);
  if (!enumId || *enumId == 0 || *enumId > enums.size()) {
    return;
  }
  const EnumInfo &owner = enums[*enumId - 1];
  SymbolId symbol = symbolForDeclaration(owner.name);
  if (symbol == 0) {
    symbol = recordToolingSymbol(
        owner.name, SymbolKind::Enum,
        qualifiedName(owner.namespaceScope, owner.name.lexeme),
        SemanticType::enumType(owner.id));
  }
  semanticModel.recordOccurrence(
      {.sourceUnit = currentSourceUnit,
       .span = tokenSpan(ownerPath.last()),
       .kind = SemanticOccurrenceKind::Symbol,
       .symbol = symbol,
       .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
       .name = ownerPath.last().lexeme,
       .type = SemanticType::enumType(owner.id),
       .traits = typeTraits(SemanticType::enumType(owner.id))});
}

auto SemanticVisitor::Impl::recordBindingOccurrence(
    const Token &name, const SemanticType &type, bool mutableBinding,
    SemanticBindingKind bindingKind, bool staticMember, bool internalLinkage,
    bool instanceDependentType) -> SymbolId {
  const AccessMode access =
      mutableBinding ? AccessMode::Mutable : AccessMode::ReadOnly;
  const SymbolId symbol =
      recordBindingSymbol(name, type, mutableBinding, bindingKind, 0, {},
                          staticMember, internalLinkage, instanceDependentType);
  OccurrenceRole roles = OccurrenceRole::Declaration;
  if (bindingKind != SemanticBindingKind::Parameter) {
    roles |= OccurrenceRole::Definition;
  }
  if (!name.generated) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(name),
         .kind = SemanticOccurrenceKind::Binding,
         .symbol = symbol,
         .roles = roles,
         .name = name.lexeme,
         .type = type,
         .traits = typeTraits(type),
         .access = access,
         .mutableBinding = mutableBinding,
         .bindingKind = bindingKind,
         .staticMember = staticMember || internalLinkage});
  }
  return symbol;
}

auto SemanticVisitor::Impl::genericParameterIndex(
    std::span<const GenericParameterInfo> parameters, GenericParameterId id)
    -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    if (parameters[index].id == id) {
      return index;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::sameSignatureValue(
    const CompileTimeValue &left,
    std::span<const GenericParameterInfo> leftParameters,
    const CompileTimeValue &right,
    std::span<const GenericParameterInfo> rightParameters) -> bool {
  const std::optional<std::size_t> leftParameter =
      left.kind == CompileTimeValue::Parameter
          ? genericParameterIndex(leftParameters, left.parameterId)
          : std::nullopt;
  const std::optional<std::size_t> rightParameter =
      right.kind == CompileTimeValue::Parameter
          ? genericParameterIndex(rightParameters, right.parameterId)
          : std::nullopt;
  if (leftParameter || rightParameter) {
    return leftParameter && rightParameter && *leftParameter == *rightParameter;
  }
  return left == right;
}

auto SemanticVisitor::Impl::sameSignatureType(
    const SemanticType &left,
    std::span<const GenericParameterInfo> leftParameters,
    const SemanticType &right,
    std::span<const GenericParameterInfo> rightParameters) -> bool {
  const std::optional<std::size_t> leftParameter =
      left.kind == SemanticType::TypeParameter
          ? genericParameterIndex(leftParameters, left.genericParameterId)
          : std::nullopt;
  const std::optional<std::size_t> rightParameter =
      right.kind == SemanticType::TypeParameter
          ? genericParameterIndex(rightParameters, right.genericParameterId)
          : std::nullopt;
  if (leftParameter || rightParameter) {
    return leftParameter && rightParameter && *leftParameter == *rightParameter;
  }
  const std::optional<std::size_t> leftExtent =
      left.arrayLengthParameterId == 0
          ? std::nullopt
          : genericParameterIndex(leftParameters, left.arrayLengthParameterId);
  const std::optional<std::size_t> rightExtent =
      right.arrayLengthParameterId == 0
          ? std::nullopt
          : genericParameterIndex(rightParameters,
                                  right.arrayLengthParameterId);
  if (leftExtent || rightExtent) {
    if (!leftExtent || !rightExtent || *leftExtent != *rightExtent) {
      return false;
    }
  }
  if (left.kind != right.kind || left.classId != right.classId ||
      left.genericParameterId != right.genericParameterId ||
      left.functionId != right.functionId ||
      left.arrayLength != right.arrayLength ||
      (!leftExtent && !rightExtent &&
       left.arrayLengthParameterId != right.arrayLengthParameterId) ||
      left.referenceAccess != right.referenceAccess ||
      left.pointerAccess != right.pointerAccess ||
      left.arguments.size() != right.arguments.size() ||
      left.valueArguments.size() != right.valueArguments.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.arguments.size(); ++index) {
    if (!sameSignatureType(left.arguments[index], leftParameters,
                           right.arguments[index], rightParameters)) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.valueArguments.size(); ++index) {
    if (!sameSignatureValue(left.valueArguments[index], leftParameters,
                            right.valueArguments[index], rightParameters)) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::sameFunctionSignature(
    const CallableCandidate &left, const CallableCandidate &right) -> bool {
  const bool receiverQualified = left.ownerClass != 0 && right.ownerClass != 0;
  if ((receiverQualified &&
       left.receiverMutability != right.receiverMutability) ||
      left.parameterPack != right.parameterPack ||
      left.record().genericParameters.size() !=
          right.record().genericParameters.size() ||
      left.parameterTypes.size() != right.parameterTypes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.record().genericParameters.size();
       ++index) {
    if (left.record().genericParameters[index].pack !=
            right.record().genericParameters[index].pack ||
        left.record().genericParameters[index].value !=
            right.record().genericParameters[index].value) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.parameterTypes.size(); ++index) {
    if (!sameSignatureType(
            left.parameterTypes[index], left.record().genericParameters,
            right.parameterTypes[index], right.record().genericParameters)) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::sameConstructorSignature(
    const ConstructorInfo &left, const ConstructorInfo &right) -> bool {
  if (left.genericParameters.size() != right.genericParameters.size() ||
      left.parameterTypes.size() != right.parameterTypes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.genericParameters.size(); ++index) {
    if (left.genericParameters[index].pack !=
            right.genericParameters[index].pack ||
        left.genericParameters[index].value !=
            right.genericParameters[index].value) {
      return false;
    }
  }
  for (std::size_t index = 0; index < left.parameterTypes.size(); ++index) {
    if (!sameSignatureType(left.parameterTypes[index], left.genericParameters,
                           right.parameterTypes[index],
                           right.genericParameters)) {
      return false;
    }
  }
  return true;
}

auto SemanticVisitor::Impl::appendFunctionOverload(Symbol &existing,
                                                   Symbol incoming,
                                                   const Token &name) -> bool {
  if (existing.type != SemanticType::Function ||
      incoming.type != SemanticType::Function ||
      incoming.overloads.size() != 1) {
    return false;
  }

  if (existing.ownerClass != 0 && incoming.ownerClass != 0 &&
      existing.staticMember != incoming.staticMember) {
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2039", DiagnosticPhase::Semantics, name,
        "Static and instance methods cannot share an overload set in the "
        "current class model.");
    diagnostic.related.push_back({tokenSpan(existing.declaration),
                                  "Previous method declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return true;
  }

  CallableCandidate candidate = std::move(incoming.overloads.front());
  const FunctionInfo *candidateInfo =
      candidate.declaration == nullptr
          ? nullptr
          : semanticModel.findFunction(*candidate.declaration);
  if (candidateInfo != nullptr && candidateInfo->entryPoint) {
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                       "The main entry point cannot be overloaded.");
    diagnostic.related.push_back(
        {tokenSpan(existing.overloads.front().declaration->name()),
         "Previous main declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return true;
  }

  for (const CallableCandidate &previous : existing.overloads) {
    const bool previousC =
        previous.declaration != nullptr && previous.declaration->hasCLinkage();
    const bool candidateC = candidate.declaration != nullptr &&
                            candidate.declaration->hasCLinkage();
    if (previousC != candidateC) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2054", DiagnosticPhase::Semantics, name,
          "extern \"C\" functions cannot be overloaded or share their "
          "GTI name with another function.");
      diagnostic.related.push_back({tokenSpan(previous.declaration->name()),
                                    "Previous function declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }
    if ((previous.declaration != nullptr &&
         previous.declaration->runtimeBinding()) ||
        (candidate.declaration != nullptr &&
         candidate.declaration->runtimeBinding())) {
      Diagnostic diagnostic =
          makeDiagnostic("GTI-S2011", DiagnosticPhase::Semantics, name,
                         "Runtime-bound functions cannot be overloaded.");
      diagnostic.related.push_back({tokenSpan(previous.declaration->name()),
                                    "Previous function declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }
    if (!(previousC && candidateC) &&
        sameFunctionSignature(previous, candidate)) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2011", DiagnosticPhase::Semantics, name,
          "Duplicate overload signature for '" + name.lexeme + "'.");
      diagnostic.related.push_back(
          {tokenSpan(previous.declaration->name()),
           "Previous overload with this parameter signature is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }
  }

  existing.overloads.emplace_back(std::move(candidate));
  refreshOverloadPrivacy(existing);
  return true;
}

auto SemanticVisitor::Impl::declare(const Token &name, SemanticType type,
                                    bool assignable,
                                    SemanticBindingKind bindingKind,
                                    const VariableDecl *variableDeclaration,
                                    const Parameter *parameterDeclaration,
                                    std::optional<ConstantValue> constant)
    -> bool {
  return declare(name, Symbol{.type = type,
                              .constant = std::move(constant),
                              .assignable = assignable,
                              .declaration = name,
                              .variableDeclaration = variableDeclaration,
                              .parameterDeclaration = parameterDeclaration,
                              .bindingKind = bindingKind});
}

auto SemanticVisitor::Impl::declare(const Token &name, Symbol symbol) -> bool {
  const auto existing = scopes.back().find(name.lexeme);
  if (existing != scopes.back().end()) {
    if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
      return true;
    }
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                       "Duplicate declaration of '" + name.lexeme + "'.");
    diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                  "Previous declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  scopes.back().emplace(name.lexeme, std::move(symbol));
  return true;
}

auto SemanticVisitor::Impl::declareNamespaceSymbol(
    const std::vector<std::string> &scope, const Token &name, SemanticType type,
    bool assignable, bool internalLinkage, SymbolId toolingSymbol,
    const VariableDecl *variableDeclaration,
    std::optional<ConstantValue> constant) -> bool {
  return declareNamespaceSymbol(
      scope, name,
      Symbol{.type = type,
             .constant = std::move(constant),
             .sourceUnit = currentSourceUnit,
             .assignable = assignable,
             .declaration = name,
             .variableDeclaration = variableDeclaration,
             .bindingKind = type.kind == SemanticType::TypeName
                                ? SemanticBindingKind::None
                                : SemanticBindingKind::GlobalVariable,
             .internalLinkage = internalLinkage,
             .toolingSymbol = toolingSymbol});
}

auto SemanticVisitor::Impl::declareNamespaceSymbol(
    const std::vector<std::string> &scope, const Token &name, Symbol symbol)
    -> bool {
  const std::string qualified = qualifiedName(scope, name.lexeme);
  symbol.qualifiedName = qualified;
  const bool scopePrivate =
      compilerPrivateDeclaration(symbol.sourceUnit, scope);
  symbol.compilerPrivate = symbol.compilerPrivate || scopePrivate ||
                           semanticModel.isCompilerPrivateType(symbol.type);
  for (CallableCandidate &overload : symbol.overloads) {
    overload.compilerPrivate =
        overload.compilerPrivate || symbol.compilerPrivate ||
        semanticModel.isCompilerPrivateType(overload.returnType) ||
        std::any_of(overload.parameterTypes.begin(),
                    overload.parameterTypes.end(),
                    [this](const SemanticType &type) {
                      return semanticModel.isCompilerPrivateType(type);
                    });
  }
  if (symbol.type == SemanticType::Function) {
    symbol.compilerPrivate =
        scopePrivate ||
        (!symbol.overloads.empty() &&
         std::all_of(symbol.overloads.begin(), symbol.overloads.end(),
                     [](const CallableCandidate &overload) {
                       return overload.compilerPrivate;
                     }));
  }
  const bool sourceLocal = symbol.internalLinkage && sourceGraph != nullptr &&
                           currentSourceUnit != 0;
  bool categoryConflict =
      namespaces.contains(qualified) || namespaceAliases.contains(qualified) ||
      typeAliasIds.contains(qualified) || enumIds.contains(qualified) ||
      conceptIds.contains(qualified);
  if (sourceLocal) {
    categoryConflict = namespaceIsVisible(qualified) ||
                       findNamespaceAlias(qualified) != nullptr ||
                       currentTypeAliasIds().contains(qualified) ||
                       currentEnumIds().contains(qualified) ||
                       currentConceptIds().contains(qualified);
  }
  if (categoryConflict) {
    report(name, "Duplicate declaration of '" + name.lexeme + "'.");
    return false;
  }

  if (sourceLocal) {
    auto &internal = activeInternalNamespaceSymbols();
    auto &unitSymbols = internal[currentSourceUnit];
    const auto existing = unitSymbols.find(qualified);
    if (existing != unitSymbols.end()) {
      const Symbol published = symbol;
      if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
        publishNamespaceSymbol(qualified, published);
        return true;
      }
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2006", DiagnosticPhase::Semantics, name,
          "Duplicate static declaration of '" + name.lexeme + "'.");
      diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                    "Previous static declaration is here."});
      diagnostics.emplace_back(std::move(diagnostic));
      return false;
    }
    const auto &namespaceIndex = activeNamespaceSymbols();
    if (const auto external = namespaceIndex.find(qualified);
        external != namespaceIndex.end() &&
        sourceVisible(declarationSourceUnit(external->second))) {
      report(name,
             "Static declaration of '" + name.lexeme +
                 "' conflicts with a visible namespace declaration.",
             "GTI-S2006");
      return false;
    }
    publishNamespaceSymbol(qualified, symbol);
    unitSymbols.emplace(qualified, std::move(symbol));
    return true;
  }

  auto &internal = activeInternalNamespaceSymbols();
  if (const auto unit = internal.find(currentSourceUnit);
      unit != internal.end() && unit->second.contains(qualified)) {
    report(name,
           "Declaration of '" + name.lexeme +
               "' conflicts with a static declaration in this source "
               "file.",
           "GTI-S2006");
    return false;
  }

  auto &namespaceIndex = activeNamespaceSymbols();
  const auto existing = namespaceIndex.find(qualified);
  if (existing != namespaceIndex.end()) {
    const Symbol published = symbol;
    if (appendFunctionOverload(existing->second, std::move(symbol), name)) {
      publishNamespaceSymbol(qualified, published);
      return true;
    }
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2006", DiagnosticPhase::Semantics, name,
                       "Duplicate declaration of '" + name.lexeme + "'.");
    diagnostic.related.push_back({tokenSpan(existing->second.declaration),
                                  "Previous declaration is here."});
    diagnostics.emplace_back(std::move(diagnostic));
    return false;
  }
  publishNamespaceSymbol(qualified, symbol);
  namespaceIndex.emplace(qualified, std::move(symbol));
  return true;
}

auto SemanticVisitor::Impl::directNamespaceChild(std::string_view qualified,
                                                 std::string_view parent,
                                                 std::string &name) -> bool {
  std::string_view remainder = qualified;
  if (!parent.empty()) {
    if (!qualified.starts_with(parent) ||
        qualified.size() <= parent.size() + 2 ||
        qualified.substr(parent.size(), 2) != "::") {
      return false;
    }
    remainder.remove_prefix(parent.size() + 2);
  }
  if (remainder.empty() || remainder.find("::") != std::string_view::npos) {
    return false;
  }
  name = std::string(remainder);
  return true;
}

auto SemanticVisitor::Impl::completionKind(const Symbol &symbol) const
    -> SemanticCompletionCandidateKind {
  if (symbol.type == SemanticType::Function) {
    return symbol.ownerClass == 0 ? SemanticCompletionCandidateKind::Function
                                  : SemanticCompletionCandidateKind::Method;
  }
  if (symbol.type.kind == SemanticType::TypeName) {
    const ClassInfo *owner =
        classInfo(SemanticType::classType(symbol.type.classId));
    return owner != nullptr && owner->kind == ClassKind::Struct
               ? SemanticCompletionCandidateKind::Struct
               : SemanticCompletionCandidateKind::Class;
  }
  switch (symbol.bindingKind) {
  case SemanticBindingKind::GlobalVariable:
    return SemanticCompletionCandidateKind::GlobalVariable;
  case SemanticBindingKind::Parameter:
    return SemanticCompletionCandidateKind::Parameter;
  case SemanticBindingKind::Field:
  case SemanticBindingKind::StaticField:
    return SemanticCompletionCandidateKind::Field;
  case SemanticBindingKind::LocalVariable:
  case SemanticBindingKind::LambdaCapture:
    return SemanticCompletionCandidateKind::LocalVariable;
  case SemanticBindingKind::None:
    return symbol.ownerClass == 0
               ? SemanticCompletionCandidateKind::LocalVariable
               : SemanticCompletionCandidateKind::Field;
  }
  return SemanticCompletionCandidateKind::LocalVariable;
}

auto SemanticVisitor::Impl::appendSymbolCompletions(
    std::vector<SemanticCompletionCandidateRecord> &result,
    const std::string &name, const std::string &qualifiedName,
    const Symbol &symbol, std::size_t scopeDistance, bool substitutedCallable,
    bool mutableReceiver) const -> void {
  if (name.rfind("__gti_", 0) == 0 ||
      symbol.valueState != ValueState::Available ||
      (!isCompilerTrustedUnit(currentSourceUnit) &&
       (symbol.compilerPrivate ||
        semanticModel.isCompilerPrivateType(symbol.type)))) {
    return;
  }
  if (currentStaticMemberFunction && symbol.ownerClass != 0 &&
      !symbol.staticMember) {
    return;
  }
  if (symbol.type == SemanticType::Function) {
    for (const CallableCandidate &overload : symbol.overloads) {
      if (!isCompilerTrustedUnit(currentSourceUnit) &&
          (overload.compilerPrivate ||
           semanticModel.isCompilerPrivateType(overload.returnType) ||
           std::any_of(overload.parameterTypes.begin(),
                       overload.parameterTypes.end(),
                       [this](const SemanticType &type) {
                         return semanticModel.isCompilerPrivateType(type);
                       }))) {
        continue;
      }
      if (overload.access == AccessModifier::Private &&
          currentClass != overload.ownerClass) {
        continue;
      }
      if (!mutableReceiver &&
          overload.receiverMutability == ReceiverMutability::Mutable) {
        continue;
      }
      result.push_back(
          {.kind = overload.ownerClass == 0
                       ? SemanticCompletionCandidateKind::Function
                       : SemanticCompletionCandidateKind::Method,
           .name = name,
           .qualifiedName = qualifiedName,
           .type = overload.returnType,
           .scopeDistance = scopeDistance,
           .function = overload.id,
           .parameterTypes = overload.parameterTypes,
           .requirements = overload.requirements,
           .substitutedCallable = substitutedCallable,
           .staticMember =
               overload.staticMember || overload.record().internalLinkage,
           // An overload set shares one scope Symbol; each overload's own
           // SymbolRecord — the identity carrying its retained `///`
           // documentation — resolves through its declaration name.
           .symbol = overload.declaration == nullptr
                         ? symbol.toolingSymbol
                         : symbolForDeclaration(overload.declaration->name()),
           .compilerPrivate = overload.compilerPrivate});
    }
    return;
  }

  SemanticCompletionCandidateRecord candidate{
      .kind = completionKind(symbol),
      .name = name,
      .qualifiedName = qualifiedName,
      .type = symbol.type,
      .mutableBinding = symbol.assignable,
      .scopeDistance = scopeDistance,
      .staticMember = symbol.staticMember || symbol.internalLinkage,
      .symbol = symbol.toolingSymbol,
      .compilerPrivate = symbol.compilerPrivate};
  if (symbol.type.kind == SemanticType::TypeName) {
    candidate.classType = symbol.type.classId;
  }
  result.push_back(std::move(candidate));
}

auto SemanticVisitor::Impl::appendNamespaceChildren(
    std::vector<SemanticCompletionCandidateRecord> &result,
    const std::string &parent, std::size_t scopeDistance,
    std::unordered_set<std::string> *seen) const -> void {
  const auto appendName = [&](const std::string &name) {
    return seen == nullptr || seen->insert(name).second;
  };
  for (const auto &[qualified, symbol] : currentNamespaceSymbols()) {
    std::string name;
    if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
      appendSymbolCompletions(result, name, qualified, symbol, scopeDistance);
    }
  }
  for (const auto &[qualified, id] : currentEnumIds()) {
    std::string name;
    if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
      result.push_back({.kind = SemanticCompletionCandidateKind::Enum,
                        .name = name,
                        .qualifiedName = qualified,
                        .type = SemanticType::enumType(id),
                        .scopeDistance = scopeDistance,
                        .enumType = id});
    }
  }
  for (const auto &[qualified, id] : currentTypeAliasIds()) {
    std::string name;
    if (!directNamespaceChild(qualified, parent, name) || !appendName(name) ||
        id == 0 || id > typeAliases.size()) {
      continue;
    }
    const RegisteredTypeAlias &alias = typeAliases[id - 1];
    if (!isCompilerTrustedUnit(currentSourceUnit) &&
        (alias.compilerPrivate ||
         semanticModel.isCompilerPrivateType(alias.type))) {
      continue;
    }
    result.push_back({.kind = SemanticCompletionCandidateKind::TypeAlias,
                      .name = name,
                      .qualifiedName = qualified,
                      .type = alias.type,
                      .scopeDistance = scopeDistance,
                      .typeAlias = alias.declaration,
                      .symbol = symbolForDeclaration(alias.declaration->name()),
                      .compilerPrivate = alias.compilerPrivate});
  }

  const auto &visibleNamespaceSet = [&]() -> const auto & {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return namespaces;
    }
    const auto found = visibleNamespaces.find(currentSourceUnit);
    if (found != visibleNamespaces.end()) {
      return found->second;
    }
    static const std::unordered_set<std::string> empty;
    return empty;
  }();
  for (const std::string &qualified : visibleNamespaceSet) {
    std::string name;
    if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
      result.push_back({.kind = SemanticCompletionCandidateKind::Namespace,
                        .name = name,
                        .qualifiedName = qualified,
                        .scopeDistance = scopeDistance});
    }
  }

  const auto &visibleAliases = [&]() -> const auto & {
    if (sourceGraph == nullptr || currentSourceUnit == 0) {
      return namespaceAliases;
    }
    const auto found = visibleNamespaceAliases.find(currentSourceUnit);
    if (found != visibleNamespaceAliases.end()) {
      return found->second;
    }
    static const std::unordered_map<std::string, NamespaceAliasInfo> empty;
    return empty;
  }();
  for (const auto &[qualified, _] : visibleAliases) {
    std::string name;
    if (directNamespaceChild(qualified, parent, name) && appendName(name)) {
      result.push_back({.kind = SemanticCompletionCandidateKind::Namespace,
                        .name = name,
                        .qualifiedName = qualified,
                        .scopeDistance = scopeDistance});
    }
  }
}

auto SemanticVisitor::Impl::captureUnqualifiedCompletion(
    const Token &completion) -> void {
  SemanticCompletionContext context{.kind = SemanticCompletionKind::Unqualified,
                                    .sourceUnit = currentSourceUnit,
                                    .replacementRange = tokenSpan(completion),
                                    .prefix = completion.lexeme};
  std::unordered_set<std::string> seen;
  std::size_t distance = 0;
  for (auto scope = scopes.rbegin(); scope != scopes.rend();
       ++scope, ++distance) {
    for (const auto &[name, symbol] : *scope) {
      if (name == receiverStateName) {
        continue;
      }
      if (seen.insert(name).second) {
        appendSymbolCompletions(context.candidates, name, name, symbol,
                                distance);
      }
    }
  }
  for (auto types = typeParameterScopes.rbegin();
       types != typeParameterScopes.rend(); ++types, ++distance) {
    for (const auto &[name, type] : *types) {
      if (seen.insert(name).second) {
        context.candidates.push_back(
            {.kind = SemanticCompletionCandidateKind::TypeParameter,
             .name = name,
             .qualifiedName = name,
             .type = type,
             .scopeDistance = distance});
      }
    }
  }
  for (auto values = valueParameterScopes.rbegin();
       values != valueParameterScopes.rend(); ++values, ++distance) {
    for (const auto &[name, _] : *values) {
      if (seen.insert(name).second) {
        context.candidates.push_back(
            {.kind = SemanticCompletionCandidateKind::ValueParameter,
             .name = name,
             .qualifiedName = name,
             .type = SemanticType::UInt64,
             .scopeDistance = distance});
      }
    }
  }
  if (currentClass) {
    const ClassInfo &owner = classInfo(*currentClass);
    for (const auto &[name, alias] : owner.typeAliases) {
      if (!seen.insert(name).second ||
          (alias.access == AccessModifier::Private &&
           currentClass != alias.ownerClass) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           (alias.compilerPrivate ||
            semanticModel.isCompilerPrivateType(alias.type)))) {
        continue;
      }
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::TypeAlias,
           .name = name,
           .qualifiedName = alias.qualifiedName,
           .type = alias.type,
           .scopeDistance = distance,
           .typeAlias = alias.declaration,
           .symbol = symbolForDeclaration(alias.declaration->name()),
           .compilerPrivate = alias.compilerPrivate});
    }
    ++distance;
  }
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0;
       --depth, ++distance) {
    std::string parent = qualifiedName(currentNamespace, depth - 1, "");
    if (!parent.empty()) {
      parent.resize(parent.size() - 2);
    }
    appendNamespaceChildren(context.candidates, parent, distance, &seen);
  }
  semanticModel.recordCompletion(std::move(context));
}

auto SemanticVisitor::Impl::captureQualifiedCompletion(const NamePath &path)
    -> void {
  const Token &completion = path.last();
  SemanticCompletionContext context{.kind = SemanticCompletionKind::Namespace,
                                    .sourceUnit = currentSourceUnit,
                                    .replacementRange = tokenSpan(completion),
                                    .prefix = completion.lexeme};
  if (path.segments.size() < 2) {
    semanticModel.recordCompletion(std::move(context));
    return;
  }
  const NamePath ownerPath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  if (const std::optional<EnumId> owner =
          resolveEnumPath(ownerPath, currentNamespace)) {
    context.kind = SemanticCompletionKind::Enum;
    const EnumInfo &enumeration = enums.at(*owner - 1);
    for (const auto &[name, enumerator] : enumeration.enumerators) {
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::Enumerator,
           .name = name,
           .qualifiedName = qualifiedName(enumeration.namespaceScope,
                                          enumeration.name.lexeme) +
                            "::" + name,
           .type = enumerator.symbol.type,
           .scopeDistance = 0,
           .enumType = *owner});
    }
  } else if (const std::optional<ClassId> owner =
                 resolveClassPath(ownerPath, currentNamespace)) {
    context.kind = SemanticCompletionKind::Member;
    const ClassInfo &classType = classInfo(*owner);
    std::unordered_set<std::string> seenTypeAliases;
    for (const auto &[name, alias] : classType.typeAliases) {
      if ((alias.access == AccessModifier::Private &&
           currentClass != alias.ownerClass) ||
          (!classType.genericParameters.empty() && currentClass != *owner) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           (alias.compilerPrivate ||
            semanticModel.isCompilerPrivateType(alias.type)))) {
        continue;
      }
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::TypeAlias,
           .name = name,
           .qualifiedName = qualifiedName(classType.namespaceScope,
                                          classType.name.lexeme + "::" + name),
           .type = alias.type,
           .scopeDistance = 0,
           .typeAlias = alias.declaration,
           .symbol = symbolForDeclaration(alias.declaration->name()),
           .compilerPrivate = alias.compilerPrivate});
      seenTypeAliases.insert(name);
    }
    for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
      const RegisteredTypeAlias &alias = typeAliases[id - 1];
      if (alias.ownerClass != *owner ||
          alias.resolution != TypeAliasResolution::Resolved ||
          !seenTypeAliases.insert(alias.declaration->name().lexeme).second ||
          (alias.access == AccessModifier::Private &&
           currentClass != alias.ownerClass) ||
          (!classType.genericParameters.empty() && currentClass != *owner) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           (alias.compilerPrivate ||
            semanticModel.isCompilerPrivateType(alias.type)))) {
        continue;
      }
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::TypeAlias,
           .name = alias.declaration->name().lexeme,
           .qualifiedName = alias.qualifiedName,
           .type = alias.type,
           .scopeDistance = 0,
           .typeAlias = alias.declaration,
           .symbol = symbolForDeclaration(alias.declaration->name()),
           .compilerPrivate = alias.compilerPrivate});
    }
    for (const auto &[name, member] : classType.members) {
      if (!member.symbol.staticMember ||
          (member.access == AccessModifier::Private &&
           currentClass != member.symbol.ownerClass)) {
        continue;
      }
      appendSymbolCompletions(
          context.candidates, name,
          qualifiedName(classType.namespaceScope,
                        classType.name.lexeme + "::" + name),
          member.symbol, 0);
    }
  } else if (const std::optional<std::string> owner =
                 resolveNamespacePath(ownerPath, currentNamespace)) {
    appendNamespaceChildren(context.candidates, *owner, 0);
  }
  semanticModel.recordCompletion(std::move(context));
}

auto SemanticVisitor::Impl::captureMemberCompletion(
    const Token &completion, const SemanticType &objectType,
    bool mutableReceiver) -> void {
  SemanticCompletionContext context{.kind = SemanticCompletionKind::Member,
                                    .sourceUnit = currentSourceUnit,
                                    .replacementRange = tokenSpan(completion),
                                    .prefix = completion.lexeme};
  if (objectType.kind == SemanticType::Class) {
    if (const ClassInfo *owner = classInfo(objectType)) {
      if (!isCompilerTrustedUnit(currentSourceUnit) && owner->compilerPrivate) {
        semanticModel.recordCompletion(std::move(context));
        return;
      }
      for (const auto &[name, member] : owner->members) {
        if (member.symbol.staticMember) {
          continue;
        }
        if (member.access == AccessModifier::Private &&
            currentClass != member.symbol.ownerClass) {
          continue;
        }
        const Symbol substituted = substituteSymbol(member.symbol, objectType);
        appendSymbolCompletions(context.candidates, name, name, substituted, 0,
                                true, mutableReceiver);
      }
    }
  } else if (objectType.kind == SemanticType::Array) {
    context.candidates.push_back(
        {.kind = SemanticCompletionCandidateKind::Method,
         .name = "size",
         .qualifiedName = "size",
         .detail = "uint64_t size()",
         .scopeDistance = 0});
  } else if (objectType.kind == SemanticType::StringView) {
    context.candidates.push_back(
        {.kind = SemanticCompletionCandidateKind::Method,
         .name = "size",
         .qualifiedName = "size",
         .detail = "uint64_t size()",
         .scopeDistance = 0});
    context.candidates.push_back(
        {.kind = SemanticCompletionCandidateKind::Method,
         .name = "empty",
         .qualifiedName = "empty",
         .detail = "bool empty()",
         .scopeDistance = 0});
  }
  semanticModel.recordCompletion(std::move(context));
}

auto SemanticVisitor::Impl::resolve(const Token &name) const -> const Symbol * {
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    if (const auto found = scope->find(name.lexeme); found != scope->end()) {
      return &found->second;
    }
  }

  const auto &symbols = currentNamespaceSymbols();
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto found =
        symbols.find(qualifiedName(currentNamespace, depth - 1, name.lexeme));
    if (found != symbols.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::resolveMutable(const Token &name) -> Symbol * {
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    if (const auto found = scope->find(name.lexeme); found != scope->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::resolvePlaceSymbol(const Token &name) -> Symbol * {
  if (Symbol *local = resolveMutable(name)) {
    return local;
  }
  std::unordered_map<std::string, Symbol> *symbols =
      currentNamespaceSymbolsMutable();
  if (symbols == nullptr) {
    return nullptr;
  }
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto found =
        symbols->find(qualifiedName(currentNamespace, depth - 1, name.lexeme));
    if (found != symbols->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::resolveNamespaceSegments(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::vector<ResolvedNamespaceSegment> {
  std::vector<ResolvedNamespaceSegment> result;
  if (path.segments.empty()) {
    return result;
  }

  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const std::string candidate =
        qualifiedName(fromScope, depth - 1, path.first().lexeme);
    if (const NamespaceAliasInfo *alias = findNamespaceAlias(candidate)) {
      result.push_back({.declaration = candidate, .target = alias->target});
      break;
    }
    if (namespaceIsVisible(candidate)) {
      result.push_back({.declaration = candidate, .target = candidate});
      break;
    }
  }
  if (result.empty()) {
    return result;
  }

  for (std::size_t index = 1; index < path.segments.size(); ++index) {
    const std::string candidate =
        result.back().target + "::" + path.segments[index].lexeme;
    if (const NamespaceAliasInfo *alias = findNamespaceAlias(candidate)) {
      result.push_back({.declaration = candidate, .target = alias->target});
    } else if (namespaceIsVisible(candidate)) {
      result.push_back({.declaration = candidate, .target = candidate});
    } else {
      break;
    }
  }
  return result;
}

auto SemanticVisitor::Impl::resolveNamespacePath(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<std::string> {
  if (path.segments.empty()) {
    return std::nullopt;
  }
  const std::vector<ResolvedNamespaceSegment> segments =
      resolveNamespaceSegments(path, fromScope);
  return segments.size() == path.segments.size()
             ? std::optional<std::string>(segments.back().target)
             : std::nullopt;
}

auto SemanticVisitor::Impl::resolveNamespacePath(const NamePath &path) const
    -> std::optional<std::string> {
  return resolveNamespacePath(path, currentNamespace);
}

auto SemanticVisitor::Impl::resolveQualified(const NamePath &path) const
    -> const Symbol * {
  if (path.segments.size() < 2) {
    return resolve(path.last());
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath);
  if (!resolvedNamespace) {
    if (const std::optional<ClassId> classId =
            resolveClassPath(namespacePath, currentNamespace)) {
      const ClassInfo &owner = classInfo(*classId);
      const auto member = owner.members.find(path.last().lexeme);
      return member != owner.members.end() && member->second.symbol.staticMember
                 ? &member->second.symbol
                 : nullptr;
    }
    const EnumeratorRecord *enumerator = resolveEnumerator(path);
    return enumerator == nullptr ? nullptr : &enumerator->symbol;
  }
  const auto &symbols = currentNamespaceSymbols();
  const auto symbol =
      symbols.find(*resolvedNamespace + "::" + path.last().lexeme);
  if (symbol != symbols.end()) {
    return &symbol->second;
  }
  const EnumeratorRecord *enumerator = resolveEnumerator(path);
  return enumerator == nullptr ? nullptr : &enumerator->symbol;
}

auto SemanticVisitor::Impl::resolveQualifiedPlaceSymbol(const NamePath &path)
    -> Symbol * {
  if (path.segments.size() < 2) {
    return resolvePlaceSymbol(path.last());
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath);
  if (!resolvedNamespace) {
    const std::optional<ClassId> classId =
        resolveClassPath(namespacePath, currentNamespace);
    if (!classId) {
      return nullptr;
    }
    ClassInfo &owner = classInfo(*classId);
    const auto member = owner.members.find(path.last().lexeme);
    return member != owner.members.end() && member->second.symbol.staticMember
               ? &member->second.symbol
               : nullptr;
  }

  std::unordered_map<std::string, Symbol> *symbols =
      currentNamespaceSymbolsMutable();
  if (symbols == nullptr) {
    return nullptr;
  }
  const auto symbol =
      symbols->find(*resolvedNamespace + "::" + path.last().lexeme);
  return symbol == symbols->end() ? nullptr : &symbol->second;
}

auto SemanticVisitor::Impl::resolveInitialNamespaceGlobally(
    const Token &name, const std::vector<std::string> &fromScope) const
    -> std::optional<std::string> {
  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const std::string candidate =
        qualifiedName(fromScope, depth - 1, name.lexeme);
    if (const auto alias = namespaceAliases.find(candidate);
        alias != namespaceAliases.end()) {
      return alias->second.target;
    }
    if (namespaces.contains(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::resolveNamespacePathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<std::string> {
  std::optional<std::string> current =
      resolveInitialNamespaceGlobally(path.first(), fromScope);
  if (!current) {
    return std::nullopt;
  }
  for (std::size_t index = 1; index < path.segments.size(); ++index) {
    const std::string candidate = *current + "::" + path.segments[index].lexeme;
    if (const auto alias = namespaceAliases.find(candidate);
        alias != namespaceAliases.end()) {
      current = alias->second.target;
    } else if (namespaces.contains(candidate)) {
      current = candidate;
    } else {
      return std::nullopt;
    }
  }
  return current;
}

auto SemanticVisitor::Impl::resolveGlobally(const Token &name) const
    -> const Symbol * {
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto &namespaceIndex = activeNamespaceSymbols();
    const auto found = namespaceIndex.find(
        qualifiedName(currentNamespace, depth - 1, name.lexeme));
    if (found != namespaceIndex.end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::resolveQualifiedGlobally(const NamePath &path) const
    -> const Symbol * {
  if (path.segments.size() < 2) {
    return resolveGlobally(path.last());
  }
  const NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePathGlobally(namespacePath, currentNamespace);
  if (!resolvedNamespace) {
    if (const std::optional<ClassId> classId =
            resolveClassPathGlobally(namespacePath, currentNamespace)) {
      const ClassInfo &owner = classInfo(*classId);
      const auto member = owner.members.find(path.last().lexeme);
      return member != owner.members.end() && member->second.symbol.staticMember
                 ? &member->second.symbol
                 : nullptr;
    }
    const EnumeratorRecord *enumerator = resolveEnumerator(path, true);
    return enumerator == nullptr ? nullptr : &enumerator->symbol;
  }
  const auto &namespaceIndex = activeNamespaceSymbols();
  const auto symbol =
      namespaceIndex.find(*resolvedNamespace + "::" + path.last().lexeme);
  if (symbol != namespaceIndex.end()) {
    return &symbol->second;
  }
  const EnumeratorRecord *enumerator = resolveEnumerator(path, true);
  return enumerator == nullptr ? nullptr : &enumerator->symbol;
}

auto SemanticVisitor::Impl::declarationSourceUnit(const Symbol &symbol) const
    -> SourceUnitId {
  if (!symbol.overloads.empty()) {
    return symbol.overloads.front().record().sourceUnit;
  }
  return symbol.sourceUnit;
}

auto SemanticVisitor::Impl::reportInvisibleDeclaration(
    const Token &use, std::string name, const Token &declaration,
    SourceUnitId declarationUnit) -> bool {
  if (sourceGraph == nullptr || currentSourceUnit == 0 ||
      declarationUnit == 0 || sourceVisible(declarationUnit)) {
    return false;
  }

  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2024", DiagnosticPhase::Semantics, use,
      "Declaration '" + name +
          "' is not visible from this source file; include its file "
          "directly.");
  diagnostic.related.push_back(
      {tokenSpan(declaration), "Declaration is in this source unit."});
  const SourceUnit *requester = sourceGraph->findUnit(currentSourceUnit);
  const SourceUnit *target = sourceGraph->findUnit(declarationUnit);
  if (requester != nullptr && target != nullptr) {
    if (target->standardLibraryName) {
      diagnostic.hints.emplace_back("Add '#include <" +
                                    *target->standardLibraryName +
                                    ">' to this source file.");
    } else {
      std::filesystem::path relative =
          target->path.lexically_relative(requester->path.parent_path());
      if (relative.empty()) {
        relative = target->path.filename();
      }
      diagnostic.hints.emplace_back("Add '#include \"" +
                                    relative.generic_string() +
                                    "\"' to this source file.");
    }
  }
  diagnostics.push_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::reportInvisibleSymbol(const Token &use,
                                                  std::string name,
                                                  const Symbol *symbol)
    -> bool {
  return symbol != nullptr &&
         reportInvisibleDeclaration(use, std::move(name), symbol->declaration,
                                    declarationSourceUnit(*symbol));
}

auto SemanticVisitor::Impl::resolveTypeAliasPath(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<ResolvedTypeAlias> {
  const auto &visibleAliases = currentTypeAliasIds();
  const auto direct = [&](TypeAliasId id) -> std::optional<ResolvedTypeAlias> {
    if (id == 0 || id > typeAliases.size()) {
      return std::nullopt;
    }
    const RegisteredTypeAlias &alias = typeAliases[id - 1];
    return ResolvedTypeAlias{.id = id,
                             .declaration = alias.declaration,
                             .type = alias.type,
                             .qualifiedName = alias.qualifiedName,
                             .ownerClass = alias.ownerClass,
                             .access = alias.access,
                             .compilerPrivate = alias.compilerPrivate};
  };
  const auto member =
      [&](ClassId owner,
          const std::string &name) -> std::optional<ResolvedTypeAlias> {
    if (owner == 0 || owner > classes.size()) {
      return std::nullopt;
    }
    const auto found = classInfo(owner).typeAliases.find(name);
    if (found == classInfo(owner).typeAliases.end()) {
      return std::nullopt;
    }
    const MemberTypeAliasInfo &alias = found->second;
    return ResolvedTypeAlias{.id = alias.id,
                             .declaration = alias.declaration,
                             .type = alias.type,
                             .qualifiedName = alias.qualifiedName,
                             .ownerClass = alias.ownerClass,
                             .access = alias.access,
                             .compilerPrivate = alias.compilerPrivate};
  };
  if (path.segments.size() == 1) {
    if (currentClass) {
      if (const std::optional<ResolvedTypeAlias> alias =
              member(*currentClass, path.last().lexeme)) {
        return alias;
      }
    }
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = visibleAliases.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != visibleAliases.end()) {
        return direct(found->second);
      }
    }
    return std::nullopt;
  }

  const std::string written = pathSpelling(path);
  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const auto found =
        visibleAliases.find(qualifiedName(fromScope, depth - 1, written));
    if (found != visibleAliases.end()) {
      return direct(found->second);
    }
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath, fromScope);
  if (resolvedNamespace) {
    const auto found =
        visibleAliases.find(*resolvedNamespace + "::" + path.last().lexeme);
    if (found != visibleAliases.end()) {
      return direct(found->second);
    }
  }
  if (const std::optional<ClassId> owner =
          resolveClassPath(namespacePath, fromScope)) {
    return member(*owner, path.last().lexeme);
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::resolveTypeAliasPathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<TypeAliasId> {
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = typeAliasIds.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != typeAliasIds.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  const std::string written = pathSpelling(path);
  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const auto found =
        typeAliasIds.find(qualifiedName(fromScope, depth - 1, written));
    if (found != typeAliasIds.end()) {
      return found->second;
    }
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePathGlobally(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      typeAliasIds.find(*resolvedNamespace + "::" + path.last().lexeme);
  return found == typeAliasIds.end()
             ? std::nullopt
             : std::optional<TypeAliasId>(found->second);
}

auto SemanticVisitor::Impl::resolveConceptPath(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<ConceptId> {
  const auto &visibleConcepts = currentConceptIds();
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = visibleConcepts.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != visibleConcepts.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      visibleConcepts.find(*resolvedNamespace + "::" + path.last().lexeme);
  return found == visibleConcepts.end()
             ? std::nullopt
             : std::optional<ConceptId>(found->second);
}

auto SemanticVisitor::Impl::resolveConceptPathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<ConceptId> {
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = conceptIds.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != conceptIds.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePathGlobally(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      conceptIds.find(*resolvedNamespace + "::" + path.last().lexeme);
  return found == conceptIds.end() ? std::nullopt
                                   : std::optional<ConceptId>(found->second);
}

auto SemanticVisitor::Impl::resolveClassPath(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<ClassId> {
  const auto &visibleClasses = currentClassIds();
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = visibleClasses.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != visibleClasses.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      visibleClasses.find(*resolvedNamespace + "::" + path.last().lexeme);
  return found == visibleClasses.end() ? std::nullopt
                                       : std::optional<ClassId>(found->second);
}

auto SemanticVisitor::Impl::resolveClassPathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<ClassId> {
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const auto found = classIds.find(
          qualifiedName(fromScope, depth - 1, path.last().lexeme));
      if (found != classIds.end()) {
        return found->second;
      }
    }
    return std::nullopt;
  }
  const NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePathGlobally(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      classIds.find(*resolvedNamespace + "::" + path.last().lexeme);
  return found == classIds.end() ? std::nullopt
                                 : std::optional<ClassId>(found->second);
}

auto SemanticVisitor::Impl::resolveEnumPath(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<EnumId> {
  const auto &visibleEnums = currentEnumIds();
  const auto &visibleAliases = currentTypeAliasIds();
  const auto enumFromAlias =
      [&](const std::string &name) -> std::optional<EnumId> {
    const auto alias = visibleAliases.find(name);
    if (alias == visibleAliases.end() || alias->second == 0 ||
        alias->second > typeAliases.size()) {
      return std::nullopt;
    }
    const SemanticType &type = typeAliases[alias->second - 1].type;
    return type.kind == SemanticType::Enum ? std::optional<EnumId>(type.enumId)
                                           : std::nullopt;
  };
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const std::string qualified =
          qualifiedName(fromScope, depth - 1, path.last().lexeme);
      const auto found = visibleEnums.find(qualified);
      if (found != visibleEnums.end()) {
        return found->second;
      }
      if (const std::optional<EnumId> alias = enumFromAlias(qualified)) {
        return alias;
      }
    }
    return std::nullopt;
  }

  const NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePath(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      visibleEnums.find(*resolvedNamespace + "::" + path.last().lexeme);
  if (found != visibleEnums.end()) {
    return found->second;
  }
  return enumFromAlias(*resolvedNamespace + "::" + path.last().lexeme);
}

auto SemanticVisitor::Impl::resolveEnumPathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<EnumId> {
  const auto enumFromAlias =
      [&](const std::string &name) -> std::optional<EnumId> {
    const auto alias = typeAliasIds.find(name);
    if (alias == typeAliasIds.end() || alias->second == 0 ||
        alias->second > typeAliases.size()) {
      return std::nullopt;
    }
    const SemanticType &type = typeAliases[alias->second - 1].type;
    return type.kind == SemanticType::Enum ? std::optional<EnumId>(type.enumId)
                                           : std::nullopt;
  };
  if (path.segments.size() == 1) {
    for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
      const std::string qualified =
          qualifiedName(fromScope, depth - 1, path.last().lexeme);
      const auto found = enumIds.find(qualified);
      if (found != enumIds.end()) {
        return found->second;
      }
      if (const std::optional<EnumId> alias = enumFromAlias(qualified)) {
        return alias;
      }
    }
    return std::nullopt;
  }
  const NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<std::string> resolvedNamespace =
      resolveNamespacePathGlobally(namespacePath, fromScope);
  if (!resolvedNamespace) {
    return std::nullopt;
  }
  const auto found =
      enumIds.find(*resolvedNamespace + "::" + path.last().lexeme);
  if (found != enumIds.end()) {
    return found->second;
  }
  return enumFromAlias(*resolvedNamespace + "::" + path.last().lexeme);
}

auto SemanticVisitor::Impl::resolveEnumerator(const NamePath &path,
                                              bool global) const
    -> const EnumeratorRecord * {
  if (path.segments.size() < 2) {
    return nullptr;
  }
  const NamePath enumPath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  const std::optional<EnumId> enumId =
      global ? resolveEnumPathGlobally(enumPath, currentNamespace)
             : resolveEnumPath(enumPath, currentNamespace);
  if (!enumId || *enumId == 0 || *enumId > enums.size()) {
    return nullptr;
  }
  const EnumInfo &owner = enums[*enumId - 1];
  const auto found = owner.enumerators.find(path.last().lexeme);
  return found == owner.enumerators.end() ? nullptr : &found->second;
}

auto SemanticVisitor::Impl::payloadEnumeratorForCallee(
    const ExprPtr &callee) const -> const EnumeratorRecord * {
  const auto *qualified = exprAs<QualifiedName>(callee.get());
  if (qualified == nullptr) {
    return nullptr;
  }
  const EnumeratorRecord *enumerator = resolveEnumerator(qualified->name());
  if (enumerator == nullptr ||
      enumerator->symbol.type.kind != SemanticType::Enum) {
    return nullptr;
  }
  const EnumId owner = enumerator->symbol.type.enumId;
  return owner != 0 && owner <= enums.size() && enums[owner - 1].payload
             ? enumerator
             : nullptr;
}

auto SemanticVisitor::Impl::analyzePayloadConstruction(
    const Call &call, const EnumeratorRecord &enumerator) -> void {
  const EnumId owner = enumerator.symbol.type.enumId;
  const bool enclosingCallCallee = analyzingCallCallee;
  const bool enclosingPayloadReference = allowPayloadEnumeratorReference;
  analyzingCallCallee = true;
  allowPayloadEnumeratorReference = true;
  (void)analyze(call.callee());
  allowPayloadEnumeratorReference = enclosingPayloadReference;
  analyzingCallCallee = enclosingCallCallee;

  for (const TypeRef &argument : call.typeArguments()) {
    validateType(argument);
  }
  if (!call.typeArguments().empty()) {
    report(call.paren(), "Payload variants do not take generic arguments.",
           "GTI-S2067");
  }

  if (enumerator.payloadTypes.empty()) {
    for (const ExprPtr &argument : call.arguments()) {
      analyze(argument);
    }
    report(call.paren(),
           "Unit variant '" + enumerator.declaration->name.lexeme +
               "' is a value; write it without '()'.",
           "GTI-S2067");
    currentType = SemanticType::enumType(owner);
    return;
  }

  if (call.arguments().size() != enumerator.payloadTypes.size()) {
    report(call.paren(),
           "Payload variant '" + enumerator.declaration->name.lexeme +
               "' expects " + std::to_string(enumerator.payloadTypes.size()) +
               " argument" + (enumerator.payloadTypes.size() == 1 ? "." : "s."),
           "GTI-S2067");
  }
  const std::size_t common =
      std::min(call.arguments().size(), enumerator.payloadTypes.size());
  for (std::size_t index = 0; index < common; ++index) {
    const SemanticType actual = analyzeInitializer(
        call.arguments()[index], enumerator.payloadTypes[index]);
    if (actual != SemanticType::Unknown &&
        !callArgumentMatches(enumerator.payloadTypes[index], actual,
                             call.arguments()[index])) {
      report(expressionToken(call.arguments()[index]),
             "Payload argument " + std::to_string(index + 1) + " has type '" +
                 typeSpelling(actual) + "'; variant '" +
                 enumerator.declaration->name.lexeme +
                 "' requires exact type '" +
                 typeSpelling(enumerator.payloadTypes[index]) + "'.",
             "GTI-S2067");
    }
    // Closed payload alternatives may own active-cleanup class values. Stored
    // borrowed state remains outside this family because the variant would
    // otherwise outlive or relocate an untracked referent.
    if (typeTraits(actual).containsBorrowedState) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics,
          expressionToken(call.arguments()[index]),
          "Payload type '" + typeSpelling(actual) +
              "' carries borrowed state and cannot live inside a variant "
              "alternative.");
      diagnostic.hints.emplace_back(
          "Payload alternatives may own values, but they cannot store GTI "
          "references or other borrowed-state carriers.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
    if (requiresActiveCleanupFor(actual) &&
        actual.kind != SemanticType::Class) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics,
          expressionToken(call.arguments()[index]),
          "Payload type '" + typeSpelling(actual) +
              "' requires active cleanup but is not a concrete class value; "
              "this payload-enum slice supports owning class alternatives "
              "only.");
      diagnostic.hints.emplace_back(
          "Nested owning payload enums, owning arrays, and other recursive "
          "cleanup carriers remain unsupported until their partial-state "
          "lifecycle is represented explicitly.");
      diagnostics.emplace_back(std::move(diagnostic));
    }
  }
  for (std::size_t index = common; index < call.arguments().size(); ++index) {
    analyze(call.arguments()[index]);
  }
  semanticModel.recordPayloadConstruction(
      call, {.owner = owner,
             .declaration = enumerator.declaration,
             .variantIndex = enumerator.variantIndex,
             .parameterTypes = enumerator.payloadTypes});
  currentType = SemanticType::enumType(owner);
}

auto SemanticVisitor::Impl::analyzePayloadSwitchPattern(
    const ExprPtr &expression, const SemanticType &subjectType)
    -> std::optional<ResolvedPayloadPatternInfo> {
  const Call *call = exprAs<Call>(expression.get());
  const auto *qualified = exprAs<QualifiedName>(
      call == nullptr ? expression.get() : call->callee().get());
  if (qualified == nullptr) {
    report(expressionToken(expression),
           "Payload-enum cases must name a variant, optionally followed by "
           "binding names in parentheses.",
           "GTI-S2067");
    return std::nullopt;
  }
  const EnumeratorRecord *enumerator = resolveEnumerator(qualified->name());
  if (enumerator == nullptr || enumerator->symbol.type != subjectType) {
    report(qualified->name().last(),
           "Switch case must name a variant of '" + typeSpelling(subjectType) +
               "'.",
           "GTI-S2067");
    return std::nullopt;
  }

  const bool enclosingCallCallee = analyzingCallCallee;
  const bool enclosingPayloadReference = allowPayloadEnumeratorReference;
  analyzingCallCallee = true;
  allowPayloadEnumeratorReference = true;
  (void)analyze(*qualified);
  allowPayloadEnumeratorReference = enclosingPayloadReference;
  analyzingCallCallee = enclosingCallCallee;

  ResolvedPayloadPatternInfo result{.owner = subjectType.enumId,
                                    .declaration = enumerator->declaration,
                                    .variantIndex = enumerator->variantIndex};
  if (call == nullptr) {
    return result;
  }
  semanticModel.record(*call, makeExpressionInfo(subjectType));
  if (!call->typeArguments().empty()) {
    for (const TypeRef &argument : call->typeArguments()) {
      validateType(argument);
    }
    report(call->paren(),
           "Payload switch patterns do not take generic arguments.",
           "GTI-S2067");
    return std::nullopt;
  }
  if (call->arguments().size() != enumerator->payloadTypes.size()) {
    report(call->paren(),
           "Variant '" + enumerator->declaration->name.lexeme + "' exposes " +
               std::to_string(enumerator->payloadTypes.size()) +
               " payload field" +
               (enumerator->payloadTypes.size() == 1 ? "." : "s."),
           "GTI-S2067");
    return std::nullopt;
  }
  std::unordered_set<std::string> names;
  for (std::size_t index = 0; index < call->arguments().size(); ++index) {
    const auto *binding = exprAs<Variable>(call->arguments()[index].get());
    if (binding == nullptr) {
      report(expressionToken(call->arguments()[index]),
             "Payload switch patterns accept only plain binding names.",
             "GTI-S2067");
      return std::nullopt;
    }
    if (!names.insert(binding->name().lexeme).second) {
      report(binding->name(),
             "Duplicate payload binding '" + binding->name().lexeme +
                 "' in this pattern.",
             "GTI-S2067");
      return std::nullopt;
    }
    const SemanticType &payloadType = enumerator->payloadTypes[index];
    if (!typeTraits(payloadType).copyable) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics, binding->name(),
          "Payload field " + std::to_string(index + 1) + " of variant '" +
              enumerator->declaration->name.lexeme + "' has move-only type '" +
              typeSpelling(payloadType) +
              "' and cannot be copied into a non-consuming match binding.");
      diagnostic.hints.emplace_back(
          "Match the variant without payload bindings (for example 'case " +
          typeSpelling(subjectType) +
          "::" + enumerator->declaration->name.lexeme +
          ":') until consuming payload patterns are available.");
      diagnostics.emplace_back(std::move(diagnostic));
      return std::nullopt;
    }
    result.bindings.push_back(
        {.name = &binding->name(),
         .source = binding,
         .binding = bindingInfo(payloadType, AccessMode::ReadOnly),
         .payloadIndex = index});
  }
  return result;
}

auto SemanticVisitor::Impl::resolveExpressionSymbol(
    const ExprPtr &expression) const -> std::optional<Symbol> {
  if (expression && expression->exprKind() == ExprKind::Variable) {
    const auto *variable = static_cast<const Variable *>(expression.get());
    const Symbol *symbol = resolve(variable->name());
    return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
  }
  if (expression && expression->exprKind() == ExprKind::QualifiedName) {
    const auto *qualified =
        static_cast<const QualifiedName *>(expression.get());
    const Symbol *symbol = resolveQualified(qualified->name());
    return symbol == nullptr ? std::nullopt : std::optional<Symbol>(*symbol);
  }
  if (expression && expression->exprKind() == ExprKind::Get) {
    const auto *get = static_cast<const Get *>(expression.get());
    const SemanticType memberObjectType = memberAccessObjectType(*get);
    if (memberObjectType != SemanticType::Unknown) {
      if (const MemberInfo *member =
              findMember(memberObjectType, get->name())) {
        return substituteSymbol(member->symbol, memberObjectType);
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::isMutableTarget(const ExprPtr &expression) const
    -> bool {
  if (expression) {
    if (const ExpressionInfo *info =
            semanticModel.findExpression(*expression)) {
      return info->category == ValueCategory::Place &&
             info->access == AccessMode::Mutable;
    }
  }
  if (expression && expression->exprKind() == ExprKind::Variable) {
    const auto *variable = static_cast<const Variable *>(expression.get());
    const Symbol *symbol = resolve(variable->name());
    if (symbol == nullptr) {
      return true;
    }
    return symbol->assignable &&
           (symbol->ownerClass == 0 || symbol->staticMember ||
            receiverAllowsMutation(currentReceiverMutability));
  }
  if (expression && expression->exprKind() == ExprKind::QualifiedName) {
    const auto *qualified =
        static_cast<const QualifiedName *>(expression.get());
    const Symbol *symbol = resolveQualified(qualified->name());
    if (symbol == nullptr) {
      return true;
    }
    return symbol->assignable &&
           (symbol->ownerClass == 0 || symbol->staticMember ||
            receiverAllowsMutation(currentReceiverMutability));
  }
  if (expression && expression->exprKind() == ExprKind::Get) {
    const auto *get = static_cast<const Get *>(expression.get());
    const SemanticType memberObjectType = memberAccessObjectType(*get);
    if (memberObjectType == SemanticType::Unknown) {
      return true;
    }
    const MemberInfo *member = findMember(memberObjectType, get->name());
    return member == nullptr ||
           (member->symbol.assignable && memberReceiverIsMutable(*get));
  }
  return false;
}

auto SemanticVisitor::Impl::isMutableObject(const ExprPtr &expression) const
    -> bool {
  if (expression) {
    if (const ExpressionInfo *info =
            semanticModel.findExpression(*expression)) {
      return info->category == ValueCategory::Place &&
             info->access == AccessMode::Mutable;
    }
  }
  return isMutableTarget(expression);
}

auto SemanticVisitor::Impl::isThisObject(const ExprPtr &expression) -> bool {
  if (exprAs<This>(expression.get()) != nullptr) {
    return true;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return isThisObject(grouping->expression());
  }
  return false;
}

auto SemanticVisitor::Impl::receiverRestrictionActive() const -> bool {
  return currentClass && currentFunctionDeclaration != nullptr &&
         !receiverAllowsMutation(currentReceiverMutability) &&
         !currentStaticMemberFunction && lambdaDepth == 0;
}

auto SemanticVisitor::Impl::receiverRestrictedField(const Token &use,
                                                    const Symbol &symbol) const
    -> std::optional<ResolvedFieldUse> {
  if (!receiverRestrictionActive() || symbol.ownerClass != *currentClass ||
      symbol.staticMember || !symbol.assignable) {
    return std::nullopt;
  }
  return ResolvedFieldUse{.use = &use, .symbol = &symbol};
}

auto SemanticVisitor::Impl::receiverRestrictedField(
    const ExprPtr &expression) const -> std::optional<ResolvedFieldUse> {
  if (!receiverRestrictionActive() || !expression) {
    return std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return receiverRestrictedField(grouping->expression());
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    const SemanticType *objectType = semanticModel.findType(*index->object());
    if (objectType == nullptr || objectType->kind != SemanticType::Array) {
      return std::nullopt;
    }
    return receiverRestrictedField(index->object());
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    const Symbol *symbol = resolve(variable->name());
    return symbol == nullptr
               ? std::nullopt
               : receiverRestrictedField(variable->name(), *symbol);
  }
  const auto *member = exprAs<Get>(expression.get());
  if (member == nullptr || member->access().kind != TokenKind::DOT) {
    return std::nullopt;
  }
  const SemanticType objectType = memberAccessObjectType(*member);
  const MemberInfo *resolved = findMember(objectType, member->name());
  if (resolved == nullptr || resolved->symbol.staticMember ||
      !resolved->symbol.assignable) {
    return std::nullopt;
  }
  if (isThisObject(member->object())) {
    return receiverRestrictedField(member->name(), resolved->symbol);
  }
  return receiverRestrictedField(member->object());
}

auto SemanticVisitor::Impl::receiverRestrictedMemberRoot(
    const ExprPtr &object, const Token &memberUse, const Symbol &member) const
    -> std::optional<ResolvedFieldUse> {
  if (!receiverRestrictionActive() || member.staticMember ||
      !member.assignable) {
    return std::nullopt;
  }
  if (isThisObject(object)) {
    return receiverRestrictedField(memberUse, member);
  }
  return receiverRestrictedField(object);
}

auto SemanticVisitor::Impl::appendReceiverRestrictionContext(
    Diagnostic &diagnostic, const ResolvedFieldUse &root) const -> void {
  const FunctionDecl &method = *currentFunctionDeclaration;
  const Token &methodToken =
      method.operatorName() ? method.operatorName()->keyword : method.name();
  diagnostic.related.push_back(
      {tokenSpan(methodToken),
       "Method '" + methodToken.lexeme + "' is read-only by default."});
  diagnostic.related.push_back(
      {tokenSpan(root.symbol->declaration),
       "Field '" + root.symbol->declaration.lexeme +
           "' is declared mutable, but it is writable only through a "
           "mutable receiver."});
  diagnostic.hints.emplace_back(
      "Add trailing 'mut' only if callers should require a mutable '" +
      classInfo(*currentClass).name.lexeme +
      "'; otherwise avoid changing receiver state in this method.");
}

auto SemanticVisitor::Impl::reportReceiverRestrictedMutation(
    const Token &writtenField, const ResolvedFieldUse &root) -> bool {
  if (root.use == nullptr || root.symbol == nullptr ||
      currentFunctionDeclaration == nullptr || !currentClass) {
    return false;
  }
  const Token &methodToken =
      currentFunctionDeclaration->operatorName()
          ? currentFunctionDeclaration->operatorName()->keyword
          : currentFunctionDeclaration->name();
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2002", DiagnosticPhase::Semantics, writtenField,
      "Cannot modify field '" + writtenField.lexeme + "' because method '" +
          methodToken.lexeme + "' has no trailing 'mut' receiver qualifier.");
  appendReceiverRestrictionContext(diagnostic, root);
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::receiverRestrictedWrittenField(
    const ExprPtr &expression) -> const Token * {
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    return receiverRestrictedWrittenField(grouping->expression());
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    return receiverRestrictedWrittenField(index->object());
  }
  if (const auto *member = exprAs<Get>(expression.get())) {
    return &member->name();
  }
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    return &variable->name();
  }
  return nullptr;
}

auto SemanticVisitor::Impl::reportReceiverRestrictedMutation(
    const ExprPtr &targetExpression) -> bool {
  const std::optional<ResolvedFieldUse> root =
      receiverRestrictedField(targetExpression);
  const Token *writtenField = receiverRestrictedWrittenField(targetExpression);
  return root && writtenField != nullptr &&
         reportReceiverRestrictedMutation(*writtenField, *root);
}

auto SemanticVisitor::Impl::reportReceiverRestrictedReference(
    const ExprPtr &initializer) -> bool {
  const std::optional<ResolvedFieldUse> root =
      receiverRestrictedField(initializer);
  if (!root || root->use == nullptr || root->symbol == nullptr ||
      currentFunctionDeclaration == nullptr || !currentClass) {
    return false;
  }
  const Token *writtenField = receiverRestrictedWrittenField(initializer);
  if (writtenField == nullptr) {
    return false;
  }
  const Token &methodToken =
      currentFunctionDeclaration->operatorName()
          ? currentFunctionDeclaration->operatorName()->keyword
          : currentFunctionDeclaration->name();
  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2017", DiagnosticPhase::Semantics, *writtenField,
      "Cannot bind a mutable reference to field '" + writtenField->lexeme +
          "' because method '" + methodToken.lexeme +
          "' has no trailing 'mut' receiver qualifier.");
  appendReceiverRestrictionContext(diagnostic, *root);
  diagnostics.emplace_back(std::move(diagnostic));
  return true;
}

auto SemanticVisitor::Impl::memberAccessObjectType(const Get &member) const
    -> SemanticType {
  const SemanticType *objectType = semanticModel.findType(*member.object());
  if (objectType == nullptr) {
    return SemanticType::Unknown;
  }
  if (member.access().kind != TokenKind::ARROW) {
    return *objectType;
  }
  if (objectType->kind == SemanticType::RawPointer &&
      objectType->arguments.size() == 1) {
    return objectType->arguments.front();
  }
  const ResolvedOperatorInfo *resolved =
      semanticModel.findProjectionOperator(member);
  if (resolved == nullptr || resolved->kind != OverloadedOperator::Arrow) {
    resolved = semanticModel.findOperator(member);
  }
  if (resolved != nullptr &&
      resolved->returnType.kind == SemanticType::Reference &&
      resolved->returnType.arguments.size() == 1) {
    return resolved->returnType.arguments[0];
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::memberAccessObjectType(const Set &member) const
    -> SemanticType {
  const SemanticType *objectType = semanticModel.findType(*member.object());
  if (objectType == nullptr) {
    return SemanticType::Unknown;
  }
  if (member.access().kind != TokenKind::ARROW) {
    return *objectType;
  }
  if (objectType->kind == SemanticType::RawPointer &&
      objectType->arguments.size() == 1) {
    return objectType->arguments.front();
  }
  const ResolvedOperatorInfo *resolved =
      semanticModel.findProjectionOperator(member);
  if (resolved == nullptr || resolved->kind != OverloadedOperator::Arrow) {
    resolved = semanticModel.findOperator(member);
  }
  if (resolved != nullptr &&
      resolved->returnType.kind == SemanticType::Reference &&
      resolved->returnType.arguments.size() == 1) {
    return resolved->returnType.arguments[0];
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::memberReceiverIsMutable(const Get &member) const
    -> bool {
  if (member.access().kind != TokenKind::ARROW) {
    return isMutableObject(member.object());
  }
  if (const SemanticType *objectType = semanticModel.findType(*member.object());
      objectType != nullptr && objectType->kind == SemanticType::RawPointer) {
    return objectType->pointerAccess == AccessMode::Mutable;
  }
  const ResolvedOperatorInfo *resolved = semanticModel.findOperator(member);
  if (resolved != nullptr) {
    return resolved->returnType.kind == SemanticType::Reference &&
           resolved->returnType.referenceAccess == AccessMode::Mutable;
  }
  return isMutableObject(member.object());
}

auto SemanticVisitor::Impl::transparentTemporaryProducer(
    const ExprPtr &expression) -> const Expr * {
  const Expr *producer = expression.get();
  while (const auto *grouping = exprAs<Grouping>(producer)) {
    producer = grouping->expression().get();
  }
  return producer;
}

auto SemanticVisitor::Impl::freshTemporaryValue(const ExprPtr &expression) const
    -> FreshTemporaryValueAssessment {
  FreshTemporaryValueAssessment result;
  if (!expression) {
    return result;
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  if (info == nullptr || info->category != ValueCategory::Value ||
      info->type.kind != SemanticType::Class) {
    return result;
  }
  result.type = info->type;
  if (isExplicitMoveReceiver(expression)) {
    result.failure = FreshTemporaryValueFailure::ExplicitMove;
    return result;
  }
  const ClassInfo *record = classInfo(info->type);
  if (record == nullptr ||
      (record->kind != ClassKind::Class && record->kind != ClassKind::Struct)) {
    result.failure = FreshTemporaryValueFailure::NonConcreteRecord;
    return result;
  }
  if (info->traits.containsBorrowedState) {
    result.failure = FreshTemporaryValueFailure::BorrowedState;
    return result;
  }

  const Expr *producer = transparentTemporaryProducer(expression);
  if (producer == nullptr) {
    result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
    return result;
  }
  if (const ResolvedConstructionInfo *construction =
          semanticModel.findConstruction(*producer)) {
    if (construction->kind == ConstructorKind::Ordinary &&
        construction->constructedType == info->type &&
        construction->borrowOrigin == BorrowOriginKind::None) {
      result.eligible = true;
      result.failure = FreshTemporaryValueFailure::None;
      return result;
    }
  }
  if (const auto *call = exprAs<Call>(producer)) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    const FunctionInfo *target =
        resolved == nullptr || resolved->function == 0
            ? nullptr
            : semanticModel.findFunction(resolved->function);
    if (target == nullptr && instanceBaseModel != nullptr &&
        resolved != nullptr && resolved->function != 0) {
      target = instanceBaseModel->findFunction(resolved->function);
    }
    if (resolved != nullptr && target != nullptr &&
        resolved->declaration != nullptr &&
        resolved->declaration->body() != nullptr &&
        !resolved->declaration->runtimeBinding() &&
        !resolved->declaration->hasCLinkage() &&
        !resolved->declaration->operatorName() &&
        resolved->intrinsic == IntrinsicKind::None &&
        resolved->returnType == info->type &&
        resolved->borrowOrigin == BorrowOriginKind::None &&
        target->linkage == LanguageLinkage::Gti) {
      result.eligible = true;
      result.failure = FreshTemporaryValueFailure::None;
      return result;
    }
  }
  result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
  return result;
}

auto SemanticVisitor::Impl::freshTemporaryMethodSupported(
    const CallableCandidate &function) const -> bool {
  return function.ownerClass != 0 && !function.staticMember &&
         !function.virtualMethod && function.declaration != nullptr &&
         function.returnType.kind != SemanticType::Reference &&
         function.returnBorrowOrigin == BorrowOriginKind::None &&
         !typeTraits(function.returnType).containsBorrowedState;
}

auto SemanticVisitor::Impl::receiverModeForObject(
    const ExprPtr &receiver, const CallableCandidate &function,
    bool allowFreshTemporary, bool consumingReceiver) const
    -> CallReceiverMode {
  if (function.staticMember) {
    return CallReceiverMode::None;
  }
  if (consumingReceiver) {
    return CallReceiverMode::Consuming;
  }
  const ExpressionInfo *info =
      receiver ? semanticModel.findExpression(*receiver) : nullptr;
  if (info != nullptr && info->category == ValueCategory::Place) {
    if (function.receiverMutability == ReceiverMutability::Mutable &&
        isMutableObject(receiver)) {
      return CallReceiverMode::MutablePlace;
    }
    return CallReceiverMode::ReadOnlyPlace;
  }
  // Some operator sites reconstruct an already-resolved named receiver.
  // Preserve its symbol-backed mutability without treating arbitrary values
  // as places; fresh temporaries still require the model-backed checks below.
  if (function.receiverMutability == ReceiverMutability::Mutable &&
      isMutableObject(receiver)) {
    return CallReceiverMode::MutablePlace;
  }
  if (allowFreshTemporary && freshTemporaryMethodSupported(function) &&
      freshTemporaryValue(receiver).eligible) {
    return function.receiverMutability == ReceiverMutability::Mutable
               ? CallReceiverMode::FreshTemporaryMutable
               : CallReceiverMode::FreshTemporaryRead;
  }
  return CallReceiverMode::Value;
}

auto SemanticVisitor::Impl::callReceiverMode(
    const ExprPtr &callee, const CallableCandidate &function) const
    -> CallReceiverMode {
  if (function.ownerClass == 0 || function.staticMember) {
    return CallReceiverMode::None;
  }
  if (const auto *member = exprAs<Get>(callee.get())) {
    if (member->access().kind == TokenKind::ARROW) {
      return function.receiverMutability == ReceiverMutability::Mutable &&
                     memberReceiverIsMutable(*member)
                 ? CallReceiverMode::MutablePlace
                 : CallReceiverMode::ReadOnlyPlace;
    }
    return receiverModeForObject(member->object(), function, true);
  }
  return function.receiverMutability == ReceiverMutability::Mutable &&
                 receiverAllowsMutation(currentReceiverMutability)
             ? CallReceiverMode::MutablePlace
             : CallReceiverMode::ReadOnlyPlace;
}

auto SemanticVisitor::Impl::receiverModeAllowsMutation(CallReceiverMode mode)
    -> bool {
  return mode == CallReceiverMode::MutablePlace ||
         mode == CallReceiverMode::FreshTemporaryMutable;
}

auto SemanticVisitor::Impl::reportMutableReceiverFailure(
    const ExprPtr &receiver, const Token &location,
    const CallableCandidate &function, std::string_view operation) -> void {
  const FreshTemporaryValueAssessment assessment =
      freshTemporaryValue(receiver);
  const bool valueShaped = assessment.type.kind == SemanticType::Class;
  if (!valueShaped) {
    report(location, std::string(operation) + " requires a mutable receiver.");
    return;
  }

  std::string reason;
  if (function.virtualMethod) {
    reason = "Virtual dispatch on a temporary receiver is not "
             "supported by the bounded temporary materialization model.";
  } else if (function.returnType.kind == SemanticType::Reference ||
             function.returnBorrowOrigin != BorrowOriginKind::None ||
             typeTraits(function.returnType).containsBorrowedState) {
    reason = "A temporary receiver cannot produce a reference or value "
             "containing tracked borrowed state.";
  } else {
    switch (assessment.failure) {
    case FreshTemporaryValueFailure::ExplicitMove:
      reason = "An explicit move from existing storage is not a fresh "
               "mutable temporary receiver.";
      break;
    case FreshTemporaryValueFailure::NonConcreteRecord:
      reason = "Only a concrete class or struct value can be a mutable "
               "temporary receiver.";
      break;
    case FreshTemporaryValueFailure::BorrowedState:
      reason = "A temporary containing tracked borrowed state cannot be "
               "borrowed as a mutable receiver.";
      break;
    case FreshTemporaryValueFailure::UnsupportedProducer:
      reason = "This value is not produced directly by an ordinary "
               "constructor or GTI function call.";
      break;
    case FreshTemporaryValueFailure::None:
    case FreshTemporaryValueFailure::NotClassValue:
      reason = std::string(operation) + " requires a mutable receiver.";
      break;
    }
  }

  Diagnostic diagnostic = makeDiagnostic(
      "GTI-S2081", DiagnosticPhase::Semantics, location, std::move(reason));
  if (function.declaration != nullptr) {
    const Token *declarationToken = &function.declaration->name();
    if (function.declaration->operatorName()) {
      declarationToken = &function.declaration->operatorName()->keyword;
    }
    diagnostic.related.push_back(
        {tokenSpan(*declarationToken),
         std::string(operation) +
             (function.receiverMutability == ReceiverMutability::Mutable
                  ? " is declared mutable here."
                  : " is declared here.")});
  }
  diagnostic.hints.emplace_back(
      "Bind the value to a 'mut' local before calling this operation when "
      "the object must come from existing or unsupported storage.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::classInfo(const SemanticType &type) const
    -> const ClassInfo * {
  if (type.kind != SemanticType::Class || type.classId == 0 ||
      type.classId > classes.size()) {
    return nullptr;
  }
  return &classes.at(type.classId - 1);
}

auto SemanticVisitor::Impl::isDefaultLibraryClass(
    const SemanticType &type, std::string_view expectedName) const -> bool {
  const ClassInfo *owner = classInfo(type);
  return owner != nullptr && isDefaultLibraryUnit(owner->sourceUnit) &&
         owner->namespaceScope.size() == 1 &&
         owner->namespaceScope.front() == "std" &&
         owner->name.lexeme == expectedName;
}

auto SemanticVisitor::Impl::ownedProgramEntryAppendFunction(
    const FunctionDecl &declaration, const FunctionInfo &function) const
    -> std::optional<FunctionId> {
  if (declaration.parameters().size() != 2 ||
      function.parameterTypes.size() != 2 ||
      function.parameterTypes[0] != SemanticType::Int32) {
    return std::nullopt;
  }
  const SemanticType &arguments = function.parameterTypes[1];
  if (!isDefaultLibraryClass(arguments, "vector") ||
      arguments.arguments.size() != 1 ||
      !isDefaultLibraryClass(arguments.arguments.front(), "string")) {
    return std::nullopt;
  }
  const ClassInfo *owner = classInfo(arguments);
  if (owner == nullptr) {
    return std::nullopt;
  }
  const auto found = owner->members.find("push_back");
  if (found == owner->members.end() ||
      found->second.access != AccessModifier::Public) {
    return std::nullopt;
  }
  const Symbol member = substituteSymbol(found->second.symbol, arguments);
  for (const CallableCandidate &candidate : member.overloads) {
    if (!candidate.staticMember &&
        candidate.record().genericParameters.empty() &&
        candidate.returnType == SemanticType::Void &&
        candidate.parameterTypes.size() == 1 &&
        candidate.parameterTypes.front() == arguments.arguments.front() &&
        candidate.id != 0 && candidate.declaration != nullptr) {
      return candidate.id;
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::classInfo(ClassId id) -> ClassInfo & {
  return classes.at(id - 1);
}

auto SemanticVisitor::Impl::classInfo(ClassId id) const -> const ClassInfo & {
  return classes.at(id - 1);
}

auto SemanticVisitor::Impl::findMember(const SemanticType &objectType,
                                       const Token &name) const
    -> const MemberInfo * {
  const ClassInfo *owner = classInfo(objectType);
  if (owner == nullptr) {
    return nullptr;
  }

  const auto found = owner->members.find(name.lexeme);
  return found == owner->members.end() ? nullptr : &found->second;
}

auto SemanticVisitor::Impl::resolveMember(const SemanticType &objectType,
                                          const Token &name)
    -> const MemberInfo * {
  const ClassInfo *owner = classInfo(objectType);
  if (owner == nullptr) {
    if (objectType != SemanticType::Unknown) {
      report(name, "Member access requires a class or struct value.");
    }
    return nullptr;
  }

  const MemberInfo *member = findMember(objectType, name);
  if (member == nullptr) {
    report(name, "Unknown member '" + name.lexeme + "' on '" +
                     owner->name.lexeme + "'.");
    return nullptr;
  }
  if (!isCompilerTrustedRequester(name) && member->symbol.compilerPrivate) {
    reportCompilerPrivateAccess(name, owner->name.lexeme + "." + name.lexeme);
    return nullptr;
  }
  if (member->symbol.type != SemanticType::Function &&
      member->access == AccessModifier::Private &&
      currentClass != member->symbol.ownerClass) {
    const ClassInfo &declaringOwner = classInfo(member->symbol.ownerClass);
    Diagnostic diagnostic =
        makeDiagnostic("GTI-S2007", DiagnosticPhase::Semantics, name,
                       "Member '" + name.lexeme + "' of '" +
                           declaringOwner.name.lexeme + "' is private.");
    diagnostic.related.push_back(
        {tokenSpan(member->symbol.declaration), "Member declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  return member;
}

auto SemanticVisitor::Impl::typeSpelling(const SemanticType &type) const
    -> std::string {
  return semantic_spelling::typeSpelling(semanticModel, type);
}

auto SemanticVisitor::Impl::tracksValueState(const Symbol &symbol) const
    -> bool {
  const bool localValue =
      symbol.bindingKind == SemanticBindingKind::LocalVariable ||
      symbol.bindingKind == SemanticBindingKind::Parameter;
  return localValue && symbol.type.kind != SemanticType::Reference &&
         (typeTraits(symbol.type).movable || packRequiresMove(symbol.type));
}

auto SemanticVisitor::Impl::declareReceiverState() -> void {
  if (!currentClass || scopes.empty()) {
    return;
  }
  scopes.back().insert_or_assign(
      std::string(receiverStateName),
      Symbol{.type = openClassType(*currentClass),
             .assignable = receiverAllowsMutation(currentReceiverMutability)});
}

auto SemanticVisitor::Impl::receiverStateSymbol() -> Symbol * {
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    const auto found = scope->find(std::string(receiverStateName));
    if (found != scope->end()) {
      return &found->second;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::placeForSymbol(Symbol &symbol) -> SemanticPlace {
  if (symbol.bindingKind == SemanticBindingKind::LocalVariable ||
      symbol.bindingKind == SemanticBindingKind::Parameter ||
      symbol.bindingKind == SemanticBindingKind::GlobalVariable ||
      symbol.bindingKind == SemanticBindingKind::StaticField) {
    return {.root = &symbol, .rootIdentity = toolingSymbolFor(symbol)};
  }
  if (symbol.bindingKind == SemanticBindingKind::Field &&
      !symbol.staticMember && symbol.variableDeclaration != nullptr) {
    return {.root = receiverStateSymbol(),
            .projections = {{.kind = PlaceProjectionKind::Field,
                             .field = toolingSymbolFor(symbol)}},
            .receiver = true};
  }
  return {};
}

auto SemanticVisitor::Impl::fixedArrayIndexProjection(
    const SemanticType &objectType, const ExprPtr &index,
    const Expr &selectionSource) -> std::optional<PlaceProjection> {
  if (objectType.kind != SemanticType::Array ||
      objectType.arguments.size() != 1) {
    return std::nullopt;
  }

  if (const auto constant = integerConstant(index.get());
      constant && !constant->negative &&
      objectType.arrayLengthParameterId == 0 &&
      constant->magnitude < objectType.arrayLength) {
    return PlaceProjection{.kind = PlaceProjectionKind::ConstantIndex,
                           .index = constant->magnitude};
  }
  if (index && objectType.arrayLengthParameterId == 0) {
    std::optional<ConstantValue> evaluated = semanticModel.findConstant(*index);
    if (!evaluated) {
      ConstexprInterpreter context = makeConstexprInterpreter();
      ConstexprEvaluation result = evaluateConstexprExpression(index, context);
      if (result) {
        evaluated = std::move(result.value);
      }
    }
    if (evaluated) {
      if (const auto *integer = std::get_if<ConstantInteger>(&*evaluated);
          integer != nullptr && !integer->negative &&
          integer->magnitude < objectType.arrayLength) {
        return PlaceProjection{.kind = PlaceProjectionKind::ConstantIndex,
                               .index = integer->magnitude};
      }
    }
  }
  return PlaceProjection{.kind = PlaceProjectionKind::DynamicIndex,
                         .selection =
                             semanticModel.placeSelection(selectionSource)};
}

auto SemanticVisitor::Impl::placeKey(const SemanticPlace &place) const
    -> PlaceKey {
  return {.domain = {.snapshot = semanticModel.placeSnapshot()},
          .root = place.receiver ? 0 : place.rootIdentity,
          .receiver = place.receiver,
          .projections = place.projections};
}

auto SemanticVisitor::Impl::semanticPlace(const Expr *expression)
    -> SemanticPlace {
  if (expression == nullptr) {
    return {};
  }
  const ExprKind kind = expression->exprKind();
  if (kind == ExprKind::Grouping) {
    return semanticPlace(
        static_cast<const Grouping *>(expression)->expression().get());
  }
  if (kind == ExprKind::Variable) {
    Symbol *symbol =
        resolvePlaceSymbol(static_cast<const Variable *>(expression)->name());
    return symbol == nullptr ? SemanticPlace{} : placeForSymbol(*symbol);
  }
  if (kind == ExprKind::QualifiedName) {
    Symbol *symbol = resolveQualifiedPlaceSymbol(
        static_cast<const QualifiedName *>(expression)->name());
    return symbol == nullptr ? SemanticPlace{} : placeForSymbol(*symbol);
  }
  if (kind == ExprKind::This) {
    return {.root = receiverStateSymbol(), .receiver = true};
  }
  if (kind == ExprKind::Get) {
    const auto *member = static_cast<const Get *>(expression);
    SemanticPlace place = semanticPlace(member->object().get());
    if (place.root == nullptr) {
      return {};
    }
    if (member->access().kind == TokenKind::ARROW) {
      place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
    }
    const MemberInfo *resolved =
        findMember(memberAccessObjectType(*member), member->name());
    if (resolved == nullptr) {
      return {};
    }
    if (resolved->symbol.type != SemanticType::Function) {
      if (resolved->symbol.variableDeclaration == nullptr) {
        return {};
      }
      place.projections.push_back(
          {.kind = PlaceProjectionKind::Field,
           .field = toolingSymbolFor(resolved->symbol)});
    }
    return place;
  }
  if (kind == ExprKind::Index) {
    const auto *index = static_cast<const Index *>(expression);
    SemanticPlace place = semanticPlace(index->object().get());
    const SemanticType *objectType = semanticModel.findType(*index->object());
    if (place.root != nullptr && objectType != nullptr) {
      if (const std::optional<PlaceProjection> projection =
              fixedArrayIndexProjection(*objectType, index->index(), *index)) {
        place.projections.push_back(*projection);
      }
    }
    return place;
  }
  if (kind == ExprKind::IndexSet) {
    return semanticPlace(*static_cast<const IndexSet *>(expression));
  }
  if (kind == ExprKind::Unary) {
    const auto *unary = static_cast<const Unary *>(expression);
    if (unary->oper().kind != TokenKind::STAR) {
      return {};
    }
    if (unary->rangeObject()) {
      SemanticPlace place = semanticPlace(unary->rangeObject().get());
      const SemanticType *objectType =
          semanticModel.findType(*unary->rangeObject());
      if (place.root != nullptr && objectType != nullptr &&
          objectType->kind == SemanticType::Array) {
        if (const std::optional<PlaceProjection> projection =
                fixedArrayIndexProjection(*objectType, unary->right(),
                                          *unary)) {
          place.projections.push_back(*projection);
        }
        return place;
      }
    }
    SemanticPlace place = semanticPlace(unary->right().get());
    if (place.root != nullptr) {
      place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
    }
    return place;
  }
  return {};
}

auto SemanticVisitor::Impl::semanticPlace(const ExprPtr &expression)
    -> SemanticPlace {
  return semanticPlace(expression.get());
}

auto SemanticVisitor::Impl::semanticPlace(const Set &expression)
    -> SemanticPlace {
  SemanticPlace place = semanticPlace(expression.object());
  if (place.root == nullptr) {
    return {};
  }
  if (expression.access().kind == TokenKind::ARROW) {
    place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
  }
  const MemberInfo *resolved =
      findMember(memberAccessObjectType(expression), expression.name());
  if (resolved == nullptr || resolved->symbol.variableDeclaration == nullptr) {
    return {};
  }
  place.projections.push_back({.kind = PlaceProjectionKind::Field,
                               .field = toolingSymbolFor(resolved->symbol)});
  return place;
}

auto SemanticVisitor::Impl::semanticPlace(const IndexSet &expression)
    -> SemanticPlace {
  SemanticPlace place = semanticPlace(expression.object());
  const SemanticType *objectType = semanticModel.findType(*expression.object());
  if (place.root != nullptr && objectType != nullptr) {
    if (const std::optional<PlaceProjection> projection =
            fixedArrayIndexProjection(*objectType, expression.index(),
                                      expression)) {
      place.projections.push_back(*projection);
    }
  }
  return place;
}

auto SemanticVisitor::Impl::projectedPlaceKey(
    const SemanticPlace &place,
    std::span<const PlaceProjection> projections) const -> PlaceKey {
  PlaceKey key = placeKey(place);
  key.projections.assign(projections.begin(), projections.end());
  return key;
}

auto SemanticVisitor::Impl::projectedValueState(
    const Symbol &root, std::span<const PlaceProjection> projections)
    -> ValueState {
  const auto found = std::find_if(
      root.projectedValueStates.begin(), root.projectedValueStates.end(),
      [&](const ProjectedValueState &candidate) {
        return std::ranges::equal(candidate.projections, projections);
      });
  return found == root.projectedValueStates.end() ? ValueState::Available
                                                  : found->state;
}

auto SemanticVisitor::Impl::unavailableProjectedState(
    const SemanticPlace &place, bool includeIndirectDescendants) const
    -> const ProjectedValueState * {
  if (place.root == nullptr) {
    return nullptr;
  }
  for (const ProjectedValueState &candidate :
       place.root->projectedValueStates) {
    if (candidate.state == ValueState::Available) {
      continue;
    }
    const PlaceRelationResult relation = placeRelation(
        projectedPlaceKey(place, candidate.projections), placeKey(place));
    if (!relation.compatibleDomain ||
        relation.relation == PlaceRelation::Equal ||
        relation.relation == PlaceRelation::LeftStrictPrefix ||
        relation.relation == PlaceRelation::MayAlias) {
      return &candidate;
    }
    if (relation.relation != PlaceRelation::RightStrictPrefix) {
      continue;
    }
    if (includeIndirectDescendants ||
        candidate.projections.size() == place.projections.size() ||
        candidate.projections[place.projections.size()].kind !=
            PlaceProjectionKind::Dereference) {
      return &candidate;
    }
  }
  return nullptr;
}

auto SemanticVisitor::Impl::unavailableAncestorState(
    const SemanticPlace &place) const -> const ProjectedValueState * {
  if (place.root == nullptr) {
    return nullptr;
  }
  const auto found = std::find_if(
      place.root->projectedValueStates.begin(),
      place.root->projectedValueStates.end(),
      [&](const ProjectedValueState &candidate) {
        if (candidate.state == ValueState::Available) {
          return false;
        }
        const PlaceRelationResult relation = placeRelation(
            projectedPlaceKey(place, candidate.projections), placeKey(place));
        return !relation.compatibleDomain ||
               relation.relation == PlaceRelation::LeftStrictPrefix ||
               relation.relation == PlaceRelation::MayAlias;
      });
  return found == place.root->projectedValueStates.end() ? nullptr : &*found;
}

auto SemanticVisitor::Impl::samePlace(const SemanticPlace &left,
                                      const SemanticPlace &right) const
    -> bool {
  if (left.root == nullptr || right.root == nullptr) {
    return false;
  }
  const PlaceRelationResult relation =
      placeRelation(placeKey(left), placeKey(right));
  return relation.compatibleDomain && relation.relation == PlaceRelation::Equal;
}

auto SemanticVisitor::Impl::movedSourcePlace(const ExprPtr &expression)
    -> SemanticPlace {
  const Call *call = directCall(expression);
  if (call == nullptr || resolvedIntrinsicKind(*call) != IntrinsicKind::Move ||
      call->arguments().size() != 1) {
    return {};
  }
  return semanticPlace(call->arguments().front());
}

auto SemanticVisitor::Impl::setProjectedValueState(const SemanticPlace &place,
                                                   ValueState state) -> void {
  if (place.root == nullptr || place.projections.empty()) {
    return;
  }
  auto found = std::find_if(place.root->projectedValueStates.begin(),
                            place.root->projectedValueStates.end(),
                            [&](const ProjectedValueState &candidate) {
                              return candidate.projections == place.projections;
                            });
  if (state == ValueState::Available) {
    if (found != place.root->projectedValueStates.end()) {
      place.root->projectedValueStates.erase(found);
    }
    return;
  }
  if (found == place.root->projectedValueStates.end()) {
    place.root->projectedValueStates.push_back(
        {.projections = place.projections, .state = state});
  } else {
    found->state = state;
  }
}

auto SemanticVisitor::Impl::reinitializePlace(const SemanticPlace &place)
    -> void {
  if (place.root == nullptr) {
    return;
  }
  if (place.projections.empty()) {
    place.root->projectedValueStates.clear();
    return;
  }
  std::erase_if(place.root->projectedValueStates,
                [&](const ProjectedValueState &candidate) {
                  const PlaceRelationResult relation = placeRelation(
                      placeKey(place),
                      projectedPlaceKey(place, candidate.projections));
                  return relation.compatibleDomain &&
                         (relation.relation == PlaceRelation::Equal ||
                          relation.relation == PlaceRelation::LeftStrictPrefix);
                });
}

auto SemanticVisitor::Impl::reportUnavailablePlace(
    const Token &use, const ProjectedValueState &state,
    const SemanticPlace *place) -> void {
  std::string name = place != nullptr && place->receiver ? "this"
                     : place != nullptr && place->root != nullptr &&
                             !place->root->declaration.lexeme.empty()
                         ? place->root->declaration.lexeme
                         : use.lexeme;
  for (const PlaceProjection &projection : state.projections) {
    if (projection.kind == PlaceProjectionKind::Field &&
        projection.field != 0) {
      if (const SymbolRecord *field =
              semanticModel.database().findSymbol(projection.field)) {
        name = name == "this" ? name + "." + field->name : field->name;
      }
    } else if (projection.kind == PlaceProjectionKind::ConstantIndex) {
      name += "[" + std::to_string(projection.index) + "]";
    } else if (projection.kind == PlaceProjectionKind::DynamicIndex) {
      name += "[dynamic index]";
    }
  }
  report(use,
         state.state == ValueState::Moved
             ? "Place '" + name + "' has already been moved."
             : "Place '" + name +
                   "' may have been moved on another control-flow path.",
         "GTI-S2018");
}

auto SemanticVisitor::Impl::validateProjectedPlaceRead(const Expr &expression)
    -> void {
  if (std::find(suppressedProjectedPlaceReads.begin(),
                suppressedProjectedPlaceReads.end(),
                &expression) != suppressedProjectedPlaceReads.end() ||
      exprAs<Assign>(&expression) != nullptr ||
      exprAs<Set>(&expression) != nullptr ||
      exprAs<IndexSet>(&expression) != nullptr ||
      exprAs<DereferenceSet>(&expression) != nullptr) {
    return;
  }
  SemanticPlace place = semanticPlace(&expression);
  const ProjectedValueState *state = unavailableProjectedState(place);
  if (state != nullptr) {
    reportUnavailablePlace(expressionToken(expression), *state, &place);
  } else if (place.root != nullptr) {
    semanticModel.recordOwnershipEvent(
        expression,
        {.kind = OwnershipEventKind::Read, .place = placeKey(place)});
  }
}

auto SemanticVisitor::Impl::analyzeProjectionBase(const ExprPtr &expression)
    -> SemanticType {
  suppressedProjectedPlaceReads.push_back(expression.get());
  const SemanticType result = analyze(expression);
  suppressedProjectedPlaceReads.pop_back();
  return result;
}

auto SemanticVisitor::Impl::requireInitializedReceiver(const Token &location)
    -> void {
  if (currentReceiverMutability == ReceiverMutability::Consuming) {
    return;
  }
  const Symbol *receiver = receiverStateSymbol();
  if (receiver == nullptr || receiver->projectedValueStates.empty()) {
    return;
  }
  const ProjectedValueState &state = receiver->projectedValueStates.front();
  report(location,
         state.state == ValueState::Moved
             ? "Method cannot return while part of 'this' remains moved."
             : "Method cannot return because part of 'this' may remain "
               "moved on another control-flow path.",
         "GTI-S2018");
  diagnostics.back().hints.emplace_back(
      "Reinitialize every moved field on each reachable path before the "
      "method returns.");
}

auto SemanticVisitor::Impl::packRequiresMove(const SemanticType &type) const
    -> bool {
  if (type.kind != SemanticType::TypePack || !type.concretePack) {
    return false;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [this](const SemanticType &element) {
                       return element.kind == SemanticType::TypePack
                                  ? packRequiresMove(element)
                                  : isMoveOnlyOwnerType(element);
                     });
}

auto SemanticVisitor::Impl::reportUnavailableValue(const Token &use,
                                                   const Symbol &symbol)
    -> void {
  report(use,
         symbol.valueState == ValueState::Moved
             ? "Value '" + use.lexeme + "' has already been moved."
             : "Value '" + use.lexeme +
                   "' may have been moved on another control-flow path.",
         "GTI-S2018");
}

auto SemanticVisitor::Impl::mergedValueState(ValueState left, ValueState right)
    -> ValueState {
  return left | right;
}

auto SemanticVisitor::Impl::mergedProjectedValueStates(const Symbol &left,
                                                       const Symbol &right)
    -> std::vector<ProjectedValueState> {
  std::vector<ProjectedValueState> result;
  const auto merge = [&](const std::vector<PlaceProjection> &projections) {
    if (std::any_of(result.begin(), result.end(),
                    [&](const ProjectedValueState &candidate) {
                      return candidate.projections == projections;
                    })) {
      return;
    }
    const ValueState state =
        mergedValueState(projectedValueState(left, projections),
                         projectedValueState(right, projections));
    if (state != ValueState::Available) {
      result.push_back({.projections = projections, .state = state});
    }
  };
  for (const ProjectedValueState &state : left.projectedValueStates) {
    merge(state.projections);
  }
  for (const ProjectedValueState &state : right.projectedValueStates) {
    merge(state.projections);
  }
  return result;
}

auto SemanticVisitor::Impl::mergeValueStates(const ScopeStack &base,
                                             const ScopeStack &left,
                                             const ScopeStack &right) -> void {
  const std::size_t depth =
      std::min({scopes.size(), base.size(), left.size(), right.size()});
  for (std::size_t scopeIndex = 0; scopeIndex < depth; ++scopeIndex) {
    for (const auto &[name, baseSymbol] : base[scopeIndex]) {
      const auto leftSymbol = left[scopeIndex].find(name);
      const auto rightSymbol = right[scopeIndex].find(name);
      const auto target = scopes[scopeIndex].find(name);
      if (leftSymbol != left[scopeIndex].end() &&
          rightSymbol != right[scopeIndex].end() &&
          target != scopes[scopeIndex].end()) {
        if (tracksValueState(baseSymbol)) {
          target->second.valueState = mergedValueState(
              leftSymbol->second.valueState, rightSymbol->second.valueState);
        }
        target->second.projectedValueStates =
            mergedProjectedValueStates(leftSymbol->second, rightSymbol->second);
        target->second.storageLoans = leftSymbol->second.storageLoans;
        for (const SemanticLoanId loan : rightSymbol->second.storageLoans) {
          appendUniqueLoan(target->second.storageLoans, loan);
        }
        target->second.retainedLoan =
            leftSymbol->second.retainedLoan == rightSymbol->second.retainedLoan
                ? leftSymbol->second.retainedLoan
                : baseSymbol.retainedLoan;
      }
    }
  }
}

auto SemanticVisitor::Impl::mergedValueStates(
    const ScopeStack &base, const std::vector<ScopeStack> &states)
    -> ScopeStack {
  if (states.empty()) {
    return base;
  }
  ScopeStack merged = states.front();
  if (merged.size() > base.size()) {
    merged.resize(base.size());
  }
  for (std::size_t index = 1; index < states.size(); ++index) {
    scopes = base;
    mergeValueStates(base, merged, states[index]);
    merged = scopes;
  }
  return merged;
}

auto SemanticVisitor::Impl::applyMoveStates(const ScopeStack &source) -> void {
  const std::size_t depth = std::min(scopes.size(), source.size());
  for (std::size_t scopeIndex = 0; scopeIndex < depth; ++scopeIndex) {
    for (auto &[name, target] : scopes[scopeIndex]) {
      const auto found = source[scopeIndex].find(name);
      if (found == source[scopeIndex].end()) {
        continue;
      }
      target.valueState = found->second.valueState;
      target.projectedValueStates = found->second.projectedValueStates;
    }
  }
}

auto SemanticVisitor::Impl::beginValueControlFlow(ValueControlFlowKind kind,
                                                  const Stmt &statement)
    -> void {
  valueControlFlow.push_back({.kind = kind, .statement = &statement});
}

auto SemanticVisitor::Impl::endValueControlFlow() -> ValueControlFlowContext {
  if (valueControlFlow.empty()) {
    return {};
  }
  ValueControlFlowContext context = std::move(valueControlFlow.back());
  valueControlFlow.pop_back();
  return context;
}

auto SemanticVisitor::Impl::recordValueControlFlowExit(bool isBreak) -> void {
  if (isBreak) {
    if (!valueControlFlow.empty()) {
      valueControlFlow.back().breakStates.push_back(scopes);
    }
    return;
  }
  const auto loop =
      std::find_if(valueControlFlow.rbegin(), valueControlFlow.rend(),
                   [](const ValueControlFlowContext &context) {
                     return context.kind == ValueControlFlowKind::Loop;
                   });
  if (loop != valueControlFlow.rend()) {
    loop->continueStates.push_back(scopes);
  }
}

auto SemanticVisitor::Impl::mergedValueControlFlow(
    const std::vector<ValueControlFlowContext> &base,
    const std::vector<ValueControlFlowContext> &left,
    const std::vector<ValueControlFlowContext> &right)
    -> std::vector<ValueControlFlowContext> {
  std::vector<ValueControlFlowContext> merged = base;
  const std::size_t depth = std::min({base.size(), left.size(), right.size()});
  for (std::size_t index = 0; index < depth; ++index) {
    for (std::size_t state = base[index].breakStates.size();
         state < left[index].breakStates.size(); ++state) {
      merged[index].breakStates.push_back(left[index].breakStates[state]);
    }
    for (std::size_t state = base[index].breakStates.size();
         state < right[index].breakStates.size(); ++state) {
      merged[index].breakStates.push_back(right[index].breakStates[state]);
    }
    for (std::size_t state = base[index].continueStates.size();
         state < left[index].continueStates.size(); ++state) {
      merged[index].continueStates.push_back(left[index].continueStates[state]);
    }
    for (std::size_t state = base[index].continueStates.size();
         state < right[index].continueStates.size(); ++state) {
      merged[index].continueStates.push_back(
          right[index].continueStates[state]);
    }
  }
  return merged;
}

auto SemanticVisitor::Impl::validateLoopBackedge(const ScopeStack &beforeLoop,
                                                 const ScopeStack &backedge,
                                                 const Token &location)
    -> void {
  const std::size_t depth = std::min(beforeLoop.size(), backedge.size());
  for (std::size_t scopeIndex = 0; scopeIndex < depth; ++scopeIndex) {
    for (const auto &[name, beforeSymbol] : beforeLoop[scopeIndex]) {
      const auto after = backedge[scopeIndex].find(name);
      if (after == backedge[scopeIndex].end()) {
        continue;
      }
      const Symbol &afterSymbol = after->second;
      if (tracksValueState(beforeSymbol) &&
          beforeSymbol.valueState == ValueState::Available &&
          afterSymbol.valueState != ValueState::Available) {
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2018", DiagnosticPhase::Semantics, location,
            "Value '" + name +
                "' is not available at a reachable loop backedge.");
        diagnostic.related.push_back(
            {tokenSpan(beforeSymbol.declaration), "Value declared here."});
        diagnostic.hints.emplace_back(
            "Reinitialize the value on every path before 'continue' or the "
            "end of the loop body.");
        diagnostics.emplace_back(std::move(diagnostic));
      }

      for (const ProjectedValueState &state :
           afterSymbol.projectedValueStates) {
        if (state.state == ValueState::Available ||
            projectedValueState(beforeSymbol, state.projections) !=
                ValueState::Available) {
          continue;
        }
        const SymbolRecord *field = nullptr;
        std::optional<std::uint64_t> element;
        for (const PlaceProjection &projection : state.projections) {
          if (projection.kind == PlaceProjectionKind::Field &&
              projection.field != 0) {
            field = semanticModel.database().findSymbol(projection.field);
          } else if (projection.kind == PlaceProjectionKind::ConstantIndex) {
            element = projection.index;
          }
        }
        std::string placeName = field == nullptr ? name : field->name;
        if (element) {
          placeName += "[" + std::to_string(*element) + "]";
        }
        Diagnostic diagnostic = makeDiagnostic(
            "GTI-S2018", DiagnosticPhase::Semantics, location,
            "Place '" + placeName +
                "' is not available at a reachable loop backedge.");
        diagnostic.related.push_back({field == nullptr
                                          ? tokenSpan(beforeSymbol.declaration)
                                          : field->nameSpan,
                                      "Place declared here."});
        diagnostic.hints.emplace_back(
            "Reinitialize the place on every path before 'continue' or the "
            "end of the loop body.");
        diagnostics.emplace_back(std::move(diagnostic));
      }
    }
  }
}

auto SemanticVisitor::Impl::beginScope() -> void { scopes.emplace_back(); }

auto SemanticVisitor::Impl::endScope() -> void {
  if (scopes.empty()) {
    return;
  }
  for (auto &[_, symbol] : scopes.back()) {
    removeLoanCarrier(symbol);
  }
  scopes.pop_back();
}

auto SemanticVisitor::Impl::report(const Token &token, std::string message,
                                   std::string code) -> void {
  reporter.report(token, std::move(message), std::move(code));
}

auto SemanticVisitor::Impl::requireUnsafe(const Expr &expression,
                                          UnsafeOperationKind operation,
                                          const Token &token,
                                          std::string_view description)
    -> void {
  semanticModel.recordUnsafeOperation(expression, operation);
  if (unsafeDepth != 0) {
    return;
  }
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2055", DiagnosticPhase::Semantics, token,
                     std::string(description) + " requires an unsafe block.");
  diagnostic.hints.emplace_back(
      "Wrap only the operation whose pointer validity you have established "
      "in 'unsafe { ... }'.");
  diagnostics.emplace_back(std::move(diagnostic));
}

auto SemanticVisitor::Impl::containsRawPointer(const SemanticType &type)
    -> bool {
  if (type.kind == SemanticType::RawPointer) {
    return true;
  }
  return std::any_of(type.arguments.begin(), type.arguments.end(),
                     [](const SemanticType &argument) {
                       return containsRawPointer(argument);
                     });
}

auto SemanticVisitor::Impl::requireBool(const ExprPtr &expression,
                                        SemanticType type, const Token &token,
                                        std::string_view message) -> void {
  if (type.kind == SemanticType::Class && expression) {
    const std::optional<CallableCandidate> selected =
        resolveOperator(*expression, OverloadedOperator::ContextualBool,
                        expression, type, token, {}, {}, true);
    (void)selected;
    return;
  }
  if (type != SemanticType::Unknown && !isContextuallyBool(type)) {
    report(token, std::string(message) + " Found '" + typeSpelling(type) + "'.",
           "GTI-S2003");
  }
}

auto SemanticVisitor::Impl::acceptsContextualIntegerOperand(TokenKind operation)
    -> bool {
  switch (operation) {
  case TokenKind::EQUAL_EQUAL:
  case TokenKind::BANG_EQUAL:
  case TokenKind::LESS:
  case TokenKind::LESS_EQUAL:
  case TokenKind::GREATER:
  case TokenKind::GREATER_EQUAL:
  case TokenKind::PLUS:
  case TokenKind::MINUS:
  case TokenKind::STAR:
  case TokenKind::SLASH:
  case TokenKind::PERCENT:
  case TokenKind::AMPERSAND:
  case TokenKind::CARET:
  case TokenKind::PIPE:
    return true;
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::contextualIntegerLiteral(const Expr *expression,
                                                     bool allowSign)
    -> const LiteralExpr * {
  if (expression == nullptr) {
    return nullptr;
  }
  if (expression->exprKind() == ExprKind::LiteralExpr) {
    const auto *literal = static_cast<const LiteralExpr *>(expression);
    return std::holds_alternative<std::uint64_t>(literal->value()) ? literal
                                                                   : nullptr;
  }
  if (expression->exprKind() == ExprKind::Grouping) {
    const auto *grouping = static_cast<const Grouping *>(expression);
    return contextualIntegerLiteral(grouping->expression().get(), allowSign);
  }
  const auto *unary = expression->exprKind() == ExprKind::Unary
                          ? static_cast<const Unary *>(expression)
                          : nullptr;
  if (allowSign && unary != nullptr &&
      (unary->oper().kind == TokenKind::PLUS ||
       unary->oper().kind == TokenKind::MINUS)) {
    return contextualIntegerLiteral(unary->right().get(), false);
  }
  return nullptr;
}

auto SemanticVisitor::Impl::isContextualIntegerLiteral(const Expr *expression)
    -> bool {
  return contextualIntegerLiteral(expression) != nullptr;
}

auto SemanticVisitor::Impl::analyzeContextualIntegerOperand(
    const ExprPtr &expression, SemanticType target) -> SemanticType {
  if (!isIntegral(target)) {
    return analyze(expression);
  }

  const std::optional<IntegerConstant> value =
      integerConstant(expression.get());
  if (isInteger(target) && value && !integerFits(target, *value)) {
    const LiteralExpr *literal = contextualIntegerLiteral(expression.get());
    const std::string valueSpelling =
        (value->negative ? "-" : "") + std::to_string(value->magnitude);
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2004", DiagnosticPhase::Semantics,
        literal == nullptr ? expressionToken(expression) : literal->token(),
        "Integer literal '" + valueSpelling +
            "' does not fit the contextual operand type '" +
            typeSpelling(target) + "'.");
    diagnostic.hints.emplace_back(
        "Choose a representable value or explicitly convert the other "
        "operand to a wider integer type.");
    diagnostics.emplace_back(std::move(diagnostic));
  }

  const std::optional<SemanticType> enclosingType =
      contextualOperatorIntegerType;
  contextualOperatorIntegerType = target;
  const SemanticType result = analyze(expression);
  contextualOperatorIntegerType = enclosingType;
  return result;
}

auto SemanticVisitor::Impl::analyzeBinaryOperands(const Binary &expression)
    -> std::pair<SemanticType, SemanticType> {
  if (!acceptsContextualIntegerOperand(expression.oper().kind)) {
    return {analyze(expression.left()), analyze(expression.right())};
  }

  const bool leftLiteral = isContextualIntegerLiteral(expression.left().get());
  const bool rightLiteral =
      isContextualIntegerLiteral(expression.right().get());
  if (leftLiteral == rightLiteral) {
    return {analyze(expression.left()), analyze(expression.right())};
  }
  if (rightLiteral) {
    const SemanticType left = analyze(expression.left());
    return {left, analyzeContextualIntegerOperand(expression.right(), left)};
  }

  const SemanticType right = analyze(expression.right());
  return {analyzeContextualIntegerOperand(expression.left(), right), right};
}

auto SemanticVisitor::Impl::requireNumeric(SemanticType left,
                                           SemanticType right,
                                           const Token &token) -> void {
  if ((left != SemanticType::Unknown && !isNumeric(left)) ||
      (right != SemanticType::Unknown && !isNumeric(right))) {
    report(token,
           "Operator '" + token.lexeme +
               "' requires numeric operands but found '" + typeSpelling(left) +
               "' and '" + typeSpelling(right) + "'.",
           "GTI-S2004");
  }
}

auto SemanticVisitor::Impl::requireOrdered(SemanticType left,
                                           SemanticType right,
                                           const Token &token) -> void {
  if ((left != SemanticType::Unknown && !isOrdered(left)) ||
      (right != SemanticType::Unknown && !isOrdered(right))) {
    report(token,
           "Operator '" + token.lexeme +
               "' requires ordered operands but found '" + typeSpelling(left) +
               "' and '" + typeSpelling(right) + "'.",
           "GTI-S2004");
  }
}

auto SemanticVisitor::Impl::requireInteger(SemanticType left,
                                           SemanticType right,
                                           const Token &token) -> void {
  if ((left != SemanticType::Unknown && !isIntegral(left)) ||
      (right != SemanticType::Unknown && !isIntegral(right))) {
    report(token,
           "Operator '" + token.lexeme +
               "' requires integer operands but found '" + typeSpelling(left) +
               "' and '" + typeSpelling(right) + "'.",
           "GTI-S2004");
  }
}

auto SemanticVisitor::Impl::validateIntegerDivisor(const Token &operation,
                                                   const Expr *right) -> void {
  const std::optional<IntegerConstant> divisor = integerConstant(right);
  if (!divisor || divisor->magnitude != 0) {
    return;
  }
  report(operation,
         operation.kind == TokenKind::PERCENT ||
                 operation.kind == TokenKind::PERCENT_EQUAL
             ? "Modulo divisor cannot be zero."
             : "Integer division by zero is not allowed.",
         "GTI-S2004");
}

auto SemanticVisitor::Impl::validateCompoundAssignment(
    const Token &operation, SemanticType target, SemanticType value,
    const Expr *valueExpression) -> void {
  if (target.kind == SemanticType::RawPointer) {
    if (rejectCOpaquePointeeOperation(target, operation,
                                      "raw-pointer arithmetic")) {
      return;
    }
    if ((operation.kind == TokenKind::PLUS_EQUAL ||
         operation.kind == TokenKind::MINUS_EQUAL) &&
        isInteger(value) && target.arguments.size() == 1 &&
        target.arguments.front() != SemanticType::Void) {
      return;
    }
    report(operation,
           "Raw pointers support only '+=' and '-=' with an integer "
           "offset, and void* does not support arithmetic.",
           "GTI-S2056");
    return;
  }
  const auto rejectUnsafeFloatDestination = [&]() {
    if (isIntegral(target) && semanticFloatFormat(value).has_value()) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2003", DiagnosticPhase::Semantics, operation,
          "Compound assignment to an integer cannot use a floating-point "
          "right operand.");
      diagnostic.hints.emplace_back(
          "Convert the right operand explicitly before applying '" +
          operation.lexeme + "'.");
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }
    if (target == SemanticType::Float && value == SemanticType::Double) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2003", DiagnosticPhase::Semantics, operation,
          "Compound assignment to 'float' cannot implicitly narrow a "
          "'double' right operand.");
      diagnostic.hints.emplace_back(
          "Convert the right operand to 'float' explicitly before applying "
          "'" +
          operation.lexeme + "'.");
      diagnostics.emplace_back(std::move(diagnostic));
      return true;
    }
    return false;
  };
  const auto requireSafeCommonIntegerType = [&]() {
    if (isInteger(target) && isInteger(value) &&
        numericResult(target, value, nullptr, valueExpression) ==
            SemanticType::Unknown) {
      report(operation,
             "Signed and unsigned operands have no safe common type.",
             "GTI-S2004");
    }
  };

  switch (operation.kind) {
  case TokenKind::PLUS_EQUAL:
  case TokenKind::MINUS_EQUAL:
  case TokenKind::STAR_EQUAL:
  case TokenKind::SLASH_EQUAL:
    requireNumeric(target, value, operation);
    if (rejectUnsafeFloatDestination()) {
      return;
    }
    requireSafeCommonIntegerType();
    if (operation.kind == TokenKind::SLASH_EQUAL && isIntegral(target) &&
        isIntegral(value)) {
      validateIntegerDivisor(operation, valueExpression);
    }
    return;
  case TokenKind::PERCENT_EQUAL:
  case TokenKind::AMPERSAND_EQUAL:
  case TokenKind::PIPE_EQUAL:
  case TokenKind::CARET_EQUAL:
    requireInteger(target, value, operation);
    requireSafeCommonIntegerType();
    if (operation.kind == TokenKind::PERCENT_EQUAL && isIntegral(target) &&
        isIntegral(value)) {
      validateIntegerDivisor(operation, valueExpression);
    }
    return;
  case TokenKind::SHIFT_LEFT_EQUAL:
  case TokenKind::SHIFT_RIGHT_EQUAL:
    requireInteger(target, value, operation);
    if (isIntegral(target) && isIntegral(value)) {
      validateShiftCount(promotedInteger(target), valueExpression, operation);
    }
    return;
  default:
    report(operation, "Unsupported compound assignment operator.", "GTI-S2004");
  }
}

auto SemanticVisitor::Impl::validateShiftCount(SemanticType result,
                                               const Expr *right,
                                               const Token &token) -> void {
  const std::optional<IntegerConstant> count = integerConstant(right);
  if (!count) {
    return;
  }
  if (count->negative) {
    report(token, "Shift count cannot be negative.");
    return;
  }
  const int width = integerRank(result);
  if (width != 0 && count->magnitude >= static_cast<std::uint64_t>(width)) {
    report(token,
           "Shift count must be less than " + std::to_string(width) + ".");
  }
}

auto SemanticVisitor::Impl::isInteger(SemanticType type) -> bool {
  return type == SemanticType::Int8 || type == SemanticType::Int16 ||
         type == SemanticType::Int32 || type == SemanticType::Int64 ||
         type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
         type == SemanticType::UInt32 || type == SemanticType::UInt64;
}

auto SemanticVisitor::Impl::constraintImplies(GenericConstraintSet actual,
                                              GenericConstraintKind required)
    -> bool {
  if (required == GenericConstraintKind::None ||
      required == GenericConstraintKind::Invalid ||
      invalidConstraintSet(actual)) {
    return true;
  }
  if (hasConstraint(actual, required)) {
    return true;
  }
  if (!hasConstraint(actual, GenericConstraintKind::AtomicScalar)) {
    return false;
  }
  switch (required) {
  case GenericConstraintKind::CopyConstructible:
  case GenericConstraintKind::Copyable:
  case GenericConstraintKind::Movable:
  case GenericConstraintKind::Transferable:
  case GenericConstraintKind::Shareable:
  case GenericConstraintKind::DefaultInitializable:
  case GenericConstraintKind::EqualityComparable:
    return true;
  default:
    return false;
  }
}

auto SemanticVisitor::Impl::constraintOf(const SemanticType &type) const
    -> GenericConstraintSet {
  if (type.kind != SemanticType::TypeParameter &&
      type.kind != SemanticType::TypePack) {
    return 0;
  }
  const auto found = genericConstraints.find(type.genericParameterId);
  const GenericConstraintSet declared =
      found == genericConstraints.end() ? 0 : found->second;
  return declared | activeAtomicConstraints(type);
}

auto SemanticVisitor::Impl::isNumeric(SemanticType type) const -> bool {
  return isInteger(type) || semanticFloatFormat(type).has_value() ||
         (type.kind == SemanticType::TypeParameter &&
          constraintImplies(constraintOf(type),
                            GenericConstraintKind::Numeric));
}

auto SemanticVisitor::Impl::isOrdered(SemanticType type) const -> bool {
  return isInteger(type) || semanticFloatFormat(type).has_value() ||
         (type.kind == SemanticType::TypeParameter &&
          constraintImplies(constraintOf(type),
                            GenericConstraintKind::RelationallyOrdered));
}

auto SemanticVisitor::Impl::isIntegral(SemanticType type) const -> bool {
  return isInteger(type) ||
         (type.kind == SemanticType::TypeParameter &&
          constraintImplies(constraintOf(type),
                            GenericConstraintKind::Integral));
}

auto SemanticVisitor::Impl::isSignedNumeric(SemanticType type) const -> bool {
  return isSignedInteger(type) || semanticFloatFormat(type).has_value() ||
         (type.kind == SemanticType::TypeParameter &&
          constraintImplies(constraintOf(type),
                            GenericConstraintKind::SignedNumeric));
}

auto SemanticVisitor::Impl::isSignedInteger(SemanticType type) -> bool {
  return type == SemanticType::Int8 || type == SemanticType::Int16 ||
         type == SemanticType::Int32 || type == SemanticType::Int64;
}

auto SemanticVisitor::Impl::isUnsignedInteger(SemanticType type) -> bool {
  return type == SemanticType::UInt8 || type == SemanticType::UInt16 ||
         type == SemanticType::UInt32 || type == SemanticType::UInt64;
}

auto SemanticVisitor::Impl::integerRank(SemanticType type) -> int {
  switch (type.kind) {
  case SemanticType::Int8:
  case SemanticType::UInt8:
    return 8;
  case SemanticType::Int16:
  case SemanticType::UInt16:
    return 16;
  case SemanticType::Int32:
  case SemanticType::UInt32:
    return 32;
  case SemanticType::Int64:
  case SemanticType::UInt64:
    return 64;
  default:
    return 0;
  }
}

auto SemanticVisitor::Impl::integerFits(SemanticType type,
                                        IntegerConstant value) -> bool {
  const int rank = integerRank(type);
  if (rank == 0) {
    return false;
  }
  if (isUnsignedInteger(type)) {
    const std::uint64_t maximum =
        rank == 64 ? std::numeric_limits<std::uint64_t>::max()
                   : (std::uint64_t{1} << rank) - 1;
    return !value.negative && value.magnitude <= maximum;
  }
  const std::uint64_t limit = std::uint64_t{1} << (rank - 1);
  return value.negative ? value.magnitude <= limit : value.magnitude < limit;
}

auto SemanticVisitor::Impl::integerConstant(const Expr *expression)
    -> std::optional<IntegerConstant> {
  if (expression == nullptr) {
    return std::nullopt;
  }
  if (const auto *literal = exprAs<LiteralExpr>(expression)) {
    const auto *magnitude = std::get_if<std::uint64_t>(&literal->value());
    if (magnitude == nullptr) {
      return std::nullopt;
    }
    return IntegerConstant{.negative = false, .magnitude = *magnitude};
  }
  if (const auto *grouping = exprAs<Grouping>(expression)) {
    return integerConstant(grouping->expression().get());
  }
  const auto *unary = exprAs<Unary>(expression);
  if (unary == nullptr || (unary->oper().kind != TokenKind::MINUS &&
                           unary->oper().kind != TokenKind::PLUS)) {
    return std::nullopt;
  }
  std::optional<IntegerConstant> value = integerConstant(unary->right().get());
  if (!value || unary->oper().kind == TokenKind::PLUS) {
    return value;
  }
  value->negative = value->magnitude != 0 && !value->negative;
  return value;
}

auto SemanticVisitor::Impl::switchCaseConstant(
    const Expr *expression, const SemanticType &subjectType) const
    -> std::optional<SwitchCaseValue> {
  if (expression == nullptr) {
    return std::nullopt;
  }
  if (const auto *grouping = exprAs<Grouping>(expression)) {
    return switchCaseConstant(grouping->expression().get(), subjectType);
  }
  if (subjectType.kind == SemanticType::Enum) {
    const auto *qualified = exprAs<QualifiedName>(expression);
    if (qualified == nullptr) {
      return std::nullopt;
    }
    const ResolvedEnumeratorInfo *enumerator =
        semanticModel.findEnumerator(*qualified);
    if (enumerator == nullptr || enumerator->owner != subjectType.enumId) {
      return std::nullopt;
    }
    return SwitchCaseValue{.kind = SwitchCaseKind::Enumerator,
                           .type = subjectType,
                           .value = enumerator->value,
                           .enumOwner = enumerator->owner};
  }
  if (subjectType == SemanticType::Char) {
    const auto *literal = exprAs<LiteralExpr>(expression);
    if (literal == nullptr) {
      return std::nullopt;
    }
    const auto *character = std::get_if<CharacterLiteral>(&literal->value());
    if (character == nullptr) {
      return std::nullopt;
    }
    return SwitchCaseValue{.kind = SwitchCaseKind::Character,
                           .type = subjectType,
                           .value =
                               EnumConstant{.magnitude = character->value}};
  }
  if (!isInteger(subjectType)) {
    return std::nullopt;
  }

  const Expr *constantExpression = expression;
  if (const auto *conversion = exprAs<Conversion>(constantExpression)) {
    constantExpression = conversion->value().get();
  } else if (const auto *call = exprAs<Call>(constantExpression)) {
    const ResolvedCallInfo *resolution = semanticModel.findCall(*call);
    if (resolution == nullptr ||
        resolution->intrinsic != IntrinsicKind::NumericAliasConversion ||
        call->arguments().size() != 1) {
      return std::nullopt;
    }
    constantExpression = call->arguments().front().get();
  }
  const std::optional<IntegerConstant> integer =
      integerConstant(constantExpression);
  if (!integer) {
    return std::nullopt;
  }
  return SwitchCaseValue{.kind = SwitchCaseKind::Integer,
                         .type = subjectType,
                         .value =
                             EnumConstant{.negative = integer->negative,
                                          .magnitude = integer->magnitude}};
}

auto SemanticVisitor::Impl::integerRangeFits(SemanticType target,
                                             SemanticType value) -> bool {
  if (isSignedInteger(target) == isSignedInteger(value)) {
    return integerRank(value) <= integerRank(target);
  }
  return isSignedInteger(target) && isUnsignedInteger(value) &&
         integerRank(value) < integerRank(target);
}

auto SemanticVisitor::Impl::promotedInteger(SemanticType type) -> SemanticType {
  if (type == SemanticType::Int8 || type == SemanticType::Int16 ||
      type == SemanticType::UInt8 || type == SemanticType::UInt16) {
    return SemanticType::Int32;
  }
  return type;
}

auto SemanticVisitor::Impl::widerInteger(SemanticType left, SemanticType right)
    -> SemanticType {
  return integerRank(left) >= integerRank(right) ? left : right;
}

auto SemanticVisitor::Impl::canConvertToUnsigned(SemanticType originalType,
                                                 const Expr *expression,
                                                 SemanticType unsignedTarget)
    -> bool {
  if (isUnsignedInteger(originalType) &&
      integerRank(originalType) <= integerRank(unsignedTarget)) {
    return true;
  }
  if (const std::optional<IntegerConstant> constant =
          integerConstant(expression)) {
    return integerFits(unsignedTarget, *constant);
  }
  return false;
}

auto SemanticVisitor::Impl::isContextuallyBool(const SemanticType &type)
    -> bool {
  return type == SemanticType::Bool || type.kind == SemanticType::Expected;
}

auto SemanticVisitor::Impl::isExpectedVoid(const SemanticType &type) -> bool {
  return type.kind == SemanticType::Expected && type.arguments.size() == 2 &&
         type.arguments[0] == SemanticType::Void;
}

auto SemanticVisitor::Impl::numericResult(SemanticType left, SemanticType right,
                                          const Expr *leftExpression,
                                          const Expr *rightExpression)
    -> SemanticType {
  if (left == SemanticType::Unknown || right == SemanticType::Unknown) {
    return SemanticType::Unknown;
  }
  if (left == SemanticType::Double || right == SemanticType::Double) {
    return SemanticType::Double;
  }
  if (left == SemanticType::Float || right == SemanticType::Float) {
    return SemanticType::Float;
  }
  if (!isInteger(left) || !isInteger(right)) {
    return SemanticType::Unknown;
  }

  if (isSignedInteger(left) != isSignedInteger(right)) {
    const SemanticType signedOriginal = isSignedInteger(left) ? left : right;
    const SemanticType unsignedOriginal =
        isUnsignedInteger(left) ? left : right;
    if (integerRangeFits(signedOriginal, unsignedOriginal)) {
      return promotedInteger(signedOriginal);
    }
    if (integerRank(left) < 32 && integerRank(right) < 32) {
      return SemanticType::Int32;
    }
  }

  const SemanticType promotedLeft = promotedInteger(left);
  const SemanticType promotedRight = promotedInteger(right);
  if (isSignedInteger(promotedLeft) == isSignedInteger(promotedRight)) {
    return widerInteger(promotedLeft, promotedRight);
  }

  const bool leftIsSigned = isSignedInteger(promotedLeft);
  const SemanticType signedType = leftIsSigned ? promotedLeft : promotedRight;
  const SemanticType unsignedType = leftIsSigned ? promotedRight : promotedLeft;
  if (integerRank(signedType) > integerRank(unsignedType)) {
    return signedType;
  }

  const SemanticType originalSignedType = leftIsSigned ? left : right;
  const Expr *signedExpression =
      leftIsSigned ? leftExpression : rightExpression;
  if (canConvertToUnsigned(originalSignedType, signedExpression,
                           unsignedType)) {
    return unsignedType;
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::isOwnershipAssignable(
    const SemanticType &target, const SemanticType &value,
    const ExprPtr &expression) const -> bool {
  if (target.kind == SemanticType::UniqueOwner &&
      value == SemanticType::NullPtr) {
    return true;
  }
  if (!isAssignable(target, value, expression.get())) {
    return false;
  }
  if (target != value || !expression || target.kind == SemanticType::TypePack) {
    return true;
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  if (info == nullptr) {
    return true;
  }
  const SemanticTypeTraits traits = typeTraits(target);
  return info->category == ValueCategory::Value ? traits.movable
                                                : traits.copyable;
}

auto SemanticVisitor::Impl::isOwnershipAssignment(
    const SemanticType &target, const SemanticType &value,
    const ExprPtr &expression) const -> bool {
  const ExpressionInfo *info =
      expression ? semanticModel.findExpression(*expression) : nullptr;
  const bool moving = info != nullptr && info->category == ValueCategory::Value;
  const SemanticTypeTraits traits = typeTraits(target);
  if ((moving && !traits.moveAssignable) ||
      (!moving && !traits.copyAssignable)) {
    return target == SemanticType::Unknown || value == SemanticType::Unknown;
  }
  return isOwnershipAssignable(target, value, expression);
}

auto SemanticVisitor::Impl::isAssignable(SemanticType target,
                                         SemanticType value,
                                         const Expr *expression) -> bool {
  if (target == SemanticType::Unknown || value == SemanticType::Unknown) {
    return true;
  }
  if (target == SemanticType::Double &&
      (isInteger(value) || value == SemanticType::Float)) {
    return true;
  }
  if (target == SemanticType::Float && isInteger(value)) {
    return true;
  }
  if (target == SemanticType::CString && value == SemanticType::NullPtr) {
    return true;
  }
  if (target.kind == SemanticType::NativeFunction &&
      value == SemanticType::NullPtr) {
    return true;
  }
  if (target.kind == SemanticType::RawPointer) {
    if (value == SemanticType::NullPtr) {
      return true;
    }
    if (value.kind != SemanticType::RawPointer ||
        target.arguments.size() != 1 || value.arguments.size() != 1 ||
        target.arguments.front() != value.arguments.front()) {
      return false;
    }
    return target.pointerAccess == AccessMode::ReadOnly ||
           value.pointerAccess == AccessMode::Mutable;
  }
  if (isInteger(target) && isInteger(value)) {
    if (const std::optional<IntegerConstant> constant =
            integerConstant(expression)) {
      return integerFits(target, *constant);
    }
    return integerRangeFits(target, value);
  }
  if (target == value) {
    return true;
  }
  if (target.kind != SemanticType::Expected || target.arguments.size() != 2) {
    return false;
  }
  if (value.kind == SemanticType::Unexpected && value.arguments.size() == 1) {
    const Expr *errorExpression = expression;
    if (const auto *unexpected = exprAs<Unexpected>(expression)) {
      errorExpression = unexpected->error().get();
    }
    return isAssignable(target.arguments[1], value.arguments[0],
                        errorExpression);
  }
  return target.arguments[0] != SemanticType::Void &&
         isAssignable(target.arguments[0], value, expression);
}

auto SemanticVisitor::Impl::isComparable(SemanticType left, SemanticType right,
                                         const Expr *leftExpression,
                                         const Expr *rightExpression) const
    -> bool {
  if (left.kind == SemanticType::TypeParameter ||
      right.kind == SemanticType::TypeParameter) {
    return left == right &&
           constraintImplies(constraintOf(left),
                             GenericConstraintKind::EqualityComparable);
  }
  if (isInteger(left) && isInteger(right)) {
    return numericResult(left, right, leftExpression, rightExpression) !=
           SemanticType::Unknown;
  }
  if ((left == SemanticType::CString && right == SemanticType::NullPtr) ||
      (right == SemanticType::CString && left == SemanticType::NullPtr)) {
    return true;
  }
  if ((left.kind == SemanticType::NativeFunction &&
       right == SemanticType::NullPtr) ||
      (right.kind == SemanticType::NativeFunction &&
       left == SemanticType::NullPtr)) {
    return true;
  }
  return isAssignable(left, right, rightExpression) ||
         isAssignable(right, left, leftExpression);
}

auto SemanticVisitor::Impl::baseTypeOf(
    const TypeRef &type, const std::vector<std::string> &fromScope) const
    -> SemanticType {
  if (type.isNativeFunction()) {
    const std::size_t parameterCount =
        type.nativeFunctionParameterCount.value_or(0);
    if (type.arguments.size() != parameterCount + 1) {
      return SemanticType::Unknown;
    }
    std::vector<SemanticType> parameters;
    parameters.reserve(parameterCount);
    for (std::size_t index = 0; index < parameterCount; ++index) {
      parameters.emplace_back(typeOf(type.arguments[index], fromScope));
    }
    return SemanticType::nativeFunctionType(
        typeOf(type.arguments.back(), fromScope), parameters);
  }
  const CompilerCapabilityTypeKind compilerCapability =
      compilerCapabilityType(type, fromScope);
  if (compilerCapability == CompilerCapabilityTypeKind::TextView) {
    return type.arguments.empty() ? SemanticType::StringView
                                  : SemanticType::Unknown;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::CString) {
    return type.arguments.empty() ? SemanticType::CString
                                  : SemanticType::Unknown;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::UniqueOwner) {
    return type.arguments.size() == 1 ? SemanticType::uniqueOwnerOf(typeOf(
                                            type.arguments[0], fromScope))
                                      : SemanticType::Unknown;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::Storage) {
    return type.arguments.size() == 1
               ? SemanticType::storageOf(typeOf(type.arguments[0], fromScope))
               : SemanticType::Unknown;
  }
  if (compilerCapability == CompilerCapabilityTypeKind::PrefixStorage) {
    return type.arguments.size() == 1 ? SemanticType::prefixStorageOf(typeOf(
                                            type.arguments[0], fromScope))
                                      : SemanticType::Unknown;
  }
  switch (type.name.last().kind) {
  case TokenKind::AUTO:
    return SemanticType::Unknown;
  case TokenKind::VOID:
    return SemanticType::Void;
  case TokenKind::INT:
  case TokenKind::INT32:
    return SemanticType::Int32;
  case TokenKind::INT8:
    return SemanticType::Int8;
  case TokenKind::INT16:
    return SemanticType::Int16;
  case TokenKind::INT64:
    return SemanticType::Int64;
  case TokenKind::UINT:
  case TokenKind::UINT32:
    return SemanticType::UInt32;
  case TokenKind::UINT8:
    return SemanticType::UInt8;
  case TokenKind::UINT16:
    return SemanticType::UInt16;
  case TokenKind::UINT64:
    return SemanticType::UInt64;
  case TokenKind::FLOAT:
    return SemanticType::Float;
  case TokenKind::DOUBLE:
    return SemanticType::Double;
  case TokenKind::BOOL:
    return SemanticType::Bool;
  case TokenKind::CHAR:
    return SemanticType::Char;
  case TokenKind::NULLPTR_TYPE:
    return SemanticType::NullPtr;
  case TokenKind::EXPECTED: {
    std::vector<SemanticType> arguments;
    arguments.reserve(type.arguments.size());
    for (const TypeRef &argument : type.arguments) {
      arguments.emplace_back(typeOf(argument, fromScope));
    }
    return SemanticType(SemanticType::Expected, std::move(arguments));
  }
  default:
    if (const std::optional<SemanticType> parameter =
            resolveTypeParameter(type.name)) {
      return type.arguments.empty() ? *parameter : SemanticType::Unknown;
    }
    if (const std::optional<ResolvedTypeAlias> alias =
            resolveTypeAliasPath(type.name, fromScope)) {
      if (!type.arguments.empty() || alias->id == 0 ||
          alias->id > typeAliases.size()) {
        return SemanticType::Unknown;
      }
      const RegisteredTypeAlias &declaration = typeAliases[alias->id - 1];
      if (declaration.resolution != TypeAliasResolution::Resolved) {
        return SemanticType::Unknown;
      }
      return substituteType(
          alias->type,
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
    }
    if (const std::optional<EnumId> id =
            resolveEnumPath(type.name, fromScope)) {
      return type.arguments.empty() ? SemanticType::enumType(*id)
                                    : SemanticType::Unknown;
    }
    if (const std::optional<ClassId> id =
            resolveClassPath(type.name, fromScope)) {
      const ClassInfo &owner = classInfo(*id);
      GenericSubstitution substitution;
      const std::size_t count =
          std::min(type.arguments.size(), owner.genericParameters.size());
      for (std::size_t index = 0; index < count; ++index) {
        const GenericParameterInfo &parameter = owner.genericParameters[index];
        const TypeRef &argument = type.arguments[index];
        if (parameter.value) {
          if (const std::optional<CompileTimeValue> value =
                  genericValueArgument(argument)) {
            substitution.values.emplace(parameter.id, *value);
          }
        } else {
          substitution.types.emplace(parameter.id, typeOf(argument, fromScope));
        }
      }
      for (const GenericParameterInfo &parameter : owner.genericParameters) {
        if (parameter.value) {
          if (substitution.values.contains(parameter.id) ||
              !parameter.defaultValue) {
            continue;
          }
          const GenericValueEvaluation evaluation =
              evaluateGenericValueExpression(*parameter.defaultValue,
                                             substitution);
          if (evaluation.value) {
            substitution.values.emplace(parameter.id, *evaluation.value);
          }
          continue;
        }
        if (substitution.types.contains(parameter.id) ||
            !parameter.defaultType) {
          continue;
        }
        substitution.types.emplace(
            parameter.id, substituteType(*parameter.defaultType, substitution));
      }
      std::vector<SemanticType> arguments;
      std::vector<CompileTimeValue> valueArguments;
      arguments.reserve(genericTypeParameterCount(owner.genericParameters));
      valueArguments.reserve(
          genericValueParameterCount(owner.genericParameters));
      for (const GenericParameterInfo &parameter : owner.genericParameters) {
        if (parameter.value) {
          const auto found = substitution.values.find(parameter.id);
          if (found == substitution.values.end()) {
            return SemanticType::Unknown;
          }
          valueArguments.emplace_back(found->second);
        } else {
          const auto found = substitution.types.find(parameter.id);
          if (found == substitution.types.end() ||
              found->second == SemanticType::Unknown) {
            return SemanticType::Unknown;
          }
          arguments.emplace_back(found->second);
        }
      }
      return selectExactClassSpecialization(SemanticType::classType(
          *id, std::move(arguments), std::move(valueArguments)));
    }
    return SemanticType::Unknown;
  }
}

auto SemanticVisitor::Impl::typeOf(
    const TypeRef &type, const std::vector<std::string> &fromScope) const
    -> SemanticType {
  SemanticType result = baseTypeOf(type, fromScope);
  if (type.pointer) {
    result = SemanticType::rawPointerTo(
        std::move(result),
        type.pointeeConst ? AccessMode::ReadOnly : AccessMode::Mutable);
  }
  if (type.outerPointer) {
    result = SemanticType::rawPointerTo(std::move(result));
  }
  for (auto extent = type.arrayExtents.rbegin();
       extent != type.arrayExtents.rend(); ++extent) {
    const std::optional<CompileTimeValue> length = resolvedArrayExtent(*extent);
    if (!length || length->kind == CompileTimeValue::Unknown) {
      return SemanticType::Unknown;
    }
    result = SemanticType::arrayOf(std::move(result), *length);
  }
  if (type.reference) {
    result = SemanticType::referenceTo(std::move(result));
  }
  return selectExactClassSpecialization(std::move(result));
}

auto SemanticVisitor::Impl::typeOf(const TypeRef &type) const -> SemanticType {
  return typeOf(type, currentNamespace);
}

auto SemanticVisitor::Impl::typeOf(
    const TypeRef &type, Mutability mutability,
    const std::vector<std::string> &fromScope) const -> SemanticType {
  SemanticType result = typeOf(type, fromScope);
  if (result.kind == SemanticType::Reference) {
    result.referenceAccess = mutability == Mutability::Mutable
                                 ? AccessMode::Mutable
                                 : AccessMode::ReadOnly;
  }
  return result;
}

auto SemanticVisitor::Impl::typeOf(const TypeRef &type,
                                   Mutability mutability) const
    -> SemanticType {
  return typeOf(type, mutability, currentNamespace);
}

auto SemanticVisitor::Impl::typeOf(
    const Parameter &parameter, const std::vector<std::string> &fromScope) const
    -> SemanticType {
  return typeOf(parameter.type, parameter.mutability, fromScope);
}

auto SemanticVisitor::Impl::typeOf(const Parameter &parameter) const
    -> SemanticType {
  return typeOf(parameter, currentNamespace);
}

auto SemanticVisitor::Impl::literalType(const Literal &literal)
    -> SemanticType {
  if (const auto *value = std::get_if<std::uint64_t>(&literal)) {
    if (*value <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
      return SemanticType::Int32;
    }
    if (*value <=
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return SemanticType::Int64;
    }
    return SemanticType::UInt64;
  }
  if (const auto *floating = std::get_if<BinaryFloat>(&literal)) {
    return floating->format == BinaryFloatFormat::Binary32
               ? SemanticType::Float
               : SemanticType::Double;
  }
  if (std::holds_alternative<CharacterLiteral>(literal)) {
    return SemanticType::Char;
  }
  if (std::holds_alternative<std::string>(literal)) {
    return SemanticType::StringView;
  }
  if (std::holds_alternative<bool>(literal)) {
    return SemanticType::Bool;
  }
  if (std::holds_alternative<std::nullptr_t>(literal)) {
    return SemanticType::NullPtr;
  }
  return SemanticType::Unknown;
}

auto SemanticVisitor::Impl::expressionToken(const Expr &expression) -> Token {
  switch (expression.exprKind()) {
  case ExprKind::Assign:
    return static_cast<const Assign &>(expression).oper();
  case ExprKind::ArrayInitializer:
    return static_cast<const ArrayInitializer &>(expression).brace();
  case ExprKind::Binary:
    return static_cast<const Binary &>(expression).oper();
  case ExprKind::Call:
    return static_cast<const Call &>(expression).paren();
  case ExprKind::ConditionalExpr:
    return static_cast<const ConditionalExpr &>(expression).question();
  case ExprKind::Conversion:
    return static_cast<const Conversion &>(expression).targetType().name.last();
  case ExprKind::DirectInitializer:
    return static_cast<const DirectInitializer &>(expression).brace();
  case ExprKind::DereferenceSet:
    return static_cast<const DereferenceSet &>(expression).dereference();
  case ExprKind::Get:
    return static_cast<const Get &>(expression).name();
  case ExprKind::Grouping:
    return expressionToken(
        static_cast<const Grouping &>(expression).expression());
  case ExprKind::Index:
    return static_cast<const Index &>(expression).bracket();
  case ExprKind::IndexSet:
    return static_cast<const IndexSet &>(expression).bracket();
  case ExprKind::Lambda:
    return static_cast<const Lambda &>(expression).bracket();
  case ExprKind::LayoutQuery:
    return static_cast<const LayoutQuery &>(expression).keyword();
  case ExprKind::LiteralExpr:
    return static_cast<const LiteralExpr &>(expression).token();
  case ExprKind::Logical:
    return static_cast<const Logical &>(expression).oper();
  case ExprKind::PackFold:
    return static_cast<const PackFold &>(expression).ellipsis();
  case ExprKind::PackExpansion:
    return static_cast<const PackExpansion &>(expression).ellipsis();
  case ExprKind::Postfix:
    return static_cast<const Postfix &>(expression).oper();
  case ExprKind::QualifiedName:
    return static_cast<const QualifiedName &>(expression).name().last();
  case ExprKind::This:
    return static_cast<const This &>(expression).keyword();
  case ExprKind::Set:
    return static_cast<const Set &>(expression).name();
  case ExprKind::Unary:
    return static_cast<const Unary &>(expression).oper();
  case ExprKind::Unexpected:
    return static_cast<const Unexpected &>(expression).keyword();
  case ExprKind::Variable:
    return static_cast<const Variable &>(expression).name();
  }
  return Token{};
}

auto SemanticVisitor::Impl::expressionToken(const ExprPtr &expr) -> Token {
  return expr ? expressionToken(*expr) : Token{};
}

auto SemanticVisitor::Impl::callableToken(const ExprPtr &callee) -> Token {
  if (callee && callee->exprKind() == ExprKind::Variable) {
    const auto *variable = static_cast<const Variable *>(callee.get());
    return variable->name();
  }
  if (callee && callee->exprKind() == ExprKind::QualifiedName) {
    const auto *qualified = static_cast<const QualifiedName *>(callee.get());
    return qualified->name().last();
  }
  if (callee && callee->exprKind() == ExprKind::Get) {
    const auto *member = static_cast<const Get *>(callee.get());
    return member->name();
  }
  return expressionToken(callee);
}

} // namespace lang
