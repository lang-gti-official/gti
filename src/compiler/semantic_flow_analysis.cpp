#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::canDataInitializeProgramConstant(
    const ConstantValue &) -> bool {
  return true;
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
      !hasRole(roles, OccurrenceRole::Write);
  const bool substitution = constant && valueSubstitutionCandidate &&
                            !isThreadLocalStorage(symbol) &&
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
    ResolvedCallArguments arguments) -> void {
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

auto SemanticVisitor::Impl::recordConcreteProgramEffect(
    ProgramEffectOwner target, const SemanticType &owner,
    const std::vector<SemanticType> &types,
    const std::vector<CompileTimeValue> &values, const SourceSpan &site)
    -> void {
  if (!currentProgramEffectOwner.valid())
    return;
  if (hasOpenProgramCleanupShape(owner) ||
      std::any_of(
          types.begin(), types.end(),
          [&](const auto &type) { return hasOpenProgramCleanupShape(type); }) ||
      std::any_of(values.begin(), values.end(), [](const auto &value) {
        return value.kind != CompileTimeValue::UInt64;
      })) {
    addUnknownProgramEffect(site,
                            "a GTI call has unresolved generic arguments");
    return;
  }
  if (!requestSession.programConcreteEffects)
    requestSession.programConcreteEffects =
        std::make_shared<ProgramConcreteEffectState>();
  auto &state = *requestSession.programConcreteEffects;
  auto &queries = state.queries;
  std::size_t hash = ProgramEffectOwnerHash{}(target);
  const auto add = [&](std::size_t value) {
    hash ^= value + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
  };
  const auto hashType = [&](const auto &self,
                            const SemanticType &type) -> void {
    add(type.kind);
    add(type.classId);
    add(type.enumId);
    add(type.genericParameterId);
    add(type.functionId);
    add(type.arrayLength);
    add(type.arrayLengthParameterId);
    add(type.arguments.size());
    for (const auto &argument : type.arguments)
      self(self, argument);
    add(type.valueArguments.size());
    for (const auto &value : type.valueArguments)
      add(value.value);
  };
  hashType(hashType, owner);
  for (const auto &type : types)
    hashType(hashType, type);
  for (const auto &value : values)
    add(value.value);
  auto &bucket = state.buckets[hash];
  const auto existing =
      std::find_if(bucket.begin(), bucket.end(), [&](std::size_t index) {
        const auto &query = queries[index];
        return query.target == target && query.owner == owner &&
               query.types == types && query.values == values;
      });
  const std::size_t index =
      existing == bucket.end() ? queries.size() : *existing;
  if (existing == bucket.end()) {
    queries.push_back({.target = target,
                       .owner = owner,
                       .types = types,
                       .values = values,
                       .site = site});
    bucket.push_back(index);
  }
  addProgramEffectEdge(
      {.kind = ProgramEffectOwnerKind::ConcreteInstance, .id = index + 1},
      site);
}

auto SemanticVisitor::Impl::closeConcreteProgramEffects(
    const ProgramInitializationPlan &plan) -> void {
  const auto state = requestSession.programConcreteEffects;
  if (!state)
    return;
  std::unique_ptr<Impl> worker;
  std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visited;
  const auto visit = [&](const auto &self, ProgramEffectOwner owner,
                         std::size_t depth) -> void {
    if (!visited.insert(owner).second)
      return;
    if (owner.kind == ProgramEffectOwnerKind::ConcreteInstance &&
        owner.id != 0 && owner.id <= state->queries.size()) {
      const auto query = state->queries[owner.id - 1];
      if (!query.complete) {
        if (depth >= 64 || state->analyzed >= 16384) {
          diagnostics.push_back(makeDiagnostic(
              DiagnosticId::GenericInstantiationLimit, query.site,
              {depth >= 64 ? "initializer-effect-depth"
                           : "initializer-effect-instance",
               depth >= 64 ? "64" : "16384"}));
          programEffects[owner].unknown.push_back(
              {.span = query.site,
               .reason = "concrete effect resource exhaustion"});
          return;
        }
        ++state->analyzed;
        if (!worker) {
          worker = std::make_unique<Impl>(target, sourceGraph, false,
                                          workMetrics, sources, rawPointerAudit,
                                          requireFunctionDefinitions);
          worker->catalog = catalog.snapshotForInstanceAnalysis();
          worker->semanticModel = semanticModel;
          worker->valueFlow = valueFlow;
          // Initializer effects close over the completed program, so every
          // analysed constexpr definition is available to the concrete
          // instances they reach, exactly as to their HIR reanalysis.
          worker->constexprDefinitionsAnalyzed = constexprDefinitionsAnalyzed;
          worker->constexprConstructorsAnalyzed = constexprConstructorsAnalyzed;
          worker->genericDefaultConstants = genericDefaultConstants;
          worker->defaultArgumentSyntax = defaultArgumentSyntax;
          worker->defaultArgumentDependencies = defaultArgumentDependencies;
          worker->nextLambdaId = nextLambdaId;
          worker->requestSession.constexprWork = requestSession.constexprWork;
          worker->requestSession.programConcreteEffects = state;
        }
        worker->programEffects.clear();
        worker->requestSession.programStorageBorrowChecks.clear();
        SemanticInstanceAnalysis analysis;
        switch (query.target.kind) {
        case ProgramEffectOwnerKind::Function:
          analysis = worker->analyzeFunctionInstance(
              query.target.id, query.owner.arguments,
              query.owner.valueArguments, query.types, query.values);
          break;
        case ProgramEffectOwnerKind::Constructor:
          analysis = worker->analyzeConstructorInstance(
              query.target.id, query.owner.arguments,
              query.owner.valueArguments, query.types, query.values);
          break;
        case ProgramEffectOwnerKind::Destructor:
          analysis = worker->analyzeDestructorInstance(
              query.target.id, query.owner.arguments,
              query.owner.valueArguments);
          break;
        case ProgramEffectOwnerKind::FieldInitializers:
          analysis = worker->analyzeClassFieldInitializers(
              query.target.id, query.owner.arguments,
              query.owner.valueArguments);
          break;
        default:
          programEffects[owner].unknown.push_back(
              {.span = query.site,
               .reason = "unsupported concrete effect owner"});
          return;
        }
        if (!analysis.valid()) {
          for (auto &diagnostic : analysis.diagnostics) {
            diagnostic.related.push_back(
                {query.site, "Initializer instance requested here."});
            diagnostics.push_back(std::move(diagnostic));
          }
          programEffects[owner].unknown.push_back(
              {.span = query.site,
               .reason = "a concrete GTI body is not valid"});
        } else {
          requestSession.programStorageBorrowChecks.insert(
              requestSession.programStorageBorrowChecks.end(),
              worker->requestSession.programStorageBorrowChecks.begin(),
              worker->requestSession.programStorageBorrowChecks.end());
          // Local callable/initializer owners share this selected body. Flatten
          // those exact records, never mix in the primary generic's open arms.
          std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> local;
          std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash>
              cleanupLocal;
          const auto collect = [&](const auto &collectSelf,
                                   ProgramEffectOwner source,
                                   bool cleanup = false) -> void {
            auto &visited = cleanup ? cleanupLocal : local;
            if (!visited.insert(source).second)
              return;
            const auto found = worker->programEffects.find(source);
            if (found == worker->programEffects.end()) {
              programEffects[owner].edges.push_back(
                  {.target = source, .span = query.site, .cleanup = cleanup});
              return;
            }
            const auto &summary = found->second;
            auto &destination = programEffects[owner];
            destination.directFailure |= !cleanup && summary.directFailure;
            destination.uses.insert(destination.uses.end(),
                                    summary.uses.begin(), summary.uses.end());
            for (auto use : summary.borrowUses) {
              use.cleanup |= cleanup;
              destination.borrowUses.push_back(std::move(use));
            }
            for (auto unknown : summary.unknown) {
              unknown.cleanup |= cleanup;
              destination.unknown.push_back(std::move(unknown));
            }
            for (const auto &edge : summary.edges) {
              if (edge.target.kind ==
                      ProgramEffectOwnerKind::ConcreteInstance ||
                  edge.target.kind == ProgramEffectOwnerKind::Initializer)
              {
                auto retained = edge;
                retained.cleanup |= cleanup;
                destination.edges.push_back(std::move(retained));
              }
              else
                collectSelf(collectSelf, edge.target, cleanup || edge.cleanup);
            }
          };
          // An empty selected body has no effects; an absent local record is
          // empty at the root, but an absent referenced owner is a primary
          // edge.
          if (worker->programEffects.contains(query.target))
            collect(collect, query.target);
        }
        state->queries[owner.id - 1].complete = true;
        ++depth;
      }
    }
    const auto found = programEffects.find(owner);
    if (found != programEffects.end()) {
      const auto edges = found->second.edges;
      for (const auto &edge : edges)
        self(self, edge.target, depth);
    }
  };
  for (const auto &step : plan.steps) {
    if (step.role != ProgramInitializationStepRole::Initializer)
      continue;
    const auto *binding = step.declaration
                              ? semanticModel.findBinding(*step.declaration)
                              : nullptr;
    // A failed constant has no runtime initializer to close. Keep its owning
    // constant/discovery diagnostics instead of querying the rejected instance
    // again and adding an unrelated initializer-effect budget cascade.
    if (step.declaration && step.declaration->isConstexpr() &&
        (!binding || !binding->constant))
      continue;
    visit(visit,
          {.kind = ProgramEffectOwnerKind::Initializer, .id = step.symbol}, 0);
  }
  for (std::size_t index = 0;
       index < requestSession.programStorageBorrowChecks.size(); ++index) {
    const auto targets =
        requestSession.programStorageBorrowChecks[index].targets;
    for (const auto target : targets)
      visit(visit, target, 0);
  }
}

auto SemanticVisitor::Impl::programConstructor(ConstructorId id) const
    -> const ConstructorInfo * {
  if (id == 0) {
    return nullptr;
  }
  for (const ClassInfo &owner : classes) {
    if (owner.designatedConstructor && owner.designatedConstructor->id == id) {
      return &*owner.designatedConstructor;
    }
    if (owner.reversedDesignatedConstructor &&
        owner.reversedDesignatedConstructor->id == id) {
      return &*owner.reversedDesignatedConstructor;
    }
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
  const ProgramEffectOwner fields{
      .kind = ProgramEffectOwnerKind::FieldInitializers, .id = owner.id};
  if (!owner.genericParameters.empty())
    recordConcreteProgramEffect(fields, type, {}, {}, span);
  else
    addProgramEffectEdge(fields, span);

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
      recordProgramSelectedConstructorEffect(declared, type, span);
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
    const SourceSpan &span, const std::vector<SemanticType> &types,
    const std::vector<CompileTimeValue> &values) -> void {
  if (constructor == nullptr || constructor->declaration == nullptr) {
    addUnknownProgramEffect(
        span, "a selected GTI constructor has no exact declaration");
    return;
  }
  if (constructor->declaration->body() != nullptr) {
    const ProgramEffectOwner effect{.kind = ProgramEffectOwnerKind::Constructor,
                                    .id = constructor->id};
    if (!classInfo(constructedType.classId).genericParameters.empty() ||
        !constructor->genericParameters.empty())
      recordConcreteProgramEffect(effect, constructedType, types, values, span);
    else
      addProgramEffectEdge(effect, span);
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
                    return value.kind != CompileTimeValue::UInt64;
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
  if (std::find(requestSession.invalidOrdinaryLayouts.begin(),
                requestSession.invalidOrdinaryLayouts.end(),
                type) != requestSession.invalidOrdinaryLayouts.end())
    return;
  if (!currentProgramEffectOwner.valid() || !requiresActiveCleanupFor(type)) {
    return;
  }
  if (visiting.size() >= 64) {
    diagnostics.push_back(
        makeDiagnostic(DiagnosticId::GenericInstantiationLimit, span,
                       {"cleanup-lifecycle-depth", "64"}));
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
  case SemanticType::Enum: {
    const auto enumeration = enumTypeFor(type);
    if (!enumeration) {
      addUnknownProgramEffect(span,
                              "enum cleanup has no exact payload inventory");
      finish();
      return;
    }
    for (const auto &variant : enumeration->enumerators)
      for (const auto &payload : variant.payloadTypes)
        recordProgramCleanupEffects(payload, span, visiting);
    finish();
    return;
  }
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
    const ProgramEffectOwner effect{.kind = ProgramEffectOwnerKind::Destructor,
                                    .id = owner.id};
    if (!owner.genericParameters.empty())
      recordConcreteProgramEffect(effect, type, {}, {}, span);
    else
      addProgramEffectEdge(effect, span);
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
  if (!currentProgramEffectOwner.valid())
    return;
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  std::vector<SemanticType> visiting;
  recordProgramCleanupEffects(type, span, visiting, includeDirectDestructor);
  auto &summary = programEffects[owner];
  for (std::size_t index = edgeStart; index < summary.edges.size(); ++index)
    summary.edges[index].cleanup = true;
  for (std::size_t index = unknownStart; index < summary.unknown.size();
       ++index)
    summary.unknown[index].cleanup = true;
}

auto SemanticVisitor::Impl::recordProgramConstructionEffects(
    const Expr &site, const ResolvedConstructionInfo &construction,
    std::size_t suppliedArguments) -> bool {
  return recordProgramConstructionEffects(tokenSpan(expressionToken(site)),
                                          construction, suppliedArguments);
}

auto SemanticVisitor::Impl::recordProgramConstructionEffects(
    const SourceSpan &span, const ResolvedConstructionInfo &construction,
    std::size_t suppliedArguments) -> bool {
  recordProgramConstructorDefaultArgumentEffects(
      programConstructor(construction.constructor), suppliedArguments, span);
  if (construction.constructedType.kind != SemanticType::Class ||
      construction.constructedType.classId == 0) {
    return false;
  }
  if (hasOpenProgramCleanupShape(construction.constructedType)) {
    addUnknownProgramEffect(span, "a generic construction has no closed "
                                  "program-storage effect summary");
    return true;
  }
  if (construction.generatedDefault || construction.constructor == 0) {
    recordProgramDefaultConstructionEffects(construction.constructedType, span);
  } else {
    const ProgramEffectOwner fields{
        .kind = ProgramEffectOwnerKind::FieldInitializers,
        .id = construction.constructedType.classId};
    if (!classInfo(construction.constructedType.classId)
             .genericParameters.empty())
      recordConcreteProgramEffect(fields, construction.constructedType, {}, {},
                                  span);
    else
      addProgramEffectEdge(fields, span);
    recordProgramSelectedConstructorEffect(
        programConstructor(construction.constructor),
        construction.constructedType, span, construction.typeArguments,
        construction.valueArguments);
  }
  return false;
}

auto SemanticVisitor::Impl::recordProgramOperatorTargetEffects(
    const Expr &expression, const ResolvedOperatorInfo &operation) -> void {
  if (!currentProgramEffectOwner.valid()) {
    return;
  }
  const FunctionInfo *target =
      operation.function == 0 ? nullptr
                              : semanticModel.findFunction(operation.function);
  if (target != nullptr && target->linkage != LanguageLinkage::Gti) {
    return;
  }
  if (operation.dispatch != CallDispatch::Static || target == nullptr ||
      !target->genericParameters.empty()) {
    addUnknownProgramEffect(
        expression,
        "an overloaded operation has no closed static GTI target set");
  } else if (target->declaration != nullptr &&
             target->declaration->runtimeBinding()) {
    // Trusted runtime bindings cannot execute a hidden GTI body. Their
    // explicit receiver/argument storage effects are the caller's.
    return;
  } else if (target->declaration == nullptr ||
             target->bodyDeclaration()->body() == nullptr) {
    addUnknownProgramEffect(expression,
                            "a selected GTI operation has no executable body");
  } else {
    const ProgramEffectOwner effect{.kind = ProgramEffectOwnerKind::Function,
                                    .id = target->id};
    if (target->ownerClass != 0 &&
        !classInfo(target->ownerClass).genericParameters.empty())
      recordConcreteProgramEffect(effect, operation.dispatchOwner, {}, {},
                                  tokenSpan(expressionToken(expression)));
    else
      addProgramEffectEdge(effect, expression);
  }
}

auto SemanticVisitor::Impl::appendProgramStorageBorrowProtection(
    ProgramStorageBorrowCheck &check, const SemanticLoanPlace &place,
    AccessMode access, const SourceSpan &origin, SemanticLoanId loan) -> void {
  if (!isPersistentStorageSymbol(place.root))
    return;
  const auto found =
      std::find_if(check.protections.begin(), check.protections.end(),
                   [&](const auto &entry) {
                     return entry.place == place && entry.loan == loan;
                   });
  if (found == check.protections.end())
    check.protections.push_back({place, access, origin, loan});
  else if (access == AccessMode::Mutable)
    found->access = access;
}

auto SemanticVisitor::Impl::captureProgramStorageBorrows(const SourceSpan &site)
    -> ProgramStorageBorrowCheck {
  ProgramStorageBorrowCheck check{.site = site};
  for (const auto &[id, flow] : loanFlow.records) {
    if (!flow.active)
      continue;
    const auto *loan = semanticModel.findLoan(id);
    if (loan == nullptr)
      continue;
    const SourceSpan origin =
        loan->origin ? tokenSpan(expressionToken(*loan->origin)) : check.site;
    appendProgramStorageBorrowProtection(check, loan->place, loan->access,
                                         origin, id);
    for (const auto &dependency : loan->additionalDependencies)
      appendProgramStorageBorrowProtection(check, dependency.place,
                                           dependency.access, origin, id);
  }
  return check;
}

auto SemanticVisitor::Impl::appendProgramStorageBorrowInput(
    ProgramStorageBorrowCheck &check, const ExprPtr &input,
    std::optional<AccessMode> directAccess) -> void {
  if (!input)
    return;
  const SourceSpan span = tokenSpan(expressionToken(*input));
  const auto append = [&](const SemanticLoanPlace &place, AccessMode access) {
    appendProgramStorageBorrowProtection(check, place, access, span);
    if (currentProgramEffectOwner.valid() &&
        isPersistentStorageSymbol(place.root))
      programEffects[currentProgramEffectOwner].borrowUses.push_back(
          {.place = {.root = place.root, .projections = place.projections},
           .access = access,
           .span = span});
  };
  if (directAccess)
    for (const auto &origin : loanPlaces(input, true))
      append(origin, *directAccess);
  const auto *info = semanticModel.findExpression(*input);
  if (info == nullptr || !info->traits.containsBorrowedState)
    return;
  // Complete dependency sets cover source classes, enums, closures and checked
  // erased references uniformly, including several external origins.
  for (const auto &dependency : storedBorrowDependencies(input))
    append(dependency.place, dependency.access);
}

auto SemanticVisitor::Impl::recordProgramCallEffects(const Expr &expression)
    -> void {
  if (!currentProgramEffectOwner.valid())
    return;
  SymbolId threadStorage = semanticModel.findResolvedSymbol(expression);
  if (!isThreadLocalStorage(threadStorage)) {
    const auto *place = semanticModel.findPlace(expression);
    threadStorage =
        place && isThreadLocalStorage(place->root) ? place->root : 0;
  }
  if (threadStorage != 0 &&
      !threadLocalEnsureFailure(threadStorage, expressionToken(expression))
           .empty()) {
    auto initialization =
        captureProgramStorageBorrows(tokenSpan(expressionToken(expression)));
    const auto owner = currentProgramEffectOwner;
    const auto edges = programEffects[owner].edges.size();
    const auto unknown = programEffects[owner].unknown.size();
    addProgramEffectEdge(
        {.kind = ProgramEffectOwnerKind::Initializer, .id = threadStorage},
        expression);
    publishProgramStorageBorrowCheck(std::move(initialization), owner, edges,
                                     unknown);
  }
  if (exprAs<Call>(&expression) == nullptr &&
      exprAs<PackFold>(&expression) == nullptr &&
      exprAs<ArrayInitializer>(&expression) == nullptr &&
      semanticModel.findConstruction(expression) == nullptr &&
      semanticModel.findOperator(expression) == nullptr)
    return;
  auto check =
      captureProgramStorageBorrows(tokenSpan(expressionToken(expression)));
  const SourceSpan callSite = check.site;
  const auto appendArgument = [&](const ExprPtr &argument,
                                  const SemanticType &parameter) {
    appendProgramStorageBorrowInput(
        check, argument,
        parameter.kind == SemanticType::Reference
            ? std::optional(parameter.referenceAccess)
            : std::nullopt);
    if (parameter.kind != SemanticType::Reference)
      consumeProgramTemporary(argument);
  };
  const auto appendArguments = [&](const auto &arguments,
                                   const auto &parameters,
                                   const ProgramStorageBorrowCheck *receiver =
                                       nullptr) {
    ProgramStorageBorrowCheck earlier{.site = callSite};
    if (receiver)
      earlier.protections = receiver->protections;
    for (std::size_t index = 0;
         index < std::min(arguments.size(), parameters.size()); ++index) {
      recordProgramOrderedInputEffects(earlier, arguments[index]);
      appendArgument(arguments[index], parameters[index]);
      appendProgramStorageBorrowInput(
          earlier, arguments[index],
          parameters[index].kind == SemanticType::Reference
              ? std::optional(parameters[index].referenceAccess)
              : std::nullopt);
    }
  };
  const auto appendReceiver = [&](const ExprPtr &receiver,
                                  CallReceiverMode mode) {
    ProgramStorageBorrowCheck receiverBorrow{.site = callSite};
    if (!receiver || mode == CallReceiverMode::None)
      return receiverBorrow;
    const AccessMode access =
        mode == CallReceiverMode::MutablePlace ||
                mode == CallReceiverMode::FreshTemporaryMutable ||
                mode == CallReceiverMode::Consuming
            ? AccessMode::Mutable
            : AccessMode::ReadOnly;
    appendProgramStorageBorrowInput(check, receiver, access);
    appendProgramStorageBorrowInput(receiverBorrow, receiver, access);
    if (mode == CallReceiverMode::Consuming)
      consumeProgramTemporary(receiver);
    return receiverBorrow;
  };
  // Reference inputs are call-scoped loans even without a named binding.
  // Apply the same rule to ordinary calls, constructors, overloaded operators
  // and callable environments rather than granting one surface an exception.
  if (const auto *construction = semanticModel.findConstruction(expression)) {
    if (const auto *call = exprAs<Call>(&expression))
      appendArguments(call->arguments(), construction->parameterTypes);
    else if (const auto *initializer = exprAs<DirectInitializer>(&expression))
      appendArguments(initializer->arguments(), construction->parameterTypes);
    if (construction->kind == ConstructorKind::Move ||
        construction->directInitialization) {
      if (const auto *call = exprAs<Call>(&expression))
        for (const auto &argument : call->arguments())
          consumeProgramTemporary(argument);
      else if (const auto *initializer = exprAs<DirectInitializer>(&expression))
        for (const auto &argument : initializer->arguments())
          consumeProgramTemporary(argument);
    }
  }
  if (const auto *operation = semanticModel.findOperator(expression)) {
    if (const auto *receiver =
            explicitConversionReceiver(expression, *operation))
      appendReceiver(*receiver, operation->receiverMode);
    else if (const auto *call = exprAs<Call>(&expression)) {
      const auto receiver = appendReceiver(call->callee(), operation->receiverMode);
      appendArguments(call->arguments(), operation->parameterTypes, &receiver);
    } else if (const auto *binary = exprAs<Binary>(&expression)) {
      const auto receiver = appendReceiver(binary->left(), operation->receiverMode);
      if (!operation->parameterTypes.empty()) {
        recordProgramOrderedInputEffects(receiver, binary->right());
        appendArgument(binary->right(), operation->parameterTypes.front());
      }
    } else if (const auto *index = exprAs<Index>(&expression)) {
      const auto receiver = appendReceiver(index->object(), operation->receiverMode);
      if (!operation->parameterTypes.empty()) {
        recordProgramOrderedInputEffects(receiver, index->index());
        appendArgument(index->index(), operation->parameterTypes.front());
      }
    } else if (const auto *index = exprAs<IndexSet>(&expression)) {
      const auto receiver = appendReceiver(index->object(), operation->receiverMode);
      if (!operation->parameterTypes.empty()) {
        recordProgramOrderedInputEffects(receiver, index->index());
        appendArgument(index->index(), operation->parameterTypes.front());
      }
    } else if (const auto *set = exprAs<DereferenceSet>(&expression)) {
      appendReceiver(set->object(), operation->receiverMode);
    } else if (const auto *unary = exprAs<Unary>(&expression))
      appendReceiver(unary->right(), operation->receiverMode);
    else if (const auto *postfix = exprAs<Postfix>(&expression))
      appendReceiver(postfix->expression(), operation->receiverMode);
    else if (const auto *member = exprAs<Get>(&expression))
      appendReceiver(member->object(), operation->receiverMode);
  }
  if (const auto *call = exprAs<Call>(&expression)) {
    if (const auto *payload = semanticModel.findPayloadConstruction(*call))
      appendArguments(call->arguments(), payload->parameterTypes);
    const auto *resolved = semanticModel.findCall(*call);
    if (resolved != nullptr) {
      ProgramStorageBorrowCheck receiver{.site = callSite};
      appendProgramStorageBorrowInput(receiver, call->callee());
      appendProgramStorageBorrowInput(check, call->callee());
      if (resolved->receiverMode != CallReceiverMode::None)
        if (const auto *member = exprAs<Get>(call->callee().get())) {
          auto explicitReceiver =
              appendReceiver(member->object(), resolved->receiverMode);
          receiver.protections.insert(receiver.protections.end(),
                                       explicitReceiver.protections.begin(),
                                       explicitReceiver.protections.end());
        }
      appendArguments(ResolvedCallArguments(*call, resolved),
                      resolved->parameterTypes, &receiver);
    }
    ProgramStorageBorrowCheck callable{.site = callSite};
    appendProgramStorageBorrowInput(callable, call->callee());
    if (const auto *lambda = semanticModel.findLambdaCall(*call))
      appendArguments(call->arguments(), lambda->parameterTypes, &callable);
    if (const auto *deferred = semanticModel.findDeferredCallableCall(*call))
      appendArguments(call->arguments(), deferred->parameterTypes, &callable);
    if (resolvedIntrinsicKind(*call) == IntrinsicKind::Move)
      for (const auto &argument : call->arguments())
        consumeProgramTemporary(argument);
    appendProgramStorageBorrowInput(check, call->callee());
  }
  if (const auto *initializer = exprAs<ArrayInitializer>(&expression)) {
    if (const auto *collection = semanticModel.findCollection(*initializer)) {
      if (const auto *insert = semanticModel.findFunction(collection->insert);
          insert != nullptr && !insert->parameterTypes.empty() &&
          insert->parameterTypes.back().kind != SemanticType::Reference)
        for (const auto &element : initializer->elements())
          consumeProgramTemporary(element);
    } else {
      for (const auto &element : initializer->elements())
        consumeProgramTemporary(element);
    }
  }
  const ProgramEffectOwner owner = currentProgramEffectOwner;
  const std::size_t edges = programEffects[owner].edges.size();
  const std::size_t unknown = programEffects[owner].unknown.size();
  recordProgramCallEffectTargets(expression);
  publishProgramStorageBorrowCheck(std::move(check), owner, edges, unknown);
}

auto SemanticVisitor::Impl::publishProgramStorageBorrowCheck(
    ProgramStorageBorrowCheck check, ProgramEffectOwner owner,
    std::size_t edges, std::size_t unknown) -> void {
  if (check.protections.empty())
    return;
  const auto &summary = programEffects[owner];
  for (std::size_t index = edges; index < summary.edges.size(); ++index)
    check.targets.push_back(summary.edges[index].target);
  check.unknown.insert(check.unknown.end(),
                       summary.unknown.begin() +
                           static_cast<std::ptrdiff_t>(unknown),
                       summary.unknown.end());
  if (!check.uses.empty() || !check.targets.empty() || !check.unknown.empty()) {
    const std::size_t checkIndex =
        requestSession.programStorageBorrowChecks.size();
    std::vector<SemanticLoanId> namedLoans;
    bool scopeExit = false;
    for (const auto &protection : check.protections)
      if (protection.loan != 0) {
        appendUniqueLoan(namedLoans, protection.loan);
        scopeExit |= protection.scopeExit;
      }
    requestSession.programStorageBorrowChecks.push_back(std::move(check));
    if (scopeExit)
      loanFlow.programScopeExitChecks.push_back(checkIndex);
    if (!namedLoans.empty() && loanFlow.currentStatement != nullptr &&
        loanFlow.currentRegion != 0)
      loanFlow.conflicts.push_back({.loans = std::move(namedLoans),
                                    .region = loanFlow.currentRegion,
                                    .order = loanFlow.currentOrder,
                                    .sequence = loanFlow.nextSequence++,
                                    .statement = loanFlow.currentStatement,
                                    .path = loanFlow.activeConditionalArms,
                                    .switchPath = loanFlow.activeSwitchArms,
                                    .loops = loanFlow.activeLoops,
                                    .programStorageCheck = checkIndex});
  }
}

auto SemanticVisitor::Impl::recordProgramOrderedInputEffects(
    const ProgramStorageBorrowCheck &earlier, const ExprPtr &input) -> void {
  if (earlier.protections.empty() || !input)
    return;
  const auto found = requestSession.programExpressionEffects.find(input.get());
  if (found == requestSession.programExpressionEffects.end() ||
      found->second.owner != currentProgramEffectOwner)
    return;
  const auto &slice = found->second;
  const auto &summary = programEffects[slice.owner];
  auto check = earlier;
  check.site = tokenSpan(expressionToken(input));
  check.siteKind = ProgramStorageBorrowSite::OrderedInput;
  check.targets.clear();
  check.unknown.clear();
  check.uses.assign(summary.borrowUses.begin() + slice.useStart,
                    summary.borrowUses.begin() + slice.useEnd);
  for (std::size_t edge = slice.edgeStart; edge < slice.edgeEnd; ++edge)
    if (!summary.edges[edge].cleanup)
      check.targets.push_back(summary.edges[edge].target);
  for (std::size_t unknown = slice.unknownStart; unknown < slice.unknownEnd;
       ++unknown)
    if (!summary.unknown[unknown].cleanup)
      check.unknown.push_back(summary.unknown[unknown]);
  // These are the exact earlier stabilized inputs, not lexical bindings.
  // Their loans end at this invocation and cannot be shortened by NLL.
  publishProgramStorageBorrowCheck(std::move(check), slice.owner,
                                   summary.edges.size(), summary.unknown.size());
}

auto SemanticVisitor::Impl::recordStorageBorrowCleanup(const Symbol &symbol)
    -> void {
  if (!currentProgramEffectOwner.valid() ||
      (symbol.bindingKind != SemanticBindingKind::LocalVariable &&
       symbol.bindingKind != SemanticBindingKind::Parameter) ||
      (symbol.variableDeclaration &&
       symbol.variableDeclaration->isThreadLocal()) ||
      symbol.valueState == ValueState::Moved ||
      !requiresActiveCleanupFor(symbol.type))
    return;
  auto check = captureProgramStorageBorrows(tokenSpan(symbol.declaration));
  check.siteKind = ProgramStorageBorrowSite::ScopeCleanup;
  std::unordered_set<SymbolId> retiringCarriers;
  for (const auto &[_, carrier] : scopes.back())
    retiringCarriers.insert(toolingSymbolFor(carrier));
  for (auto &protection : check.protections) {
    const auto flow = loanFlow.records.find(protection.loan);
    if (flow != loanFlow.records.end() && !flow->second.carriers.empty() &&
        std::all_of(flow->second.carriers.begin(), flow->second.carriers.end(),
                    [&](SymbolId carrier) {
                      return retiringCarriers.contains(carrier);
                    }))
      protection.scopeExit = true;
  }
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  recordProgramExecutedCleanupEffects(symbol.type, check.site);
  publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                   unknownStart);
}

auto SemanticVisitor::Impl::recordProgramScopeBorrowCleanup() -> void {
  if (scopes.empty() || loanFlow.finalized ||
      !currentProgramEffectOwner.valid())
    return;
  std::vector<const Symbol *> order;
  for (const auto &[_, symbol] : scopes.back())
    if (symbol.retainedLoan != 0 || requiresActiveCleanupFor(symbol.type))
      order.push_back(&symbol);
  std::sort(order.begin(), order.end(),
            [](const auto *left, const auto *right) {
              return tokenSpan(left->declaration).start >
                     tokenSpan(right->declaration).start;
            });
  // Simulate carrier retirement in the same reverse declaration order as
  // lexical cleanup. Keep endpoint diagnostics in the real flow and restore
  // only this temporary availability view before ordinary scope processing.
  std::vector<std::pair<SemanticLoanId, LoanFlowRecord>> retired;
  for (const auto *symbol : order) {
    recordStorageBorrowCleanup(*symbol);
    const auto found = loanFlow.records.find(symbol->retainedLoan);
    if (found == loanFlow.records.end())
      continue;
    retired.emplace_back(found->first, found->second);
    found->second.carriers.erase(toolingSymbolFor(*symbol));
    if (found->second.carriers.empty())
      found->second.active = false;
  }
  for (auto it = retired.rbegin(); it != retired.rend(); ++it)
    loanFlow.records[it->first] = std::move(it->second);
}

auto SemanticVisitor::Impl::recordReturnedStorageBorrowCleanup(
    const ReturnStmt &statement) -> void {
  if (!currentProgramEffectOwner.valid() || !statement.value() ||
      (currentReturnType.kind != SemanticType::Reference &&
       !typeTraits(currentReturnType).containsBorrowedState))
    return;
  ProgramStorageBorrowCheck check{.site = tokenSpan(statement.keyword())};
  appendProgramStorageBorrowInput(
      check, statement.value(),
      currentReturnType.kind == SemanticType::Reference
          ? std::optional(currentReturnType.referenceAccess)
          : std::nullopt);
  if (check.protections.empty())
    return;
  const auto owner = currentProgramEffectOwner;
  const auto &summary = programEffects[owner];
  const auto edgeStart = summary.edges.size();
  const auto unknownStart = summary.unknown.size();
  for (const auto &scope : scopes)
    for (const auto &[_, symbol] : scope)
      if ((symbol.bindingKind == SemanticBindingKind::LocalVariable ||
           symbol.bindingKind == SemanticBindingKind::Parameter) &&
          !(symbol.variableDeclaration &&
            symbol.variableDeclaration->isThreadLocal()) &&
          symbol.valueState != ValueState::Moved)
        recordProgramExecutedCleanupEffects(symbol.type,
                                             tokenSpan(symbol.declaration));
  publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                   unknownStart);
}

auto SemanticVisitor::Impl::beginProgramFullExpression(const Expr &root)
    -> std::optional<std::size_t> {
  if (!currentProgramEffectOwner.valid())
    return std::nullopt;
  auto &session = requestSession;
  const auto &expressions = semanticModel.fullExpressions();
  while (session.indexedFullExpressions < expressions.size()) {
    const auto &expression = expressions[session.indexedFullExpressions++];
    for (const auto *value : expression.roots)
      session.fullExpressionRoots.insert_or_assign(value, expression);
  }
  const auto found = session.fullExpressionRoots.find(&root);
  if (found == session.fullExpressionRoots.end() ||
      found->second.constructorInitializer != nullptr)
    return std::nullopt;
  if (!session.fullExpressionStates.empty()) {
    const auto &current = session.fullExpressionStates.back();
    if (current.owner == currentProgramEffectOwner &&
        std::find(current.source.roots.begin(), current.source.roots.end(),
                  &root) != current.source.roots.end())
      return std::nullopt;
  }
  const auto index = session.fullExpressionStates.size();
  session.fullExpressionStates.push_back(
      {.source = found->second, .owner = currentProgramEffectOwner});
  return index;
}

auto SemanticVisitor::Impl::beginProgramFullExpression(
    const ConstructorInitializer &initializer) -> std::size_t {
  const auto index = requestSession.fullExpressionStates.size();
  const auto &sources = semanticModel.fullExpressionsFor(initializer);
  requestSession.fullExpressionStates.push_back(
      {.source = sources.empty() ? SemanticFullExpression{} : sources.back(),
       .owner = currentProgramEffectOwner});
  return index;
}

auto SemanticVisitor::Impl::consumeProgramTemporary(const ExprPtr &value)
    -> void {
  if (!value || requestSession.fullExpressionStates.empty())
    return;
  auto &frame = requestSession.fullExpressionStates.back();
  if (frame.owner != currentProgramEffectOwner)
    return;
  const Expr *producer = value.get();
  while (true) {
    if (const auto *group = exprAs<Grouping>(producer))
      producer = group->expression().get();
    else if (const auto *comma = exprAs<Binary>(producer);
             comma && comma->oper().kind == TokenKind::COMMA)
      producer = comma->right().get();
    else
      break;
  }
  for (auto &temporary : frame.temporaries)
    if (temporary.producer == producer)
      temporary.consumed = true;
}

auto SemanticVisitor::Impl::recordProgramTemporary(
    const Expr &producer, const ExpressionInfo &info) -> void {
  if (requestSession.fullExpressionStates.empty() ||
      info.category != ValueCategory::Value ||
      !requiresActiveCleanupFor(info.type) || exprAs<Grouping>(&producer))
    return;
  if (const auto *comma = exprAs<Binary>(&producer);
      comma && comma->oper().kind == TokenKind::COMMA)
    return;
  auto &frame = requestSession.fullExpressionStates.back();
  if (frame.owner != currentProgramEffectOwner)
    return;
  if (std::none_of(frame.temporaries.begin(), frame.temporaries.end(),
                   [&](const auto &entry) { return entry.producer == &producer; }))
    frame.temporaries.push_back({.producer = &producer, .type = info.type});
}

auto SemanticVisitor::Impl::recordProgramTemporaryFailureCleanup(
    const Expr &operation,
    const std::vector<ProgramTemporaryCleanup> &temporaries) -> void {
  const auto failure = requestSession.programOperationFailures.find(&operation);
  if (temporaries.empty() || failure == requestSession.programOperationFailures.end())
    return;
  auto check = captureProgramStorageBorrows(tokenSpan(expressionToken(operation)));
  check.siteKind = ProgramStorageBorrowSite::FailureCleanup;
  check.failureCondition = failure->second;
  if (check.protections.empty())
    return;
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  for (const auto &temporary : temporaries)
    if (!temporary.consumed)
      recordProgramCleanupEffects(temporary.type,
                                  tokenSpan(expressionToken(*temporary.producer)));
  publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                   unknownStart);
}

auto SemanticVisitor::Impl::recordProgramExecutedCleanupEffects(
    const SemanticType &type, const SourceSpan &span, bool includeDirect) -> void {
  if (!currentProgramEffectOwner.valid())
    return;
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  recordProgramCleanupEffects(type, span, includeDirect);
  for (std::size_t edge = edgeStart; edge < programEffects[owner].edges.size();
       ++edge)
    programEffects[owner].edges[edge].cleanup = false;
  for (std::size_t unknown = unknownStart;
       unknown < programEffects[owner].unknown.size(); ++unknown)
    programEffects[owner].unknown[unknown].cleanup = false;
}

auto SemanticVisitor::Impl::recordProgramReplacementCleanup(
    const Expr &operation, const SemanticType &type) -> void {
  if (!currentProgramEffectOwner.valid() || !requiresActiveCleanupFor(type))
    return;
  const ExprPtr *input = nullptr;
  if (const auto *assignment = exprAs<Assign>(&operation))
    input = &assignment->value();
  else if (const auto *assignment = exprAs<Set>(&operation))
    input = &assignment->value();
  else if (const auto *assignment = exprAs<IndexSet>(&operation))
    input = &assignment->value();
  else if (const auto *assignment = exprAs<PlaceSet>(&operation))
    input = &assignment->value();
  else if (const auto *assignment = exprAs<DereferenceSet>(&operation))
    input = &assignment->value();
  if (input != nullptr)
    consumeProgramTemporary(*input);
  const auto *event = semanticModel.findOwnershipEvent(operation);
  if (event && event->kind == OwnershipEventKind::Reinitialize &&
      event->before == ValueState::Moved)
    return;
  auto check = captureProgramStorageBorrows(tokenSpan(expressionToken(operation)));
  check.siteKind = ProgramStorageBorrowSite::ReplacementCleanup;
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  recordProgramExecutedCleanupEffects(type, tokenSpan(expressionToken(operation)));
  publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                   unknownStart);
}

auto SemanticVisitor::Impl::finishProgramFullExpression(std::size_t index)
    -> void {
  auto &states = requestSession.fullExpressionStates;
  if (index >= states.size() || index + 1 != states.size())
    return;
  const auto source = states.back().source;
  auto check = captureProgramStorageBorrows(
      source.roots.empty() ? SourceSpan{}
                           : tokenSpan(expressionToken(*source.roots.front())));
  check.siteKind = ProgramStorageBorrowSite::FullExpressionCleanup;
  const auto destination = [&](const ExprPtr &value, const SemanticType &type) {
    const auto *info = value ? semanticModel.findExpression(*value) : nullptr;
    if (info && info->category == ValueCategory::Value &&
        (type == SemanticType::Unknown || type == info->type))
      consumeProgramTemporary(value);
    appendProgramStorageBorrowInput(
        check, value, type.kind == SemanticType::Reference
                          ? std::optional(type.referenceAccess)
                          : std::nullopt);
  };
  if (const auto *binding = stmtAs<VariableDecl>(source.statement)) {
    const auto *info = semanticModel.findBinding(*binding);
    const auto &initializer =
        binding->initializer()
            ? binding->initializer()
            : semanticModel.implicitFieldInitializer(*binding);
    destination(initializer,
                info ? info->type
                : binding->type().name.last().kind == TokenKind::AUTO
                    ? SemanticType::Unknown
                    : typeOf(binding->type()));
  } else if (const auto *returned = stmtAs<ReturnStmt>(source.statement)) {
    destination(returned->value(), currentReturnType);
  } else if (const auto *selection = stmtAs<SwitchStmt>(source.statement)) {
    const auto *info = semanticModel.findExpression(*selection->expression());
    destination(selection->expression(), info ? info->type : SemanticType::Unknown);
  } else if (const auto *initializer = source.constructorInitializer) {
    if (const auto *info = semanticModel.findConstructorInitializer(*initializer)) {
      if (info->kind == ConstructorInitializerTargetKind::Field &&
          initializer->arguments.size() == 1)
        destination(initializer->arguments.front(), info->targetType);
      else
        for (std::size_t argument = 0;
             argument < std::min(initializer->arguments.size(),
                                 info->parameterTypes.size()); ++argument) {
          if (info->parameterTypes[argument].kind != SemanticType::Reference)
            consumeProgramTemporary(initializer->arguments[argument]);
          appendProgramStorageBorrowInput(check, initializer->arguments[argument]);
        }
    }
  }
  auto frame = std::move(states.back());
  states.pop_back();
  if (!frame.owner.valid() || frame.owner != currentProgramEffectOwner)
    return;
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  for (const auto &temporary : frame.temporaries)
    if (!temporary.consumed)
      recordProgramExecutedCleanupEffects(temporary.type,
                                  tokenSpan(expressionToken(*temporary.producer)));
  publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                   unknownStart);
}

auto SemanticVisitor::Impl::recordProgramCallEffectTargets(
    const Expr &expression) -> void {
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
    if (recordProgramConstructionEffects(expression, *construction,
                                         suppliedArguments)) {
      return;
    }
  }
  if (const auto *initializer = exprAs<ArrayInitializer>(&expression)) {
    if (const auto *collection = semanticModel.findCollection(*initializer)) {
      for (FunctionId function : {collection->initialize, collection->insert}) {
        const FunctionInfo *target = semanticModel.findFunction(function);
        if (target == nullptr || !target->genericParameters.empty() ||
            !collection->type.arguments.empty() ||
            !collection->type.valueArguments.empty()) {
          addUnknownProgramEffect(expression,
                                  "a generic collection protocol has no closed "
                                  "program-storage effect summary");
        } else {
          addProgramEffectEdge(
              {.kind = ProgramEffectOwnerKind::Function, .id = function},
              expression);
        }
      }
    }
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
    recordProgramOperatorTargetEffects(expression, *operation);
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
  if (resolved == nullptr) {
    return;
  }
  const FunctionInfo *target =
      resolved->function == 0 ? nullptr
                              : semanticModel.findFunction(resolved->function);
  if (target != nullptr) {
    for (std::size_t index = resolvedCallArgumentCount(*call, resolved);
         index < resolved->parameterTypes.size(); ++index)
      if (index < target->defaultArgumentEffectOwners.size() &&
          target->defaultArgumentEffectOwners[index] != 0)
        addProgramEffectEdge(
            {.kind = ProgramEffectOwnerKind::FunctionDefaultArgument,
             .id = target->defaultArgumentEffectOwners[index]},
            tokenSpan(call->paren()));
  }
  if (resolved->intrinsic != IntrinsicKind::None)
    return;
  recordProgramArgumentBoundaryEffects(resolved->parameterTypes,
                                       ResolvedCallArguments(*call, resolved));
  if (target != nullptr && target->linkage != LanguageLinkage::Gti) {
    return;
  }
  if (resolved->dispatch != CallDispatch::Static || target == nullptr) {
    addUnknownProgramEffect(expression,
                            "a GTI call has no closed static target");
    return;
  }
  if (target->declaration != nullptr && target->declaration->runtimeBinding()) {
    // A trusted runtime boundary contributes only its explicit argument
    // storage effects; it cannot call an unrecorded GTI body.
    return;
  }
  if (target->declaration == nullptr ||
      target->bodyDeclaration()->body() == nullptr) {
    addUnknownProgramEffect(expression,
                            "a selected GTI function has no executable "
                            "body");
    return;
  }
  const ProgramEffectOwner effect{.kind = ProgramEffectOwnerKind::Function,
                                  .id = target->id};
  if (!target->genericParameters.empty() || !resolved->typeArguments.empty() ||
      !resolved->valueArguments.empty() ||
      (target->ownerClass != 0 &&
       !classInfo(target->ownerClass).genericParameters.empty())) {
    recordConcreteProgramEffect(
        effect, resolved->dispatchOwner, resolved->typeArguments,
        resolved->valueArguments, tokenSpan(expressionToken(expression)));
  } else {
    addProgramEffectEdge(effect, expression);
  }
}

auto SemanticVisitor::Impl::recordProgramBorrowStorageEffect(
    const Expr &expression, OccurrenceRole roles) -> void {
  if (!currentProgramEffectOwner.valid() || suppressLoanPlaceAccessDepth != 0 ||
      std::find(suppressedProjectedPlaceReads.begin(),
                suppressedProjectedPlaceReads.end(),
                &expression) != suppressedProjectedPlaceReads.end())
    return;
  const auto kind = expression.exprKind();
  if (kind == ExprKind::Grouping ||
      (analyzingCallCallee && kind == ExprKind::Get))
    return;
  AccessMode access = hasRole(roles, OccurrenceRole::Write)
                          ? AccessMode::Mutable
                          : AccessMode::ReadOnly;
  if (kind == ExprKind::Postfix ||
      (kind == ExprKind::Unary &&
       (static_cast<const Unary &>(expression).oper().kind ==
            TokenKind::PLUS_PLUS ||
        static_cast<const Unary &>(expression).oper().kind ==
            TokenKind::MINUS_MINUS)))
    access = AccessMode::Mutable;
  if (const auto *call = exprAs<Call>(&expression);
      call != nullptr && resolvedIntrinsicKind(*call) == IntrinsicKind::Move)
    access = AccessMode::Mutable;
  SemanticPlace source = semanticPlace(&expression);
  if (const auto *assignment = exprAs<Assign>(&expression)) {
    if (auto *symbol = resolvePlaceSymbol(assignment->name()))
      source = placeForSymbol(*symbol);
  } else if (const auto *postfix = exprAs<Postfix>(&expression)) {
    source = semanticPlace(postfix->expression());
  } else if (const auto *unary = exprAs<Unary>(&expression);
             unary && (unary->oper().kind == TokenKind::PLUS_PLUS ||
                       unary->oper().kind == TokenKind::MINUS_MINUS)) {
    source = semanticPlace(unary->right());
  } else if (const auto *call = exprAs<Call>(&expression);
             call && resolvedIntrinsicKind(*call) == IntrinsicKind::Move &&
             !call->arguments().empty()) {
    source = semanticPlace(call->arguments().front());
  }
  const auto rawAddressOperand = [&](const auto &self,
                                     const Expr *node) -> const Expr * {
    if (!node)
      return nullptr;
    switch (semanticModel.unsafeOperation(*node)) {
    case UnsafeOperationKind::RawDereference:
      if (const auto *unary = exprAs<Unary>(node))
        return unary->right().get();
      if (const auto *set = exprAs<DereferenceSet>(node))
        return set->object().get();
      break;
    case UnsafeOperationKind::RawIndex:
      if (const auto *index = exprAs<Index>(node))
        return index->object().get();
      if (const auto *set = exprAs<IndexSet>(node))
        return set->object().get();
      break;
    case UnsafeOperationKind::RawMember:
      if (const auto *member = exprAs<Get>(node))
        return member->object().get();
      if (const auto *set = exprAs<Set>(node))
        return set->object().get();
      break;
    default:
      break;
    }
    if (const auto *member = exprAs<Get>(node))
      return self(self, member->object().get());
    if (const auto *set = exprAs<Set>(node))
      return self(self, set->object().get());
    if (const auto *index = exprAs<Index>(node))
      return self(self, index->object().get());
    if (const auto *set = exprAs<IndexSet>(node))
      return self(self, set->object().get());
    if (const auto *set = exprAs<PlaceSet>(node))
      return self(self, set->target().get());
    if (const auto *group = exprAs<Grouping>(node))
      return self(self, group->expression().get());
    if (const auto *postfix = exprAs<Postfix>(node))
      return self(self, postfix->expression().get());
    if (const auto *unary = exprAs<Unary>(node))
      return self(self, unary->right().get());
    return nullptr;
  };
  if (auto *address = rawAddressOperand(rawAddressOperand, &expression)) {
    // The pointed-to access is an explicit unsafe obligation. Only loading
    // its ordinary pointer slot is an ambient checked-storage effect.
    while (const auto *base = rawAddressOperand(rawAddressOperand, address))
      address = base;
    source = semanticPlace(address);
    access = AccessMode::ReadOnly;
  }
  // A local reference selected from persistent storage still denotes that
  // storage. Preserve its checked origin rather than mistaking the local
  // carrier's declaration for the accessed object.
  for (const auto &place : loanPlaces(source))
    if (isPersistentStorageSymbol(place.root))
      programEffects[currentProgramEffectOwner].borrowUses.push_back(
          {.place = {.root = place.root, .projections = place.projections},
           .access = access,
           .span = tokenSpan(expressionToken(expression))});
}

auto SemanticVisitor::Impl::closeProgramFailureEffects() -> void {
  auto &failing = requestSession.programFailingEffects;
  failing.clear();
  for (const auto &[owner, summary] : programEffects)
    if (summary.directFailure ||
        std::any_of(summary.unknown.begin(), summary.unknown.end(),
                    [](const auto &effect) { return !effect.cleanup; }))
      failing.insert(owner);
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto &[owner, summary] : programEffects) {
      if (failing.contains(owner))
        continue;
      if (std::any_of(summary.edges.begin(), summary.edges.end(),
                      [&](const auto &edge) {
                        return !edge.cleanup && failing.contains(edge.target);
                      }))
        changed |= failing.insert(owner).second;
    }
  }
}

auto SemanticVisitor::Impl::programFailureMayHappen(
    const ProgramFailureCondition &condition) const -> bool {
  return condition.direct || condition.unknown ||
         std::any_of(condition.targets.begin(), condition.targets.end(),
                     [&](const auto target) {
                       return requestSession.programFailingEffects.contains(target);
                     });
}

auto SemanticVisitor::Impl::validateProgramStorageBorrowEffects() -> void {
  closeProgramFailureEffects();
  for (const auto &check : requestSession.programStorageBorrowChecks) {
    if (check.protections.empty() ||
        (check.failureCondition && !programFailureMayHappen(*check.failureCondition)))
      continue;
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visiting;
    std::optional<ProgramBorrowStorageUse> conflict;
    std::optional<ProgramStorageBorrowProtection> protection;
    std::optional<ProgramUnknownEffect> unknown;
    if (!check.unknown.empty())
      unknown = check.unknown.front();
    const auto inspectUses = [&](const auto &uses, bool rollback = true) {
      for (const auto &use : uses) {
        if (use.cleanup && !rollback)
          continue;
        for (const auto &loan : check.protections) {
          if (loan.place.root != use.place.root ||
              (loan.access == AccessMode::ReadOnly &&
               use.access == AccessMode::ReadOnly))
            continue;
          auto accessed = loan.place;
          accessed.projections = use.place.projections;
          if (loanPlacesOverlap(loan.place, accessed)) {
            conflict = use;
            protection = loan;
            return;
          }
        }
      }
    };
    inspectUses(check.uses);
    const auto visit = [&](const auto &self, ProgramEffectOwner owner) -> void {
      // An existing checked loan of this thread's slot proves its successful
      // initialization already completed. Its Ensure cannot reexecute the
      // initializer, even when reached transitively through an accessor.
      if (owner.kind == ProgramEffectOwnerKind::Initializer &&
          isThreadLocalStorage(owner.id) &&
          std::any_of(
              check.protections.begin(), check.protections.end(),
              [&](const auto &loan) { return loan.place.root == owner.id; }))
        return;
      if (conflict || unknown || !visiting.insert(owner).second)
        return;
      const auto found = programEffects.find(owner);
      if (found == programEffects.end())
        return;
      const auto &summary = found->second;
      const bool rollback = requestSession.programFailingEffects.contains(owner);
      const auto open = std::find_if(
          summary.unknown.begin(), summary.unknown.end(),
          [&](const auto &effect) { return !effect.cleanup || rollback; });
      if (open != summary.unknown.end()) {
        unknown = *open;
        return;
      }
      inspectUses(summary.borrowUses, rollback);
      if (conflict)
        return;
      for (const auto &edge : summary.edges)
        if (!edge.cleanup || rollback)
          self(self, edge.target);
    };
    for (const auto target : check.targets)
      visit(visit, target);
    if (!conflict && !unknown)
      continue;
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2045", DiagnosticPhase::Semantics, check.site,
        conflict ? "This call accesses persistent storage through an "
                   "overlapping path while a conflicting borrow is live."
                 : "This call has unresolved GTI storage effects while a "
                   "borrow of persistent storage is live.");
    const char *context = nullptr;
    switch (check.siteKind) {
    case ProgramStorageBorrowSite::Invocation:
      break;
    case ProgramStorageBorrowSite::OrderedInput:
      context = "Earlier reference or borrowed-state inputs are already bound "
                "while this later input is evaluated.";
      break;
    case ProgramStorageBorrowSite::ScopeCleanup:
      context = "This automatic object's cleanup runs at scope exit.";
      break;
    case ProgramStorageBorrowSite::FullExpressionCleanup:
      context = "Remaining temporaries are cleaned at this full-expression "
                "boundary.";
      break;
    case ProgramStorageBorrowSite::FailureCleanup:
      context = "Earlier untransferred temporaries are cleaned if this "
                "operation propagates a failure.";
      break;
    case ProgramStorageBorrowSite::ReplacementCleanup:
      context = "Replacing this live object runs its previous value's cleanup.";
      break;
    }
    if (context)
      diagnostic.related.push_back({check.site, context});
    if (conflict) {
      diagnostic.related.push_back(
          {conflict->span,
           "The conflicting transitive storage access occurs here."});
      diagnostic.related.push_back(
          {protection->origin, "The protected borrow originates here."});
    } else {
      diagnostic.related.push_back(
          {unknown->span, "The GTI call effect is unresolved here."});
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }
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
  if (variable == nullptr || variable->linkage() == LanguageLinkage::C ||
      (enclosingClass && !variable->isStatic())) {
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
       .role = ((variable->isThreadLocal()
                     ? isThreadLocalDataOnly(*variable, binding->type)
                     : !variable->initializer()) ||
                (variable->isConstexpr() && binding->constant &&
                 canDataInitializeProgramConstant(*binding->constant) &&
                 !requiresActiveCleanupFor(binding->type)))
                   ? ProgramInitializationStepRole::DataOnly
                   : ProgramInitializationStepRole::Initializer,
       .declaration = variable,
       .symbol = binding->symbol,
       .ownerClass = enclosingClass.value_or(0),
       .requiresActiveCleanup = requiresActiveCleanupFor(binding->type),
       .threadLocal = variable->isThreadLocal(),
       .ensureFailure =
           variable->isThreadLocal()
               ? threadLocalEnsureFailure(binding->symbol, variable->name())
               : DefinedFailureOperation{},
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
    if (!step.threadLocal)
      stepForSymbol.emplace(step.symbol, step.id);
  }
  for (const ProgramInitializationStep &initializer : plan.steps) {
    if (initializer.threadLocal ||
        initializer.role == ProgramInitializationStepRole::DataOnly) {
      continue;
    }
    if (initializer.declaration->isConstexpr()) {
      const auto *binding = semanticModel.findBinding(*initializer.declaration);
      if (binding == nullptr || !binding->constant) {
        // A rejected required constant has no executable initializer. Its
        // evaluation diagnostic owns the failure, not runtime effect proof.
        continue;
      }
    }
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visiting;
    std::unordered_set<ProgramEffectOwner, ProgramEffectOwnerHash> visited;
    const std::function<bool(ProgramEffectOwner, std::optional<SourceSpan>)>
        visit = [&](ProgramEffectOwner owner,
                    std::optional<SourceSpan> firstEdge) {
          if (visited.contains(owner) || !visiting.insert(owner).second) {
            return false;
          }
          // Late demand must not analyse a body this validation relied on.
          initializerEffectOwners.insert(owner);
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
              if (isThreadLocalStorage(use.symbol))
                continue;
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
    if (step.threadLocal)
      continue;
    const BindingInfo *binding =
        step.declaration == nullptr
            ? nullptr
            : semanticModel.findBinding(*step.declaration);
    if (binding != nullptr && binding->constant &&
        canMaterializeProgramConstant(*binding->constant)) {
      constants.insert_or_assign(step.symbol, &*binding->constant);
    }
  }
  for (auto &[owner, summary] : programEffects) {
    // A late batch substitutes only in the bodies it analysed.
    if (lateFinalization != nullptr &&
        !(owner.kind == ProgramEffectOwnerKind::Function &&
          lateFinalization->functionIds.contains(owner.id)) &&
        !(owner.kind == ProgramEffectOwnerKind::Lambda &&
          owner.id >= lateFinalization->firstLambda))
      continue;
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
  ProgramInitializationPlan threadPlan;
  threadPlan.unitOrder = plan.unitOrder;
  std::erase_if(plan.steps, [&](const auto &step) {
    if (!step.threadLocal)
      return false;
    threadPlan.steps.push_back(step);
    return true;
  });
  for (const auto &step : requestSession.threadLocalSteps)
    if (std::none_of(threadPlan.steps.begin(), threadPlan.steps.end(),
                     [&](const auto &existing) {
                       return existing.symbol == step.symbol;
                     }))
      threadPlan.steps.push_back(step);
  for (std::size_t index = 0; index < threadPlan.steps.size(); ++index)
    threadPlan.steps[index].id = index + 1;
  semanticModel.setThreadLocalInitializationPlan(std::move(threadPlan));
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    plan.steps[index].id = index + 1;
  }
  closeConcreteProgramEffects(plan);
  closeConcreteProgramEffects(semanticModel.threadLocalInitializationPlan());
  finalizeDeferredProgramConstantSubstitutions(plan);
  validateProgramInitializationEffects(plan);
  validateProgramStorageBorrowEffects();
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
  if (const auto *enumDecl = stmtAs<EnumDecl>(&statement)) {
    return sourceUnitFor(enumDecl->name());
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
        // One specialization's selected constexpr branch cannot make shared
        // source unreachable. Declaration-wide tails are diagnosed by primary
        // analysis; concrete reanalysis still checks the tail's semantics.
        if (!instanceAnalysisActive && statement != nullptr &&
            statement->extent()) {
          Diagnostic diagnostic = makeDiagnostic(
              DiagnosticId::UnreachableStatement, *statement->extent());
          diagnostic.tags.push_back(DiagnosticTag::Unnecessary);
          diagnostics.emplace_back(std::move(diagnostic));
        }
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
  if (const Expr *alias = semanticModel.findExpressionAlias(expr)) {
    // A reflected field access is the place its alias denotes; the call's
    // own borrow access decides whether that place is writable through it.
    if (const ExpressionInfo *info = semanticModel.findExpression(*alias)) {
      ExpressionInfo result = *info;
      result.type = std::move(type);
      if (const auto *call = exprAs<Call>(&expr)) {
        const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
        if (resolved != nullptr &&
            resolved->borrowAccess != AccessMode::Mutable) {
          result.access = AccessMode::ReadOnly;
        }
      }
      return result;
    }
  }
  const auto borrowedTextType = [](const SemanticType &candidate) {
    return candidate.kind == SemanticType::StringView ||
           candidate.kind == SemanticType::CString;
  };
  // A fresh value whose text field or payload received dynamic text carries
  // that owner dependency itself; its type stays dependency-free (L-OWN-02).
  const auto textDependentType = [&](const SemanticType &candidate) {
    return (candidate.kind == SemanticType::Class ||
            candidate.kind == SemanticType::Enum) &&
           !typeTraits(candidate).containsBorrowedState;
  };
  if (const auto *construction = semanticModel.findConstruction(expr);
      construction != nullptr && construction->dependentTextPublication &&
      textDependentType(type))
    return textDependentValueInfo(std::move(type));
  if (const auto *call = exprAs<Call>(&expr))
    if (const auto *payload = semanticModel.findPayloadConstruction(*call);
        payload != nullptr && payload->dependentTextPublication &&
        textDependentType(type))
      return textDependentValueInfo(std::move(type));
  const auto preserveCategory = [&](const Expr &source) {
    const ExpressionInfo *sourceInfo = semanticModel.findExpression(source);
    if (borrowedTextType(type) && sourceInfo != nullptr &&
        sourceInfo->traits.containsBorrowedState) {
      return ownerDependentTextInfo(type, sourceInfo->category,
                                    sourceInfo->access);
    }
    if (textDependentType(type) && sourceInfo != nullptr &&
        sourceInfo->traits.containsBorrowedState)
      return textDependentValueInfo(type, sourceInfo->category,
                                    sourceInfo->access);
    return sourceInfo == nullptr
               ? expressionInfo(std::move(type))
               : expressionInfo(std::move(type), sourceInfo->category,
                                sourceInfo->access);
  };

  if (kind == ExprKind::Grouping) {
    return preserveCategory(*static_cast<const Grouping &>(expr).expression());
  }
  if (const auto *unary = exprAs<Unary>(&expr);
      unary != nullptr && unary->oper().kind == TokenKind::QUESTION) {
    if (const auto *plan = semanticModel.findTryablePropagation(*unary);
        plan != nullptr &&
        plan->output.returnType.kind == SemanticType::Reference)
      return expressionInfo(std::move(type), ValueCategory::Place,
                            plan->output.borrowAccess);
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
             ? symbol->type.referenceAccess == AccessMode::Mutable &&
                   (symbol->ownerClass == 0 || symbol->staticMember ||
                    receiverAllowsMutation(currentReceiverMutability))
             : symbol->assignable &&
                   (symbol->ownerClass == 0 || symbol->staticMember ||
                    receiverAllowsMutation(currentReceiverMutability)));
    if (borrowedTextType(type) && symbol != nullptr &&
        (symbol->retainedLoan != 0 ||
         (symbol->ownerClass != 0 && !symbol->staticMember &&
          symbol->type.kind == SemanticType::Reference))) {
      return ownerDependentTextInfo(type, ValueCategory::Place,
                                    mutableAccess ? AccessMode::Mutable
                                                  : AccessMode::ReadOnly);
    }
    // An implicit receiver text field reads like `this.field` (L-OWN-02).
    if (borrowedTextType(type) && implicitReceiverTextField(variable))
      return ownerDependentTextInfo(type, ValueCategory::Place,
                                    mutableAccess ? AccessMode::Mutable
                                                  : AccessMode::ReadOnly);
    // A local binding that retains a dependent text value (L-OWN-02). A
    // reference binding's loan is its referent's, not a text dependency.
    if (textDependentType(type) && symbol != nullptr && symbol->type == type &&
        symbol->retainedLoan != 0 && symbol->variableDeclaration != nullptr) {
      const auto *binding =
          semanticModel.findBinding(*symbol->variableDeclaration);
      if (binding != nullptr && binding->traits.containsBorrowedState)
        return textDependentValueInfo(type, ValueCategory::Place,
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
             ? member->symbol.type.referenceAccess == AccessMode::Mutable &&
                   memberReceiverIsMutable(get)
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
    // Reading a checked text-reference field copies its dependent view, not
    // an ownerless value. The receiver's stored-reference contract supplies
    // the external lifetime for explicit and implicit field spelling alike.
    if (borrowedTextType(type) && member != nullptr &&
        member->symbol.type.kind == SemanticType::Reference) {
      return ownerDependentTextInfo(type, ValueCategory::Place,
                                    mutableAccess ? AccessMode::Mutable
                                                  : AccessMode::ReadOnly);
    }
    // A text field read through a formal or the receiver may hold a relayed
    // owner's dynamic text, under the function's field contract (L-OWN-02).
    if (borrowedTextType(type) && member != nullptr &&
        get.access().kind != TokenKind::ARROW && get.object() &&
        textFieldContractFormal(*get.object()) &&
        directTextField(memberObjectType,
                        symbolForDeclaration(member->symbol.declaration)))
      return ownerDependentTextInfo(type, ValueCategory::Place,
                                    mutableAccess ? AccessMode::Mutable
                                                  : AccessMode::ReadOnly);
    // A text field that a dependent value's owner dependency designates
    // holds that owner's dynamic text (L-OWN-02). Its other fields stay
    // ownerless.
    if (borrowedTextType(type) && member != nullptr && objectInfo != nullptr &&
        get.access().kind != TokenKind::ARROW &&
        isTextDependentValue(*objectInfo)) {
      const auto fields = textDependentFields(*get.object());
      if (std::find(fields.begin(), fields.end(),
                    symbolForDeclaration(member->symbol.declaration)) !=
          fields.end())
        return ownerDependentTextInfo(type, ValueCategory::Place,
                                      mutableAccess ? AccessMode::Mutable
                                                    : AccessMode::ReadOnly);
    }
    return expressionInfo(std::move(type), ValueCategory::Place,
                          mutableAccess ? AccessMode::Mutable
                                        : AccessMode::ReadOnly);
  }
  if (kind == ExprKind::Call) {
    const Call &call = static_cast<const Call &>(expr);
    const ResolvedCallInfo *resolved = semanticModel.findCall(call);
    if (resolved != nullptr &&
        resolved->returnType.kind == SemanticType::Reference &&
        resolved->returnType.arguments.size() == 1) {
      return expressionInfo(std::move(type), ValueCategory::Place,
                            resolved->returnType.referenceAccess);
    }
    // A lambda value or generic callable invocation with a reference result
    // denotes the referent place exactly like a function call result.
    if (resolved == nullptr) {
      const SemanticType *callableResult = nullptr;
      if (const ResolvedLambdaCallInfo *lambdaCall =
              semanticModel.findLambdaCall(call)) {
        callableResult = &lambdaCall->returnType;
      } else if (const DeferredCallableCallInfo *deferred =
                     semanticModel.findDeferredCallableCall(call)) {
        callableResult = &deferred->returnType;
      }
      if (callableResult != nullptr &&
          callableResult->kind == SemanticType::Reference &&
          callableResult->arguments.size() == 1) {
        return expressionInfo(std::move(type), ValueCategory::Place,
                              callableResult->referenceAccess);
      }
    }
    // Moving a dependent text value transfers its owner dependency.
    if (textDependentType(type) && call.arguments().size() == 1 &&
        resolvedIntrinsicKind(call) == IntrinsicKind::Move) {
      const ExpressionInfo *sourceInfo =
          semanticModel.findExpression(*call.arguments().front());
      if (sourceInfo != nullptr && isTextDependentValue(*sourceInfo))
        return textDependentValueInfo(std::move(type));
    }
    if (borrowedTextType(type) && resolved != nullptr &&
        resolved->borrowOrigin != BorrowOriginKind::None) {
      // A result that returns one text field of its argument or receiver
      // (L-OWN-02) holds what that value holds there: a relayed owner's text,
      // or the field of a contract formal here. Any other value holds
      // ownerless text in that field.
      if (const auto field = fieldSourcedTextResult(call, *resolved)) {
        const ExpressionInfo *sourceInfo =
            field->origin == nullptr
                ? nullptr
                : semanticModel.findExpression(**field->origin);
        std::vector<SymbolId> held;
        if (sourceInfo != nullptr && isTextDependentValue(*sourceInfo))
          held = textDependentFields(**field->origin);
        if (fieldSourcedTextFormal(*field) ||
            std::find(held.begin(), held.end(), field->field) != held.end())
          return ownerDependentTextInfo(std::move(type));
        return expressionInfo(std::move(type));
      }
      if (resolved->borrowOrigin == BorrowOriginKind::Argument &&
          resolved->borrowArgument <
              resolvedCallArgumentCount(call, resolved)) {
        const ExpressionInfo *sourceInfo = semanticModel.findExpression(
            **resolvedCallArgument(call, resolved, resolved->borrowArgument));
        const bool ownerlessCStringSource =
            type.kind == SemanticType::CString && sourceInfo != nullptr &&
            (sourceInfo->type.kind == SemanticType::CString ||
             sourceInfo->type.kind == SemanticType::StringView ||
             sourceInfo->type.kind == SemanticType::NullPtr);
        if (sourceInfo != nullptr &&
            (sourceInfo->type.kind == type.kind || ownerlessCStringSource) &&
            !sourceInfo->traits.containsBorrowedState) {
          return expressionInfo(std::move(type));
        }
      }
      return ownerDependentTextInfo(std::move(type));
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
    const Expr *leftExpression, const Expr *rightExpression) const
    -> SemanticType {
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
    DefinedFailureOrigin conversion = operation.localOrigins.back();
    conversion.outcomes.clear();
    operation.localOrigins.push_back(std::move(conversion));
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
  const auto selectedConstructor = [&] {
    const auto *construction = semanticModel.findConstruction(expr);
    if (construction == nullptr ||
        construction->kind != ConstructorKind::Ordinary ||
        construction->constructedType.kind != SemanticType::Class)
      return false;
    if (!construction->generatedDefault)
      return construction->constructor != 0;
    const auto *owner = classInfo(construction->constructedType);
    const auto *lifecycle =
        owner == nullptr || owner->declaration == nullptr
            ? nullptr
            : semanticModel.findClassLifecycle(*owner->declaration);
    return lifecycle != nullptr && lifecycle->generatedDefaultInitializers;
  };

  const SymbolId storageSymbol = semanticModel.findResolvedSymbol(expr);
  if (storageSymbol != 0 && isThreadLocalStorage(storageSymbol)) {
    operation = threadLocalEnsureFailure(storageSymbol, expressionToken(expr));
  }

  if (const ResolvedOperatorInfo *resolved = semanticModel.findOperator(expr)) {
    const bool assignmentExpression =
        kind == ExprKind::Assign || kind == ExprKind::Set ||
        kind == ExprKind::IndexSet || kind == ExprKind::DereferenceSet ||
        kind == ExprKind::PlaceSet;
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

  if (selectedConstructor() &&
      (callResolution == nullptr ||
       callResolution->intrinsic == IntrinsicKind::None)) {
    operation.propagation = FailurePropagationKind::Constructor;
    return operation;
  }

  if (kind == ExprKind::Call) {
    const auto *call = static_cast<const Call *>(&expr);
    if (const ResolvedCallInfo *resolved = semanticModel.findCall(*call)) {
      const IntrinsicKind intrinsic = resolved->intrinsic;
      if (intrinsic != IntrinsicKind::None) {
        const Token token = callableToken(call->callee());
        switch (intrinsic) {
        case IntrinsicKind::Panic:
        case IntrinsicKind::FailEmptyOwner:
        case IntrinsicKind::FailInactiveValue:
        case IntrinsicKind::FailInactiveError:
          anchor(token);
          addFailureOutcome(operation,
                            intrinsicTerminalFailureOutcome(intrinsic)->code,
                            intrinsicTerminalFailureOutcome(intrinsic)->detail);
          break;
        case IntrinsicKind::FunctionPointerInvoke:
          // The erased signature preserves GTI failure propagation but not
          // a statically selected target's narrower failure set.
          operation.propagation = FailurePropagationKind::Callable;
          break;
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
        case IntrinsicKind::SharedOwnerFromUnique:
        case IntrinsicKind::SharedOwnerClone:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::SharedOwner);
          break;
        case IntrinsicKind::SharedOwnerBorrow:
        case IntrinsicKind::UniqueOwnerBorrow:
        case IntrinsicKind::UniqueOwnerBorrowMut:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::EmptyOwnerAccess,
                            uniqueOwnerAccessDetail());
          break;
        case IntrinsicKind::ObjectConstruct:
          // Raw validity is the unsafe caller's obligation. Only the selected
          // GTI constructor may fail; there is no allocation or slot preflight.
          if (selectedConstructor())
            operation.propagation = FailurePropagationKind::Constructor;
          break;
        case IntrinsicKind::RestoringIndexedSwap:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::IndexOutOfBounds,
                            DefinedFailureDetail::PrivateStorage);
          break;
        case IntrinsicKind::OptionalStorageConstruct:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::InvalidStorageState,
                            DefinedFailureDetail::DuplicateConstruction);
          addFailureOutcome(operation, DefinedFailureCode::AllocationFailure,
                            DefinedFailureDetail::ElementConstruction);
          break;
        case IntrinsicKind::OptionalStorageRead:
        case IntrinsicKind::OptionalStorageReadMut:
          anchor(token);
          addFailureOutcome(operation, DefinedFailureCode::EmptyOwnerAccess,
                            DefinedFailureDetail::MemberAccess);
          break;
        case IntrinsicKind::TextViewFromAddress:
        case IntrinsicKind::EraseReference:
        case IntrinsicKind::EraseReferenceMut:
        case IntrinsicKind::RestoreReference:
        case IntrinsicKind::RestoreReferenceMut:
        case IntrinsicKind::FunctionPointerFromItem:
        case IntrinsicKind::CStringFromAddress:
        case IntrinsicKind::OptionalStorageHasValue:
        case IntrinsicKind::OptionalStorageReset:
        // Polymorphic storage preconditions are private library invariants;
        // a violation traps rather than producing a defined failure.
        case IntrinsicKind::PolymorphicStorageHasValue:
        case IntrinsicKind::PolymorphicStorageConstruct:
        case IntrinsicKind::PolymorphicStorageRead:
        case IntrinsicKind::PolymorphicStorageReadMut:
        case IntrinsicKind::PolymorphicStorageReset:
        case IntrinsicKind::ObjectDestroy:
        case IntrinsicKind::ObjectRelocate:
        case IntrinsicKind::ObjectRelocateRange:
        case IntrinsicKind::StringViewSize:
        case IntrinsicKind::StringViewEmpty:
        case IntrinsicKind::ArrayRangeBegin:
        case IntrinsicKind::ArraySize:
        case IntrinsicKind::ThreadHandleEmpty:
        case IntrinsicKind::ThreadSpawn:
        case IntrinsicKind::ThreadJoinable:
        case IntrinsicKind::ThreadResultTake:
        case IntrinsicKind::AtomicLoad:
        case IntrinsicKind::AtomicWait:
        case IntrinsicKind::AtomicNotifyOne:
        case IntrinsicKind::AtomicNotifyAll:
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
        case IntrinsicKind::ThreadScope:
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
        case IntrinsicKind::RequireKey:
        case IntrinsicKind::RequireAllocation:
        case IntrinsicKind::RequireContainerArgument:
        case IntrinsicKind::RequireVacantStorage:
        case IntrinsicKind::RequireLiveStorage:
        case IntrinsicKind::RequireRelocationCapacity:
        case IntrinsicKind::RequireRelocationSource:
        case IntrinsicKind::RequireRelocationDestination: {
          anchor(token);
          const auto outcome = *intrinsicBooleanFailureOutcome(intrinsic);
          addFailureOutcome(operation, outcome.code, outcome.detail);
          break;
        }
        case IntrinsicKind::ExactTypeParameterConstruction:
        case IntrinsicKind::RawPointerConversion:
        case IntrinsicKind::DefaultTypeParameterConstruction:
        case IntrinsicKind::Move:
        case IntrinsicKind::UniqueOwnerIsNull:
        case IntrinsicKind::UniqueOwnerSwap:
        case IntrinsicKind::RestoringExchange:
        case IntrinsicKind::RestoringSwap:
        case IntrinsicKind::EnumUnderlyingValue:
        case IntrinsicKind::ObjectSize:
        case IntrinsicKind::ObjectAlignment:
        case IntrinsicKind::RawBorrow:
        case IntrinsicKind::RawBorrowMut:
        case IntrinsicKind::UniqueOwnerUpcast:
        case IntrinsicKind::SharedOwnerEmpty:
        case IntrinsicKind::SharedOwnerIsNull:
        case IntrinsicKind::SharedOwnerUseCount:
        case IntrinsicKind::SharedOwnerSwap:
        case IntrinsicKind::SharedOwnerUpcast:
        case IntrinsicKind::SourceFileName:
        case IntrinsicKind::SourceFunctionName:
        case IntrinsicKind::SourceLine:
        case IntrinsicKind::SourceColumn:
        case IntrinsicKind::MetaFieldCount:
        case IntrinsicKind::MetaFieldName:
        case IntrinsicKind::MetaFieldIsPublic:
        case IntrinsicKind::MetaFieldHasDefault:
        case IntrinsicKind::MetaTypeName:
        case IntrinsicKind::MetaTypeKind:
        case IntrinsicKind::MetaFieldAttributeCount:
        case IntrinsicKind::MetaFieldHasAttribute:
        case IntrinsicKind::MetaTypeAttributeCount:
        case IntrinsicKind::MetaTypeHasAttribute:
        case IntrinsicKind::MetaTypeTrait:
        case IntrinsicKind::MetaEnumeratorCount:
        case IntrinsicKind::MetaEnumeratorName:
        case IntrinsicKind::MetaEnumeratorValue:
        case IntrinsicKind::MetaPayloadCount:
        case IntrinsicKind::MetaPayloadName:
        case IntrinsicKind::MetaBaseCount:
        case IntrinsicKind::MetaArgumentCount:
        case IntrinsicKind::MetaMethodCount:
        case IntrinsicKind::MetaMethodName:
        case IntrinsicKind::MetaMethodTrait:
        case IntrinsicKind::MetaMethodParameterCount:
        case IntrinsicKind::MetaParameterName:
        case IntrinsicKind::MetaConstructorCount:
        case IntrinsicKind::MetaConstructorParameterCount:
        case IntrinsicKind::MetaConstructorParameterName:
        case IntrinsicKind::MetaDeclarationLine:
        case IntrinsicKind::MetaDeclarationFile:
        case IntrinsicKind::MetaTypePrimaryName:
        case IntrinsicKind::MetaFieldAttribute:
        case IntrinsicKind::MetaTypeAttribute:
        case IntrinsicKind::MetaFieldDefault:
        case IntrinsicKind::MetaFieldGet:
        case IntrinsicKind::MetaFieldGetMut:
        case IntrinsicKind::MetaConstruct:
        case IntrinsicKind::SimdAdd:
        case IntrinsicKind::SimdSubtract:
        case IntrinsicKind::SimdMultiply:
        case IntrinsicKind::SimdDivide:
        case IntrinsicKind::SimdEqual:
        case IntrinsicKind::SimdLess:
        case IntrinsicKind::IntegerWrappingAdd:
        case IntrinsicKind::IntegerWrappingSubtract:
        case IntrinsicKind::IntegerWrappingMultiply:
        case IntrinsicKind::IntegerSaturatingAdd:
        case IntrinsicKind::IntegerSaturatingSubtract:
        case IntrinsicKind::IntegerSaturatingMultiply:
        case IntrinsicKind::IntegerAddOverflows:
        case IntrinsicKind::IntegerSubtractOverflows:
        case IntrinsicKind::IntegerMultiplyOverflows:
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
          anchor(unary->oper());
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
        anchor(postfix->oper());
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
  } else if (kind == ExprKind::PlaceSet) {
    const auto *set = static_cast<const PlaceSet *>(&expr);
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
  ProgramExpressionEffects evaluation{.owner = currentProgramEffectOwner};
  if (evaluation.owner.valid()) {
    const auto &summary = programEffects[evaluation.owner];
    evaluation.edgeStart = summary.edges.size();
    evaluation.unknownStart = summary.unknown.size();
    evaluation.useStart = summary.borrowUses.size();
  }
  const auto fullExpression = beginProgramFullExpression(expr);
  if (instanceAnalysisActive) {
    semanticModel.suppressInheritedOperator(expr);
  }
  valueFlow.rawOperatorResults.erase(&expr);
  expr.accept(*this);
  SemanticType result = currentType;
  const auto *construction = semanticModel.findConstruction(expr);
  const bool directInitialization =
      construction != nullptr && construction->directInitialization;
  if (!directInitialization && (exprAs<Call>(&expr) || construction))
    validateRawCallAccesses(expr);
  if (const auto *call = exprAs<Call>(&expr); call && !directInitialization)
    preserveRawCallWrites(*call);
  if (const auto *call = exprAs<Call>(&expr); call && !directInitialization)
    invalidateRawCallOrigins(call);
  if (!directInitialization && (exprAs<Call>(&expr) || construction))
    applyRawCallWrites(expr);
  ExpressionInfo info = classifyExpression(expr, result);
  // A field read, a grouping and a move keep a dependent text value inside
  // the body that created it (L-OWN-02).
  if (isTextDependentValue(info))
    noteTextDependentValue(expr);
  // Inside a field-contract scope a formal or the receiver of a publishable
  // class is noted the same way: field reads and forwarding calls consume it.
  if (exprAs<Variable>(&expr) != nullptr || exprAs<This>(&expr) != nullptr)
    if (const auto formal = textFieldContractFormal(expr))
      noteTextFieldFormalUse(expr, *formal);
  if (const auto *get = exprAs<Get>(&expr);
      get != nullptr && result != SemanticType::Function) {
    consumeTextDependentValue(get->object());
    consumeTextFieldFormalUse(get->object());
  } else if (const auto *group = exprAs<Grouping>(&expr))
    consumeTextDependentValue(group->expression());
  else if (const auto *call = exprAs<Call>(&expr);
           call != nullptr && call->arguments().size() == 1 &&
           resolvedIntrinsicKind(*call) == IntrinsicKind::Move)
    consumeTextDependentValue(call->arguments().front());
  if (semanticModel.findOperator(expr) == nullptr && isNumeric(result)) {
    const auto compound = [&](TokenKind token, const ExprPtr &right) {
      if (token != TokenKind::EQUAL) {
        info.updateArithmeticType = compoundIntermediateType(
            token, result, semanticModel.typeOf(*right), nullptr, right.get());
      }
    };
    if (const auto *unary = exprAs<Unary>(&expr);
        unary != nullptr && (unary->oper().kind == TokenKind::PLUS_PLUS ||
                             unary->oper().kind == TokenKind::MINUS_MINUS)) {
      info.updateArithmeticType =
          isInteger(result) ? promotedInteger(result) : result;
    } else if (expr.exprKind() == ExprKind::Postfix) {
      info.updateArithmeticType =
          isInteger(result) ? promotedInteger(result) : result;
    } else if (const auto *assign = exprAs<Assign>(&expr)) {
      compound(assign->oper().kind, assign->value());
    } else if (const auto *set = exprAs<Set>(&expr)) {
      compound(set->oper().kind, set->value());
    } else if (const auto *set = exprAs<IndexSet>(&expr)) {
      compound(set->oper().kind, set->value());
    } else if (const auto *set = exprAs<DereferenceSet>(&expr)) {
      compound(set->oper().kind, set->value());
    } else if (const auto *set = exprAs<PlaceSet>(&expr)) {
      compound(set->oper().kind, set->value());
    }
  }
  semanticModel.record(expr, info);
  semanticModel.recordDefinedFailure(expr, classifyDefinedFailure(expr, info));
  if (expr.exprKind() == ExprKind::Call &&
      info.type.kind == SemanticType::RawPointer &&
      info.type.arguments.size() == 1 &&
      info.type.arguments.front() != SemanticType::Void &&
      !containsOpenTypeParameter(info.type)) {
    const auto origins = rawPointerOrigins(&expr);
    for (auto &loop : valueFlow.rawPointerLoops)
      loop.accesses.push_back(
          {origins, expressionToken(expr), {}, info.type.arguments.front()});
    for (const auto &origin : origins) {
      if (origin.pointeeType != SemanticType::Unknown &&
          !containsOpenTypeParameter(origin.pointeeType) &&
          origin.pointeeType != info.type.arguments.front()) {
        // These origins are optional audit evidence, not the source type of
        // the call. In particular, a source allocator may erase byte-address
        // arithmetic before constructing a different object in its region.
        // Preserve the finding (and Strict promotion), without making Warn
        // change acceptance. Static pointer shape/constness remain mandatory
        // checks in analyzeAliasConversion, independently of this metadata.
        diagnostics.push_back(makeDiagnostic(
            origin.possible ? DiagnosticId::RawAuditPotential
                            : DiagnosticId::RawAuditViolation,
            expressionToken(expr),
            {std::string(origin.possible
                             ? "Raw pointer result may contradict a possible "
                               "pointee type '"
                             : "Raw pointer result contradicts the known "
                               "pointee type '") +
             typeSpelling(origin.pointeeType) + "'."}));
        diagnostics.back().related.push_back(
            {origin.addressSite, "Pointee type retained at this conversion."});
        break;
      }
    }
  }
  // Keep a source token for calls, including source operations that select an
  // ordinary operator and expected's synthesized mapper/failure branches.
  // This is location provenance only, not a transitive failure category set.
  if (expr.exprKind() == ExprKind::Call ||
      expr.exprKind() == ExprKind::DirectInitializer ||
      semanticModel.findOperator(expr)) {
    const Token location = expressionToken(expr);
    const SourceSpan span = tokenSpan(location);
    semanticModel.recordCallerLocation(expr,
                                       {.sourceUnit = sourceUnitFor(location),
                                        .start = span.start,
                                        .end = span.end,
                                        .line = span.line});
  }
  const SemanticPlace expressionPlace = semanticPlace(&expr);
  if (expressionPlace.root != nullptr) {
    semanticModel.recordPlace(expr, placeKey(expressionPlace));
  }
  validateProjectedPlaceRead(expr);
  validateLoanPlaceAccess(expr, requestedRoles);
  Token token = expressionToken(expr);
  const ExprKind kind = expr.exprKind();
  SemanticBindingKind bindingKind = SemanticBindingKind::None;
  bool mutableBinding = false;
  bool staticMember = false;
  SymbolId symbolId = 0;
  bool enumeratorOccurrence = false;
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
      enumeratorOccurrence = true;
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
  } else if (kind == ExprKind::IndexSet) {
    const IndexSet &set = static_cast<const IndexSet &>(expr);
    roles = OccurrenceRole::Reference | OccurrenceRole::Write;
    if (set.oper().kind != TokenKind::EQUAL) {
      roles |= OccurrenceRole::Read;
    }
  } else if (kind == ExprKind::PlaceSet) {
    const PlaceSet &set = static_cast<const PlaceSet &>(expr);
    roles = OccurrenceRole::Reference | OccurrenceRole::Write;
    if (set.oper().kind != TokenKind::EQUAL)
      roles |= OccurrenceRole::Read;
    if (expressionPlace.root != nullptr) {
      bindingKind = expressionPlace.root->bindingKind;
      mutableBinding = expressionPlace.root->assignable;
      symbolId = toolingSymbolFor(*expressionPlace.root);
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
  if (isThreadLocalStorage(symbolId))
    semanticModel.recordDefinedFailure(expr,
                                       classifyDefinedFailure(expr, info));
  const bool crossesIndirection = std::any_of(
      expressionPlace.projections.begin(), expressionPlace.projections.end(),
      [](const PlaceProjection &projection) {
        return projection.kind == PlaceProjectionKind::Dereference;
      });
  const ForeignStorageInfo *foreign =
      crossesIndirection ? nullptr : semanticModel.findForeignStorage(symbolId);
  if (foreign == nullptr && !crossesIndirection &&
      expressionPlace.root != nullptr)
    foreign = semanticModel.findForeignStorage(
        toolingSymbolFor(*expressionPlace.root));
  if (foreign != nullptr) {
    const auto address =
        std::find(requestSession.foreignAddressPlaces.begin(),
                  requestSession.foreignAddressPlaces.end(), &expr);
    bool addressOnly = address != requestSession.foreignAddressPlaces.end();
    if (addressOnly && info.type.kind == SemanticType::RawPointer) {
      // Taking a pointer slot's address is inert. Computing the address of
      // one of its pointees first loads that slot, including through grouping
      // and inline record/array projections.
      auto addressBegin = address;
      while (addressBegin != requestSession.foreignAddressPlaces.begin() &&
             *(addressBegin - 1) != nullptr) {
        --addressBegin;
      }
      addressOnly = std::none_of(addressBegin, address, [](const Expr *parent) {
        return exprAs<Index>(parent) != nullptr;
      });
    }
    const bool transparent =
        kind == ExprKind::Grouping ||
        (kind == ExprKind::Binary &&
         static_cast<const Binary &>(expr).oper().kind == TokenKind::COMMA);
    if (!addressOnly && !transparent &&
        (semanticModel.unsafeOperation(expr) == UnsafeOperationKind::None ||
         semanticModel.unsafeOperation(expr) ==
             UnsafeOperationKind::ForeignStorage)) {
      requireUnsafe(expr, UnsafeOperationKind::ForeignStorage,
                    expressionToken(expr), "Access to foreign storage");
    }
  } else {
    recordProgramStorageEffect(expr, bindingKind, symbolId, roles);
    recordProgramBorrowStorageEffect(expr, roles);
  }
  if (hasRole(roles, OccurrenceRole::Write)) {
    if (expressionPlace.root != nullptr) {
      recordMutableBindingUse(toolingSymbolFor(*expressionPlace.root));
    }
    if (mutableBinding && bindingKind == SemanticBindingKind::LocalVariable) {
      recordMutableBindingUse(symbolId);
    }
  }
  std::vector<ProgramTemporaryCleanup> rollback;
  const auto *failure = semanticModel.findDefinedFailure(expr);
  if (failure && failure->mayUnwind() &&
      !requestSession.fullExpressionStates.empty() &&
      requestSession.fullExpressionStates.back().owner == currentProgramEffectOwner)
    rollback = requestSession.fullExpressionStates.back().temporaries;
  const auto operationOwner = currentProgramEffectOwner;
  const auto operationEdges =
      operationOwner.valid() ? programEffects[operationOwner].edges.size() : 0;
  const auto operationUnknown =
      operationOwner.valid() ? programEffects[operationOwner].unknown.size() : 0;
  recordProgramCallEffects(expr);
  if (operationOwner.valid() && failure && failure->mayUnwind()) {
    auto condition = std::make_shared<ProgramFailureCondition>();
    auto local = *failure;
    local.propagation = FailurePropagationKind::None;
    condition->direct = local.mayUnwind();
    auto &summary = programEffects[operationOwner];
    for (std::size_t edge = operationEdges; edge < summary.edges.size(); ++edge)
      if (!summary.edges[edge].cleanup)
        condition->targets.push_back(summary.edges[edge].target);
    for (std::size_t unknown = operationUnknown;
         unknown < summary.unknown.size(); ++unknown)
      condition->unknown |= !summary.unknown[unknown].cleanup;
    if (failure->propagation != FailurePropagationKind::None &&
        condition->targets.empty())
      condition->unknown = true;
    summary.directFailure |= condition->direct || condition->unknown;
    requestSession.programOperationFailures.insert_or_assign(&expr, condition);
    // Failure cleanup observes the pre-transfer temporary inventory, but its
    // exact selected call targets are now available. The condition closes only
    // after all source bodies and concrete effect queries have completed.
    recordProgramTemporaryFailureCleanup(expr, rollback);
  }
  if (info.category == ValueCategory::Value &&
      requiresActiveCleanupFor(info.type)) {
    recordProgramCleanupEffects(info.type, tokenSpan(expressionToken(expr)));
    applyRawCleanupWrites(info.type, tokenSpan(expressionToken(expr)),
                          rawPointerFields(&expr));
    const bool replacement =
        semanticModel.findOperator(expr) == nullptr &&
        (exprAs<Assign>(&expr) || exprAs<Set>(&expr) ||
         exprAs<IndexSet>(&expr) || exprAs<PlaceSet>(&expr) ||
         exprAs<DereferenceSet>(&expr));
    if (replacement)
      recordProgramReplacementCleanup(expr, info.type);
    else
      recordProgramTemporary(expr, info);
  }
  recordSemanticLoanUse(symbolId);
  if (!token.generated) {
    semanticModel.recordOccurrence(
        {.sourceUnit = currentSourceUnit,
         .span = tokenSpan(token),
         .kind = functionItemDeclaration != nullptr
                     ? SemanticOccurrenceKind::Function
                 : enumeratorOccurrence ? SemanticOccurrenceKind::Symbol
                                        : SemanticOccurrenceKind::Expression,
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
  if (fullExpression)
    finishProgramFullExpression(*fullExpression);
  if (evaluation.owner.valid() && evaluation.owner == currentProgramEffectOwner) {
    const auto &summary = programEffects[evaluation.owner];
    evaluation.edgeEnd = summary.edges.size();
    evaluation.unknownEnd = summary.unknown.size();
    evaluation.useEnd = summary.borrowUses.size();
    requestSession.programExpressionEffects.insert_or_assign(&expr, evaluation);
  }
  currentType = result;
  return result;
}

auto SemanticVisitor::Impl::recordMutableBindingUse(SymbolId symbol) -> void {
  if (symbol != 0) {
    requestSession.mutableBindingUses.insert(symbol);
  }
}

auto SemanticVisitor::Impl::recordMutableBindingUse(const ExprPtr &expression)
    -> void {
  if (!expression) {
    return;
  }
  const SemanticPlace place = semanticPlace(expression);
  if (place.root != nullptr) {
    recordMutableBindingUse(toolingSymbolFor(*place.root));
    return;
  }
  const SemanticLoanPlace loan = loanPlace(expression);
  recordMutableBindingUse(loan.root);
}

auto SemanticVisitor::Impl::finalizeAdvisoryDiagnostics() -> void {
  if (hadError() || instanceAnalysisActive) {
    return;
  }
  std::unordered_set<const Token *> reportedUnsafeBlocks;
  const auto &unsafeCandidates = requestSession.unsafeBlockCandidates;
  for (std::size_t index = lateFinalization == nullptr
                               ? 0
                               : lateFinalization->unsafeBlockCandidates;
       index < unsafeCandidates.size(); ++index) {
    const auto &candidate = unsafeCandidates[index];
    if (requestSession.usedUnsafeBlocks.contains(candidate.keyword) ||
        requestSession.deferredFlowOwners.contains(candidate.owner) ||
        !reportedUnsafeBlocks.insert(candidate.keyword).second) {
      continue;
    }
    Diagnostic diagnostic = makeDiagnostic(DiagnosticId::UnnecessaryUnsafeBlock,
                                           *candidate.keyword);
    diagnostic.tags.push_back(DiagnosticTag::Unnecessary);
    // Deleting just the keyword from an expression would leave invalid
    // brace syntax. The existing statement-only fix remains safe.
    if (candidate.expression) {
      diagnostic.hints.emplace_back(
          "Replace the unnecessary unsafe expression with parentheses.");
    } else {
      diagnostic.fixes.push_back({.span = tokenSpan(*candidate.keyword),
                                  .replacement = "",
                                  .message = "Remove unnecessary 'unsafe'"});
    }
    diagnostics.emplace_back(std::move(diagnostic));
  }
  const auto &mutableCandidates = requestSession.mutableLocalCandidates;
  for (std::size_t index = lateFinalization == nullptr
                               ? 0
                               : lateFinalization->mutableLocalCandidates;
       index < mutableCandidates.size(); ++index) {
    const RequestSessionState::MutableLocalCandidate &candidate =
        mutableCandidates[index];
    if (requestSession.mutableBindingUses.contains(candidate.symbol) ||
        requestSession.deferredFlowOwners.contains(candidate.owner)) {
      continue;
    }
    Diagnostic diagnostic =
        makeDiagnostic(DiagnosticId::UnnecessaryMutableBinding,
                       candidate.keyword, {candidate.name.lexeme});
    diagnostic.tags.push_back(DiagnosticTag::Unnecessary);
    diagnostic.fixes.push_back({.span = tokenSpan(candidate.keyword),
                                .replacement = "",
                                .message = "Remove unnecessary 'mut'"});
    diagnostics.emplace_back(std::move(diagnostic));
  }
}

auto SemanticVisitor::Impl::analyze(const ExprPtr &expr) -> SemanticType {
  return analyze(expr, OccurrenceRole::Reference | OccurrenceRole::Read);
}

auto SemanticVisitor::Impl::analyze(const ExprPtr &expr, OccurrenceRole roles)
    -> SemanticType {
  if (!expr)
    return SemanticType::Unknown;
  const auto result = analyze(*expr, roles);
  if (exprAs<Get>(expr.get()))
    if (const auto complete = completeBorrowDependencySet(expr))
      semanticModel.recordBorrowDependencySet(*expr, *complete);
  return result;
}

auto SemanticVisitor::Impl::analyzeExpectedCallableResult(
    const ExprPtr &expr, const SemanticType &expectedType,
    bool inferFromInstantiation) -> SemanticType {
  // A value expected as bool is tested for truth, unless an enclosing
  // context already gave it a use.
  std::optional<ResultUseScope> truthUse;
  if (expectedType == SemanticType::Bool && consultsResultUse(expr.get()) &&
      !resultUseFor(*expr))
    truthUse.emplace(
        *this, expr.get(),
        std::vector{operatorResultUseStep(OverloadedOperator::ContextualBool)},
        ResultUseAccess::ReadOnly);
  const Call *call = directCall(expr);
  if (call == nullptr) {
    return analyze(expr);
  }
  const std::optional<ContextualCallableResult> enclosingResult =
      contextualCallableResult;
  contextualCallableResult = ContextualCallableResult{
      .call = call,
      .type = expectedType,
      .inferResultFromInstantiation = inferFromInstantiation};
  const SemanticType result = analyze(expr);
  contextualCallableResult = enclosingResult;
  return result;
}

auto SemanticVisitor::Impl::analyzeInitializer(
    const ExprPtr &expr, const SemanticType &expectedType,
    bool allowExactNullConstruction, bool allowDeclarationConstruction)
    -> SemanticType {
  const auto fullExpression =
      expr ? beginProgramFullExpression(*expr) : std::nullopt;
  const std::optional<SemanticType> enclosingType = contextualInitializerType;
  const Expr *enclosingExpression = contextualInitializerExpression;
  contextualInitializerType = expectedType;
  contextualInitializerExpression = expr.get();
  // The value initializes the expected type, unless an enclosing context
  // already gave it a use.
  std::optional<ResultUseScope> initializedUse;
  if (expectedType != SemanticType::Unknown && consultsResultUse(expr.get()) &&
      !resultUseFor(*expr))
    initializedUse.emplace(*this, expr.get(),
                           std::vector{bindResultUseStep({expectedType})},
                           ResultUseAccess::Unknown);
  const auto literal =
      isIntegral(expectedType) && isContextualIntegerLiteral(expr.get())
          ? integerConstant(expr.get())
          : std::nullopt;
  SemanticType result =
      literal &&
              (!isInteger(expectedType) || integerFits(expectedType, *literal))
          ? analyzeContextualIntegerOperand(expr, expectedType)
          : analyzeExpectedCallableResult(expr, expectedType);
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
  } else if (allowDeclarationConstruction) {
    const bool construction =
        declarationConstructionApplies(expr, expectedType, result);
    if (declarationConversionApplies(expr, expectedType, result)) {
      result = analyzeDeclarationConversion(expr, expectedType, result,
                                            construction);
    } else if (construction) {
      result = analyzeDeclarationConstruction(expr, expectedType, result);
    }
  }
  contextualInitializerType = enclosingType;
  contextualInitializerExpression = enclosingExpression;
  if (fullExpression)
    finishProgramFullExpression(*fullExpression);
  return result;
}

auto SemanticVisitor::Impl::declarationConstructionApplies(
    const ExprPtr &expr, const SemanticType &declaredType,
    const SemanticType &initializerType) -> bool {
  if (!expr || declaredType.kind != SemanticType::Class ||
      declaredType.classId == 0 || declaredType.classId > classes.size() ||
      initializerType == SemanticType::Unknown ||
      initializerType == declaredType) {
    return false;
  }
  // A union's initializer selects a member rather than a constructor, an
  // abstract class has no complete object. Conditional borrowed results use
  // a separately proved synthetic result, preserving the original argument.
  const ClassInfo &owner = classInfo(declaredType.classId);
  if (owner.kind == ClassKind::Union || owner.abstract) {
    return false;
  }
  if (typeTraits(declaredType).containsBorrowedState) {
    const auto inventory = storedReferenceInventory(declaredType);
    if (!inventory ||
        std::none_of(inventory->begin(), inventory->end(),
                     [](const auto &path) { return !path.conditions.empty(); }))
      return false;
  }
  // An initializer typed by its destination has no type of its own with which
  // to select a constructor.
  if (isContextualArrayArgument(expr) ||
      isContextualCallableResultArgument(expr) ||
      isContextualNativeFunctionArgument(expr)) {
    return false;
  }
  return !isOwnershipAssignable(declaredType, initializerType, expr);
}

auto SemanticVisitor::Impl::analyzeDeclarationConstruction(
    const ExprPtr &expr, const SemanticType &declaredType,
    const SemanticType &initializerType) -> SemanticType {
  // The declaration selects the constructor `T name{initializer}` would: one
  // exact parameter list with the initializer as the sole argument.
  semanticModel.forgetContextualConstruction(*expr);
  const std::size_t diagnosticStart = diagnostics.size();
  const SourceSpan site = tokenSpan(expressionToken(expr));
  analyzeConstructorCallArguments(
      *expr, declaredType.classId, declaredType.arguments,
      declaredType.valueArguments,
      {{.type = initializerType, .expression = &expr}}, expressionToken(expr),
      false, true);
  if (!semanticModel.recordsContextualConstruction(*expr)) {
    // No constructor takes exactly this argument: the declaration keeps its
    // exact-type initialization error. Any other rejection, such as an
    // ambiguity or a required move, is the selection's own diagnostic.
    if (diagnostics.size() == diagnosticStart + 1 &&
        diagnostics.back().code == "GTI-S2012" &&
        diagnostics.back().primary == site) {
      diagnostics.pop_back();
    }
    return diagnostics.size() == diagnosticStart ? initializerType
                                                 : SemanticType::Unknown;
  }
  const ResolvedConstructionInfo &construction =
      *semanticModel.findContextualConstruction(*expr);
  if (construction.kind == ConstructorKind::Move ||
      construction.directInitialization ||
      (!construction.parameterTypes.empty() &&
       construction.parameterTypes.front().kind != SemanticType::Reference))
    consumeProgramTemporary(expr);
  if (currentProgramEffectOwner.valid()) {
    auto check = captureProgramStorageBorrows(site);
    if (!construction.parameterTypes.empty())
      appendProgramStorageBorrowInput(
          check, expr,
          construction.parameterTypes.front().kind == SemanticType::Reference
              ? std::optional(
                    construction.parameterTypes.front().referenceAccess)
              : std::nullopt);
    const auto owner = currentProgramEffectOwner;
    const auto edgeStart = programEffects[owner].edges.size();
    const auto unknownStart = programEffects[owner].unknown.size();
    if (!construction.parameterTypes.empty() &&
        (construction.parameterTypes.front().kind == SemanticType::Reference ||
         construction.parameterTypes.front().kind ==
             SemanticType::RawPointer)) {
      forceProgramStorageBoundaryAccess(expr);
    }
    static_cast<void>(recordProgramConstructionEffects(*expr, construction, 1));
    publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                     unknownStart);
  }
  // The constructed value is the declaration's own, so its cleanup belongs
  // to the enclosing effects exactly as a direct initializer's does.
  if (requiresActiveCleanupFor(construction.constructedType)) {
    recordProgramCleanupEffects(construction.constructedType, site);
    applyRawCleanupWrites(
        construction.constructedType, site,
        contextualConstructionRawFields(*expr, construction.constructedType));
  }
  return construction.constructedType;
}

auto SemanticVisitor::Impl::declarationConversionApplies(
    const ExprPtr &expr, const SemanticType &declaredType,
    const SemanticType &initializerType) -> bool {
  if (!expr || initializerType.kind != SemanticType::Class ||
      declaredType == SemanticType::Unknown ||
      declaredType.kind == SemanticType::Reference ||
      declaredType == initializerType ||
      isOwnershipAssignable(declaredType, initializerType, expr)) {
    return false;
  }
  return !conversionOperatorCandidates(initializerType, declaredType, true)
              .empty();
}

auto SemanticVisitor::Impl::analyzeDeclarationConversion(
    const ExprPtr &expr, const SemanticType &declaredType,
    const SemanticType &initializerType, bool constructionApplies)
    -> SemanticType {
  // The declaration converts as `Target(initializer)` would, through implicit
  // operators only. An implicit constructor of the declared type that also
  // takes the initializer is a second path, which makes it ambiguous.
  semanticModel.forgetContextualConversion(*expr);
  const Token site = expressionToken(expr);
  if (const std::vector<const ConstructorInfo *> constructors =
          constructionApplies
              ? conversionConstructorCandidates(*expr, expr, initializerType,
                                                declaredType, site, true)
              : std::vector<const ConstructorInfo *>{};
      !constructors.empty()) {
    reportAmbiguousConversion(
        site, initializerType, declaredType,
        conversionOperatorCandidates(initializerType, declaredType, true),
        constructors, true);
    return SemanticType::Unknown;
  }
  auto check = captureProgramStorageBorrows(tokenSpan(site));
  const auto owner = currentProgramEffectOwner;
  const auto edgeStart = programEffects[owner].edges.size();
  const auto unknownStart = programEffects[owner].unknown.size();
  if (!selectConversionOperator(*expr, expr, initializerType, declaredType,
                                site, true) ||
      !semanticModel.recordsContextualConversion(*expr)) {
    return SemanticType::Unknown;
  }
  if (owner.valid()) {
    const auto *operation = semanticModel.findContextualConversion(*expr);
    appendProgramStorageBorrowInput(check, expr,
                                    operation->receiverMutability ==
                                            ReceiverMutability::ReadOnly
                                        ? AccessMode::ReadOnly
                                        : AccessMode::Mutable);
    publishProgramStorageBorrowCheck(std::move(check), owner, edgeStart,
                                     unknownStart);
  }
  // The converted value is the declaration's own, so its cleanup belongs to
  // the enclosing effects exactly as a constructed initializer's does.
  if (requiresActiveCleanupFor(declaredType)) {
    const SourceSpan span = tokenSpan(site);
    recordProgramCleanupEffects(declaredType, span);
    applyRawCleanupWrites(declaredType, span,
                          contextualConstructionRawFields(*expr, declaredType));
  }
  return declaredType;
}

auto SemanticVisitor::Impl::contextualConstructionRawFields(
    const Expr &initializer, const SemanticType &type)
    -> std::vector<ProjectedRawPointerOrigins> {
  // An ordinary source constructor selected for a contextual construction is
  // queried like any other constructor call: the written expression is its
  // single actual argument, and the constructor body's recorded raw fields
  // name which parameter (or fresh address) each stored pointer came from.
  if (const auto *contextual =
          rawAuditEnabled()
              ? semanticModel.findContextualConstruction(initializer)
              : nullptr;
      contextual != nullptr && contextual->constructor != 0 &&
      contextual->kind == ConstructorKind::Ordinary &&
      contextual->declaration != nullptr &&
      contextual->declaration->body() != nullptr &&
      contextual->constructedType == type &&
      contextual->parameterTypes.size() == 1 &&
      !containsOpenTypeParameter(type))
    return rawProducedFields(initializer, nullptr, nullptr, contextual);
  // A conversion operator or an unqueryable constructor may store any address
  // it receives or creates: every raw field stays unresolved.
  std::vector<ProjectedRawPointerOrigins> fields;
  for (auto path : rawPointerFieldPaths(type)) {
    mergeRawPointerFields(
        fields,
        {{std::move(path),
          {{.addressSite = tokenSpan(expressionToken(initializer)),
            .sourceResultQuery = std::numeric_limits<std::size_t>::max()}}}});
  }
  return fields;
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

auto SemanticVisitor::Impl::isOptionalLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/optional";
}

auto SemanticVisitor::Impl::isPolymorphicLibraryUnit(
    SourceUnitId sourceUnit) const -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/polymorphic";
}

auto SemanticVisitor::Impl::isMetaLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/meta";
}

auto SemanticVisitor::Impl::isSourceLocationLibraryUnit(
    SourceUnitId sourceUnit) const -> bool {
  if (sourceGraph == nullptr) {
    return false;
  }
  const SourceUnit *unit = sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/source_location";
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

auto SemanticVisitor::Impl::isSimdLibraryUnit(SourceUnitId sourceUnit) const
    -> bool {
  const SourceUnit *unit =
      sourceGraph == nullptr ? nullptr : sourceGraph->findUnit(sourceUnit);
  return unit != nullptr && unit->role == SourceUnitRole::StandardLibrary &&
         unit->standardLibraryName == "std/simd";
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
  return isDefaultLibraryMutexGuardCarrier(owner);
}

auto SemanticVisitor::Impl::isDefaultLibraryMutexGuardCarrier(
    const ClassInfo &owner) const -> bool {
  if (!isMutexLibraryUnit(owner.sourceUnit) || !owner.storedReference ||
      owner.fields.size() != 2 ||
      owner.storedReference->type.kind != SemanticType::Reference ||
      owner.storedReference->type.referenceAccess != AccessMode::ReadOnly ||
      owner.storedReference->type.arguments.size() != 1) {
    return false;
  }
  const ClassInfo *storage =
      classInfo(owner.storedReference->type.arguments.front());
  if (storage == nullptr ||
      storage->compilerCapability != CompilerCapabilityTypeKind::MutexStorage ||
      storage->sourceUnit != owner.sourceUnit) {
    return false;
  }
  for (const FieldInfo &field : owner.fields) {
    if (field.declaration == nullptr ||
        field.declaration == owner.storedReference->field) {
      continue;
    }
    const auto member = owner.members.find(field.declaration->name().lexeme);
    if (member == owner.members.end()) {
      continue;
    }
    const ClassInfo *token = classInfo(member->second.symbol.type);
    if (token != nullptr && token->sourceUnit == owner.sourceUnit &&
        token->compilerCapability ==
            CompilerCapabilityTypeKind::MutexLockToken &&
        member->second.symbol.type.arguments ==
            owner.storedReference->type.arguments.front().arguments) {
      return true;
    }
  }
  return false;
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
       .expansion = name.expansion,
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
      name.generated ? std::string_view{name.lexeme} : std::string_view{},
      name.expansion);
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
  if (info != nullptr && info->declaration != &declaration)
    return recordFunctionSymbol(*info->declaration);
  // A conversion operator's target tokens are type references of their own;
  // its declaration is the `operator` keyword.
  const Token &name =
      !declaration.operatorName() ? declaration.name()
      : declaration.operatorName()->kind == OverloadedOperator::Conversion
          ? declaration.operatorName()->keyword
          : declaration.operatorName()->symbol;
  const bool method = info != nullptr && info->ownerClass != 0;
  const SymbolKind kind =
      declaration.operatorName()
          ? SymbolKind::Operator
          : (method ? SymbolKind::Method : SymbolKind::Function);
  const SymbolId id = recordToolingSymbol(
      name, kind, info == nullptr ? name.lexeme : info->qualifiedName,
      info == nullptr ? SemanticType::Unknown : info->returnType, false,
      declaration.body() != nullptr, functionAccess(info),
      info != nullptr && info->staticMember,
      info != nullptr && info->internalLinkage,
      info != nullptr && info->compilerPrivate);
  if (info != nullptr && info->definition != nullptr) {
    if (const auto *existing = semanticModel.database().findSymbol(id)) {
      auto symbol = *existing;
      symbol.definitionSpan = tokenSpan(info->definition->name());
      semanticModel.recordSymbol(std::move(symbol));
    }
  }
  return id;
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
  for (const EnumInfo &owner : enums) {
    const auto found = std::find_if(
        owner.genericParameters.begin(), owner.genericParameters.end(),
        [id](const GenericParameterInfo &parameter) {
          return parameter.id == id;
        });
    if (found != owner.genericParameters.end()) {
      return &*found;
    }
  }
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
  if (analyzingDependentConstantCondition) {
    constantConditionGenericDependency = true;
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
  if (type.qualifiedOwner) {
    recordTypeUse(*type.qualifiedOwner);
  } else {
    recordQualifiedPathUses(type.name);
  }

  SymbolId symbol = 0;
  SemanticType resolvedType = typeOf(type);
  if (const std::optional<SemanticType> parameter =
          resolveTypeParameter(type.name)) {
    // A substitution can itself still be symbolic, such as a callable-result
    // query over an open parameter; the condition then stays dependent.
    if (analyzingDependentConstantCondition &&
        containsOpenTypeParameter(*parameter)) {
      constantConditionGenericDependency = true;
    }
    const GenericParameterInfo *info =
        genericParameterInfo(parameter->genericParameterId);
    if (info != nullptr) {
      symbol = symbolForDeclaration(info->name);
    }
  } else if (const std::optional<ResolvedTypeAlias> alias =
                 resolveTypeAliasPath(type, currentNamespace)) {
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
  // Each written type qualifier has its own declaration identity. Recording
  // only the immediate owner loses navigation and rename for Outer in
  // Outer::Inner::Leaf, even though lookup selected all three types.
  for (std::size_t index = resolvedSegments.size();
       index + 1 < path.segments.size(); ++index) {
    const NamePath ownerPath(std::vector<Token>(
        path.segments.begin(), path.segments.begin() + index + 1));
    SymbolId symbol = 0;
    SemanticType ownerType;
    if (const auto alias = resolveTypeAliasPath(ownerPath, currentNamespace)) {
      symbol = symbolForDeclaration(alias->declaration->name());
      ownerType = alias->type;
      if (symbol == 0) {
        symbol = recordToolingSymbol(alias->declaration->name(),
                                     SymbolKind::TypeAlias,
                                     alias->qualifiedName, ownerType);
      }
    } else if (const auto classId =
                   resolveClassPath(ownerPath, currentNamespace)) {
      const ClassInfo &owner = classInfo(*classId);
      ownerType = SemanticType::classType(owner.id);
      symbol = symbolForDeclaration(owner.name);
      if (symbol == 0) {
        symbol = recordToolingSymbol(
            owner.name,
            owner.kind == ClassKind::Struct ? SymbolKind::Struct
                                            : SymbolKind::Class,
            qualifiedName(owner.namespaceScope, owner.name.lexeme), ownerType);
      }
    } else if (const auto enumId = resolveEnumPath(ownerPath, currentNamespace);
               enumId && *enumId != 0 && *enumId <= enums.size()) {
      const EnumInfo &owner = enums[*enumId - 1];
      ownerType = SemanticType::enumType(owner.id);
      symbol = symbolForDeclaration(owner.name);
      if (symbol == 0) {
        symbol = recordToolingSymbol(
            owner.name, SymbolKind::Enum,
            qualifiedName(owner.namespaceScope, owner.name.lexeme), ownerType);
      }
    }
    if (symbol != 0) {
      semanticModel.recordOccurrence(
          {.sourceUnit = currentSourceUnit,
           .span = tokenSpan(ownerPath.last()),
           .kind = SemanticOccurrenceKind::Symbol,
           .symbol = symbol,
           .roles = OccurrenceRole::Reference | OccurrenceRole::TypeUse,
           .name = ownerPath.last().lexeme,
           .type = ownerType,
           .traits = typeTraits(ownerType)});
    }
  }
}

auto SemanticVisitor::Impl::recordBindingOccurrence(
    const Token &name, const SemanticType &type, bool mutableBinding,
    SemanticBindingKind bindingKind, bool staticMember, bool internalLinkage,
    bool instanceDependentType, bool threadLocal) -> SymbolId {
  const AccessMode access =
      mutableBinding ? AccessMode::Mutable : AccessMode::ReadOnly;
  const SymbolId symbol =
      recordBindingSymbol(name, type, mutableBinding, bindingKind, 0, {},
                          staticMember, internalLinkage, instanceDependentType);
  if (threadLocal)
    if (const auto *existing = semanticModel.database().findSymbol(symbol)) {
      auto updated = *existing;
      updated.threadLocal = true;
      semanticModel.recordSymbol(std::move(updated));
    }
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

auto SemanticVisitor::Impl::recordParameterBinding(const Parameter &parameter,
                                                   const SemanticType &type)
    -> SymbolId {
  Token binder = parameter.name;
  if (binder.lexeme.empty()) {
    binder = parameter.sourceAnchor();
    binder.lexeme = "__gti_unnamed_parameter";
    binder.generated = true;
  }
  return recordBindingOccurrence(binder, type,
                                 parameter.mutability == Mutability::Mutable,
                                 SemanticBindingKind::Parameter);
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
      (left.kind == SemanticType::TypeParameter ||
       (left.kind == SemanticType::TypePack && !left.concretePack))
          ? genericParameterIndex(leftParameters, left.genericParameterId)
          : std::nullopt;
  const std::optional<std::size_t> rightParameter =
      (right.kind == SemanticType::TypeParameter ||
       (right.kind == SemanticType::TypePack && !right.concretePack))
          ? genericParameterIndex(rightParameters, right.genericParameterId)
          : std::nullopt;
  if (leftParameter || rightParameter) {
    return left.kind == right.kind && leftParameter && rightParameter &&
           *leftParameter == *rightParameter;
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
  if (left.kind != right.kind || left.nativeCScalar != right.nativeCScalar ||
      left.classId != right.classId ||
      left.genericParameterId != right.genericParameterId ||
      left.functionId != right.functionId ||
      left.arrayLength != right.arrayLength ||
      (!leftExtent && !rightExtent &&
       left.arrayLengthParameterId != right.arrayLengthParameterId) ||
      left.referenceAccess != right.referenceAccess ||
      left.pointerAccess != right.pointerAccess ||
      left.signatureReceiver != right.signatureReceiver ||
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
  // A conversion operator's target is part of its identity, although return
  // types never distinguish ordinary overloads. Resolved types compare, so
  // alias spellings of one target are one conversion.
  if (isConversionOperator(left) && isConversionOperator(right) &&
      !sameSignatureType(left.returnType, left.record().genericParameters,
                         right.returnType, right.record().genericParameters)) {
    return false;
  }
  const auto unresolvedPack = [](const CallableCandidate &candidate) {
    return candidate.parameterPack &&
           (candidate.parameterTypes.empty() ||
            !candidate.parameterTypes.back().concretePack);
  };
  const auto expandedParameters = [](const CallableCandidate &candidate) {
    std::vector<SemanticType> parameters;
    for (const auto &parameter : candidate.parameterTypes)
      SemanticType::appendExpandedArgument(parameters, parameter);
    return parameters;
  };
  // Owner substitution may have resolved a base's final pack, including an
  // empty pack. Overrides compare the resulting callable signature, not the
  // number of written parameter groups. Symbolic packs remain distinct.
  const auto leftParameters = expandedParameters(left);
  const auto rightParameters = expandedParameters(right);
  const bool receiverQualified = left.ownerClass != 0 && right.ownerClass != 0;
  if ((receiverQualified &&
       left.receiverMutability != right.receiverMutability) ||
      unresolvedPack(left) != unresolvedPack(right) ||
      left.record().genericParameters.size() !=
          right.record().genericParameters.size() ||
      leftParameters.size() != rightParameters.size()) {
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
  for (std::size_t index = 0; index < leftParameters.size(); ++index) {
    if (!sameSignatureType(
            leftParameters[index], left.record().genericParameters,
            rightParameters[index], right.record().genericParameters)) {
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
    if (previousC || candidateC) {
      // The external-symbol catalog owns duplicate C symbol diagnostics,
      // including this same-name case. Keep the GTI overload rule for mixed
      // linkage and distinct native link names.
      if (previousC && candidateC &&
          previous.record().externalSymbol ==
              candidate.record().externalSymbol) {
        return true;
      }
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
          isConversionOperator(candidate)
              ? "Duplicate conversion operator to '" +
                    typeSpelling(candidate.returnType) +
                    "' with the same receiver."
              : "Duplicate overload signature for '" + name.lexeme + "'.");
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
  if (symbol.parameterDeclaration) {
    const auto place = placeKey(placeForSymbol(symbol));
    if (place.valid()) {
      for (auto path : rawPointerFieldPaths(symbol.type)) {
        auto originPlace = place;
        originPlace.projections.insert(originPlace.projections.end(),
                                       path.begin(), path.end());
        RawPointerOrigin origin{.place = std::move(originPlace),
                                .addressSite = tokenSpan(name),
                                .parameterPointee = true};
        if (path.empty() && symbol.type.kind == SemanticType::RawPointer)
          symbol.rawPointerOrigins.push_back(std::move(origin));
        else
          symbol.rawPointerFields.push_back(
              {std::move(path), {std::move(origin)}});
      }
    }
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
  if (symbol.variableDeclaration != nullptr && !instanceAnalysisActive) {
    symbol.declarationOrder = currentDeclarationOrder;
  }
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
        declarationVisible(declarationSourceUnit(external->second),
                           external->second.declaration)) {
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
    bool mutableReceiver, const SemanticType &receiverType) const -> void {
  if (name.rfind("__gti_", 0) == 0 ||
      symbol.valueState != ValueState::Available ||
      (!isCompilerTrustedUnit(currentSourceUnit) &&
       (symbol.compilerPrivate ||
        semanticModel.isCompilerPrivateType(symbol.type)))) {
    return;
  }
  if (currentStaticMemberFunction && receiverType == SemanticType::Unknown &&
      symbol.ownerClass != 0 && !symbol.staticMember) {
    return;
  }
  if (symbol.type != SemanticType::Function &&
      !canAccessMember(symbol.access, symbol.ownerClass,
                       symbol.staticMember ? SemanticType::Unknown
                                           : receiverType))
    return;
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
      if (!canAccessMember(overload.access, overload.ownerClass,
                           overload.staticMember ? SemanticType::Unknown
                                                 : receiverType,
                           overload.dispatchOwner)) {
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
      .threadLocal = symbol.variableDeclaration != nullptr &&
                     symbol.variableDeclaration->isThreadLocal(),
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
  for (ClassId ownerId = currentClass.value_or(0); ownerId != 0;
       ownerId = classInfo(ownerId).enclosingClass) {
    const ClassInfo &owner = classInfo(ownerId);
    for (const auto &[name, nestedId] : owner.nestedTypes) {
      const ClassInfo &nested = classInfo(nestedId);
      if ((!isCompilerTrustedUnit(currentSourceUnit) &&
           nested.compilerPrivate) ||
          !seen.insert(name).second) {
        continue;
      }
      context.candidates.push_back(
          {.kind = nested.kind == ClassKind::Struct
                       ? SemanticCompletionCandidateKind::Struct
                       : SemanticCompletionCandidateKind::Class,
           .name = name,
           .qualifiedName = qualifiedName(nested.namespaceScope, name),
           .type = SemanticType::classType(nested.id),
           .scopeDistance = distance,
           .classType = nested.id,
           .symbol = symbolForDeclaration(nested.name),
           .compilerPrivate = nested.compilerPrivate});
    }
    for (const auto &[name, alias] : owner.typeAliases) {
      if (!seen.insert(name).second ||
          (!canAccessMember(alias.access, alias.ownerClass)) ||
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
  std::erase_if(context.candidates, [&](const auto &candidate) {
    for (ClassId id = candidate.classType; id != 0;
         id = classInfo(id).enclosingClass) {
      const ClassInfo &nested = classInfo(id);
      if (nested.enclosingClass != 0 &&
          !canAccessMember(nested.enclosingAccess, nested.enclosingClass)) {
        return true;
      }
    }
    return false;
  });
  semanticModel.recordCompletion(std::move(context));
}

auto SemanticVisitor::Impl::captureQualifiedCompletion(const NamePath &path,
                                                       const TypeRef *ownerType,
                                                       bool typeOnly) -> void {
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
  std::optional<ClassId> completionOwner =
      resolveClassPath(ownerPath, currentNamespace);
  std::optional<SemanticType> specializedOwner;
  if (ownerType) {
    const SemanticType selected = typeOf(*ownerType);
    if (selected.kind == SemanticType::Class) {
      completionOwner = selected.classId;
      specializedOwner = selected;
    }
  }
  if (!completionOwner) {
    if (const auto alias = resolveTypeAliasPath(ownerPath, currentNamespace)) {
      if (!validateTypeAliasAccess(*alias, ownerPath.last())) {
        semanticModel.recordCompletion(std::move(context));
        return;
      }
      const SemanticType selected = substituteType(
          alias->type,
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
      if (selected.kind == SemanticType::Class && selected.classId != 0 &&
          selected.classId <= classes.size()) {
        completionOwner = selected.classId;
        specializedOwner = selected;
      }
    }
  }
  const SemanticType selectedEnum =
      typeOf(ownerType ? *ownerType : TypeRef(ownerPath));
  if (selectedEnum.kind == SemanticType::Enum && selectedEnum.enumId != 0 &&
      selectedEnum.enumId <= enums.size() && enumTypeFor(selectedEnum)) {
    context.kind = SemanticCompletionKind::Enum;
    const EnumInfo &enumeration = enums.at(selectedEnum.enumId - 1);
    for (const auto &[name, enumerator] : enumeration.enumerators) {
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::Enumerator,
           .name = name,
           .qualifiedName = qualifiedName(enumeration.namespaceScope,
                                          enumeration.name.lexeme) +
                            "::" + name,
           .type = selectedEnum,
           .scopeDistance = 0,
           .enumType = selectedEnum.enumId});
    }
  } else if (const std::optional<ClassId> owner = completionOwner) {
    context.kind = SemanticCompletionKind::Member;
    const ClassInfo &classType = classInfo(*owner);
    for (ClassId id = *owner; id != 0; id = classInfo(id).enclosingClass) {
      const ClassInfo &nested = classInfo(id);
      if ((!isCompilerTrustedUnit(currentSourceUnit) &&
           nested.compilerPrivate) ||
          (nested.enclosingClass != 0 &&
           !canAccessMember(nested.enclosingAccess, nested.enclosingClass))) {
        semanticModel.recordCompletion(std::move(context));
        return;
      }
    }
    for (const auto &[name, nestedId] : classType.nestedTypes) {
      const ClassInfo &nested = classInfo(nestedId);
      if ((!canAccessMember(nested.enclosingAccess, *owner)) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           nested.compilerPrivate)) {
        continue;
      }
      context.candidates.push_back(
          {.kind = nested.kind == ClassKind::Struct
                       ? SemanticCompletionCandidateKind::Struct
                       : SemanticCompletionCandidateKind::Class,
           .name = name,
           .qualifiedName = qualifiedName(nested.namespaceScope, name),
           .type = SemanticType::classType(nested.id),
           .scopeDistance = 0,
           .classType = nested.id,
           .symbol = symbolForDeclaration(nested.name),
           .compilerPrivate = nested.compilerPrivate});
    }
    const GenericSubstitution substitution =
        specializedOwner ? classSubstitution(*specializedOwner)
                         : GenericSubstitution{};
    const auto candidateDetail = [&](const std::string &name,
                                     const SemanticType &type) {
      return specializedOwner ? "using " +
                                    (ownerType ? typeRefSpelling(*ownerType)
                                               : pathSpelling(ownerPath)) +
                                    "::" + name + " = " + typeSpelling(type)
                              : std::string{};
    };
    std::unordered_set<std::string> seenTypeAliases;
    for (const auto &[name, alias] : classType.typeAliases) {
      const SemanticType selectedType =
          substituteType(alias.type, substitution);
      if ((!canAccessMember(
              alias.access, alias.ownerClass, SemanticType::Unknown,
              specializedOwner.value_or(openClassType(*owner)))) ||
          (!specializedOwner && !classType.genericParameters.empty() &&
           currentClass != *owner) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           (alias.compilerPrivate ||
            semanticModel.isCompilerPrivateType(selectedType)))) {
        continue;
      }
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::TypeAlias,
           .name = name,
           .qualifiedName = qualifiedName(classType.namespaceScope,
                                          classType.name.lexeme + "::" + name),
           .detail = candidateDetail(name, selectedType),
           .type = selectedType,
           .scopeDistance = 0,
           .typeAlias = alias.declaration,
           .symbol = symbolForDeclaration(alias.declaration->name()),
           .compilerPrivate = alias.compilerPrivate});
      seenTypeAliases.insert(name);
    }
    for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
      const RegisteredTypeAlias &alias = typeAliases[id - 1];
      const SemanticType selectedType =
          substituteType(alias.type, substitution);
      if (alias.ownerClass != *owner ||
          alias.resolution != TypeAliasResolution::Resolved ||
          !seenTypeAliases.insert(alias.declaration->name().lexeme).second ||
          (!canAccessMember(
              alias.access, alias.ownerClass, SemanticType::Unknown,
              specializedOwner.value_or(openClassType(*owner)))) ||
          (!specializedOwner && !classType.genericParameters.empty() &&
           currentClass != *owner) ||
          (!isCompilerTrustedUnit(currentSourceUnit) &&
           (alias.compilerPrivate ||
            semanticModel.isCompilerPrivateType(selectedType)))) {
        continue;
      }
      context.candidates.push_back(
          {.kind = SemanticCompletionCandidateKind::TypeAlias,
           .name = alias.declaration->name().lexeme,
           .qualifiedName = alias.qualifiedName,
           .detail =
               candidateDetail(alias.declaration->name().lexeme, selectedType),
           .type = selectedType,
           .scopeDistance = 0,
           .typeAlias = alias.declaration,
           .symbol = symbolForDeclaration(alias.declaration->name()),
           .compilerPrivate = alias.compilerPrivate});
    }
    for (const auto &[name, member] : classType.members) {
      // Alias qualification currently names type members, not static values.
      // Do not advertise a spelling that ordinary symbol lookup cannot use.
      if (typeOnly || (specializedOwner && !ownerType) ||
          !member.symbol.staticMember ||
          (member.symbol.type != SemanticType::Function &&
           !canAccessMember(
               member.access, member.symbol.ownerClass, SemanticType::Unknown,
               specializedOwner.value_or(openClassType(*owner))))) {
        continue;
      }
      appendSymbolCompletions(
          context.candidates, name,
          qualifiedName(classType.namespaceScope,
                        classType.name.lexeme + "::" + name),
          specializedOwner ? substituteSymbol(member.symbol, *specializedOwner)
                           : member.symbol,
          0);
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
        if (member.symbol.type != SemanticType::Function &&
            !canAccessMember(member.access, member.symbol.ownerClass,
                             objectType)) {
          continue;
        }
        const Symbol substituted = substituteSymbol(member.symbol, objectType);
        appendSymbolCompletions(context.candidates, name, name, substituted, 0,
                                true, mutableReceiver, objectType);
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
    for (const auto name : {"begin", "end", "cbegin", "cend", "end_iterator",
                            "iterator_at", "rbegin", "rend", "crbegin", "crend",
                            "rend_iterator", "reverse_iterator_at"}) {
      if (const auto factory = textViewMemberFactory(name)) {
        context.candidates.push_back(
            {.kind = SemanticCompletionCandidateKind::Method,
             .name = name,
             .qualifiedName = name,
             .detail = typeSpelling(factory->returnType) + " " + name +
                       (factory->parameterTypes.size() == 1
                            ? "()"
                            : "(std::size_t position)"),
             .scopeDistance = 0});
      }
    }
  }
  semanticModel.recordCompletion(std::move(context));
}

auto SemanticVisitor::Impl::resolveGeneratedTestReference(
    const NamePath &path) const -> const Symbol * {
  if (sourceGraph == nullptr || currentFunctionDeclaration == nullptr ||
      !currentFunctionDeclaration->generatedTestEntry())
    return nullptr;
  std::string qualified;
  for (const Token &segment : path.segments) {
    if (!qualified.empty())
      qualified += "::";
    qualified += segment.lexeme;
  }
  const auto &symbols = activeNamespaceSymbols();
  const auto found = symbols.find(qualified);
  if (found == symbols.end() || found->second.type != SemanticType::Function)
    return nullptr;
  const SourceUnit *caller = sourceGraph->findUnit(currentSourceUnit);
  for (const CallableCandidate &candidate : found->second.overloads) {
    const CallableDeclaration &function = candidate.record();
    const SourceUnit *callee = sourceGraph->findUnit(function.sourceUnit);
    if (function.testFunction && !function.internalLinkage &&
        caller != nullptr && callee != nullptr &&
        !sourceGraph->isCompilerTrusted(callee->id) &&
        caller->packageIdentity == callee->packageIdentity)
      return &found->second;
  }
  return nullptr;
}

auto SemanticVisitor::Impl::resolve(const Token &name) const -> const Symbol * {
  if (const Symbol *test = resolveGeneratedTestReference(NamePath(name)))
    return test;
  for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
    if (const auto found = scope->find(name.lexeme); found != scope->end()) {
      return &found->second;
    }
  }

  const auto &symbols = currentNamespaceSymbols();
  if (const auto id = resolveClassPath(NamePath(name), currentNamespace)) {
    const auto &type = classInfo(*id);
    if (type.enclosingClass != 0) {
      const auto found =
          symbols.find(qualifiedName(type.namespaceScope, type.name.lexeme));
      if (found != symbols.end())
        return &found->second;
    }
  }
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto found =
        symbols.find(qualifiedName(currentNamespace, depth - 1, name.lexeme));
    if (found != symbols.end() && symbolVisibleNow(found->second)) {
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
    // Inside its class, an unqualified static member names the same storage
    // as its qualified spelling.
    return staticFieldFlowSymbol(*local);
  }
  std::unordered_map<std::string, Symbol> *symbols =
      currentNamespaceSymbolsMutable();
  if (symbols == nullptr) {
    return nullptr;
  }
  for (std::size_t depth = currentNamespace.size() + 1; depth > 0; --depth) {
    const auto found =
        symbols->find(qualifiedName(currentNamespace, depth - 1, name.lexeme));
    if (found != symbols->end() && symbolVisibleNow(found->second)) {
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
  if (const Symbol *test = resolveGeneratedTestReference(path))
    return test;
  if (path.segments.size() < 2) {
    return resolve(path.last());
  }
  if (const auto id = resolveClassPath(path, currentNamespace)) {
    const auto &type = classInfo(*id);
    if (type.enclosingClass != 0) {
      const auto &symbols = currentNamespaceSymbols();
      const auto found =
          symbols.find(qualifiedName(type.namespaceScope, type.name.lexeme));
      if (found != symbols.end())
        return &found->second;
    }
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
  if (symbol != symbols.end() && symbolVisibleNow(symbol->second)) {
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
               ? staticFieldFlowSymbol(member->second.symbol)
               : nullptr;
  }

  std::unordered_map<std::string, Symbol> *symbols =
      currentNamespaceSymbolsMutable();
  if (symbols == nullptr) {
    return nullptr;
  }
  const auto symbol =
      symbols->find(*resolvedNamespace + "::" + path.last().lexeme);
  return symbol == symbols->end() || !symbolVisibleNow(symbol->second)
             ? nullptr
             : &symbol->second;
}

auto SemanticVisitor::Impl::staticFieldFlowSymbol(Symbol &member) -> Symbol * {
  if (!catalog.sealed ||
      member.bindingKind != SemanticBindingKind::StaticField ||
      member.variableDeclaration == nullptr)
    return &member;
  // beginBody carries every catalog static field; one first seen later gets
  // its carrier here. A source-result worker describes the storage as a
  // symbolic input, exactly as it seeds namespace globals.
  const auto [found, created] = valueFlow.staticFieldSymbols.try_emplace(
      member.variableDeclaration, member);
  if (created && requestSession.collectingRawResult)
    seedRawStorageInput(found->second);
  return &found->second;
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
      declarationUnit == 0 ||
      declarationVisible(declarationUnit, declaration)) {
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
  if (target != nullptr && target->moduleDeclaration) {
    if (!sourceGraph->isExported(declarationUnit, declaration.position)) {
      diagnostic.message = "Declaration '" + name + "' is private to module '" +
                           target->moduleDeclaration->lexeme +
                           "'; it is not exported.";
      diagnostic.hints.emplace_back(
          "Use the module's public API, or explicitly export this declaration "
          "in its defining interface.");
    } else {
      diagnostic.message =
          "Declaration '" + name +
          "' is not visible; import its public module interface.";
      diagnostic.hints.emplace_back(
          "Add an import of the defining interface or a module that explicitly "
          "re-exports it.");
    }
    diagnostics.push_back(std::move(diagnostic));
    return true;
  }
  if (requester != nullptr && target != nullptr) {
    diagnostic.missingIncludeName = use.lexeme;
    // The defining unit is known even when adding it would expose other
    // errors (for example, missing operations in a conformance block). Give
    // that information as a hint; automatic edits still require a successful
    // include-suggestion trial.
    const std::string relative =
        target->path.lexically_relative(requester->path.parent_path())
            .generic_string();
    if (!relative.empty() &&
        relative.find_first_of("\r\n\"") == std::string::npos) {
      const std::string spelling =
          target->standardLibraryName ? "<" + *target->standardLibraryName + ">"
                                      : "\"" + relative + "\"";
      diagnostic.hints.push_back("Add #include " + spelling +
                                 " to this source file.");
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

auto SemanticVisitor::Impl::resolveMemberTypeAlias(
    ClassId owner, const std::string &name) const
    -> std::optional<ResolvedTypeAlias> {
  if (owner == 0 || owner > classes.size()) {
    return std::nullopt;
  }
  const auto found = classInfo(owner).typeAliases.find(name);
  if (found != classInfo(owner).typeAliases.end()) {
    const MemberTypeAliasInfo &alias = found->second;
    return ResolvedTypeAlias{.id = alias.id,
                             .declaration = alias.declaration,
                             .type = alias.type,
                             .qualifiedName = alias.qualifiedName,
                             .ownerClass = alias.ownerClass,
                             .access = alias.access,
                             .compilerPrivate = alias.compilerPrivate};
  }
  // Alias dependency resolution precedes completion of the member table.
  for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
    const auto &alias = typeAliases[id - 1];
    if (alias.ownerDeclaration == classInfo(owner).declaration &&
        alias.declaration->name().lexeme == name) {
      return ResolvedTypeAlias{.id = id,
                               .declaration = alias.declaration,
                               .type = alias.type,
                               .qualifiedName = alias.qualifiedName,
                               .ownerClass = alias.ownerClass,
                               .access = alias.access,
                               .compilerPrivate = alias.compilerPrivate};
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::resolveTypeAliasPath(
    const TypeRef &type, const std::vector<std::string> &fromScope) const
    -> std::optional<ResolvedTypeAlias> {
  if (!type.qualifiedOwner) {
    return resolveTypeAliasPath(type.name, fromScope);
  }
  const SemanticType owner = typeOf(*type.qualifiedOwner, fromScope);
  if (owner.kind != SemanticType::Class) {
    return std::nullopt;
  }
  auto alias = resolveMemberTypeAlias(owner.classId, type.name.last().lexeme);
  if (!alias) {
    return std::nullopt;
  }
  alias->type = substituteType(alias->type, classSubstitution(owner));
  alias->specializedOwner = true;
  alias->ownerType = owner;
  if (auto qualifier = resolveTypeAliasPath(*type.qualifiedOwner, fromScope)) {
    alias->qualifiers.push_back(std::move(*qualifier));
  }
  return alias;
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
    return resolveMemberTypeAlias(owner, name);
  };
  if (path.segments.size() == 1) {
    for (ClassId owner = currentClass.value_or(0); owner != 0;
         owner = classInfo(owner).enclosingClass) {
      if (const std::optional<ResolvedTypeAlias> alias =
              member(owner, path.last().lexeme)) {
        return alias;
      }
      if (classInfo(owner).nestedTypes.contains(path.last().lexeme))
        return std::nullopt;
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
  if (const auto qualifier = resolveTypeAliasPath(namespacePath, fromScope)) {
    const SemanticType ownerType = substituteType(
        qualifier->type,
        GenericSubstitution{.types = instanceTypeSubstitution,
                            .values = instanceValueSubstitution});
    if (ownerType.kind == SemanticType::Class) {
      if (auto alias = member(ownerType.classId, path.last().lexeme)) {
        alias->type = substituteType(alias->type, classSubstitution(ownerType));
        alias->specializedOwner = true;
        alias->ownerType = ownerType;
        alias->qualifiers.push_back(*qualifier);
        return alias;
      }
    }
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::resolveTypeAliasPathGlobally(
    const NamePath &path, const std::vector<std::string> &fromScope) const
    -> std::optional<TypeAliasId> {
  const auto member =
      [&](ClassId owner,
          const std::string &name) -> std::optional<TypeAliasId> {
    for (TypeAliasId id = 1; id <= typeAliases.size(); ++id) {
      const auto &alias = typeAliases[id - 1];
      if (alias.ownerDeclaration == classInfo(owner).declaration &&
          alias.declaration->name().lexeme == name) {
        return id;
      }
    }
    return std::nullopt;
  };
  if (path.segments.size() == 1) {
    if (currentClass) {
      if (const auto alias = member(*currentClass, path.last().lexeme)) {
        return alias;
      }
    }
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
  if (const auto owner = resolveClassPathGlobally(namespacePath, fromScope)) {
    return member(*owner, path.last().lexeme);
  }
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
  if (currentClass) {
    for (ClassId owner = *currentClass; owner != 0;
         owner = classInfo(owner).enclosingClass) {
      if (classInfo(owner).nestedTypes.empty())
        continue;
      auto scope = classInfo(owner).namespaceScope;
      scope.push_back(classInfo(owner).name.lexeme);
      const auto found =
          visibleClasses.find(qualifiedName(scope, pathSpelling(path)));
      if (found != visibleClasses.end())
        return found->second;
    }
  }
  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const auto found = visibleClasses.find(
        qualifiedName(fromScope, depth - 1, pathSpelling(path)));
    if (found != visibleClasses.end())
      return found->second;
  }
  if (path.segments.size() == 1) {
    return std::nullopt;
  }

  NamePath namespacePath(
      std::vector<Token>(path.segments.begin(), path.segments.end() - 1));
  if (const auto owner = resolveClassPath(namespacePath, fromScope)) {
    const auto nested = classInfo(*owner).nestedTypes.find(path.last().lexeme);
    if (nested != classInfo(*owner).nestedTypes.end())
      return nested->second;
  }
  if (const auto alias = resolveTypeAliasPath(namespacePath, fromScope);
      alias && alias->type.kind == SemanticType::Class) {
    const auto &owner = classInfo(alias->type.classId);
    const auto nested = owner.nestedTypes.find(path.last().lexeme);
    if (nested != owner.nestedTypes.end())
      return nested->second;
  }
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
  for (std::size_t depth = fromScope.size() + 1; depth > 0; --depth) {
    const auto found =
        classIds.find(qualifiedName(fromScope, depth - 1, pathSpelling(path)));
    if (found != classIds.end())
      return found->second;
  }
  if (path.segments.size() == 1) {
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
  if (path.segments.size() == 1) {
    for (ClassId owner = currentClass.value_or(0); owner != 0;
         owner = classInfo(owner).enclosingClass) {
      if (classInfo(owner).nestedTypes.contains(path.last().lexeme))
        return std::nullopt;
    }
  }
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

auto SemanticVisitor::Impl::resolveEnumerator(
    const QualifiedName &expression) const -> std::optional<EnumeratorRecord> {
  if (expression.name().segments.size() < 2)
    return std::nullopt;
  const auto ownerType = expression.ownerType()
                             ? typeOf(*expression.ownerType())
                             : typeOf(TypeRef(NamePath(std::vector<Token>(
                                   expression.name().segments.begin(),
                                   expression.name().segments.end() - 1))));
  if (ownerType.kind != SemanticType::Enum || ownerType.enumId == 0 ||
      ownerType.enumId > enums.size())
    return std::nullopt;
  const auto &owner = enums[ownerType.enumId - 1];
  const auto found = owner.enumerators.find(expression.name().last().lexeme);
  if (found == owner.enumerators.end())
    return std::nullopt;
  auto result = found->second;
  result.symbol.type = ownerType;
  const auto substitution = enumSubstitution(ownerType);
  for (auto &payload : result.payloadTypes)
    payload = substituteType(payload, substitution);
  return result;
}

auto SemanticVisitor::Impl::payloadEnumeratorForCallee(
    const ExprPtr &callee) const -> std::optional<EnumeratorRecord> {
  const auto *qualified = exprAs<QualifiedName>(callee.get());
  if (qualified == nullptr) {
    return std::nullopt;
  }
  const auto enumerator = resolveEnumerator(*qualified);
  if (!enumerator || enumerator->symbol.type.kind != SemanticType::Enum) {
    return std::nullopt;
  }
  const EnumId owner = enumerator->symbol.type.enumId;
  return owner != 0 && owner <= enums.size() && enums[owner - 1].payload
             ? enumerator
             : std::nullopt;
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
    currentType = enumerator.symbol.type;
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
  // A dynamic view argument publishes its sole named owner's dependency at
  // its payload (L-OWN-02), as a constructor does at a text field. The enum
  // must hold no borrowed state of its own type.
  const auto enumeration = enumTypeFor(enumerator.symbol.type);
  const auto variant = enumeration
                           ? std::find_if(enumeration->enumerators.begin(),
                                          enumeration->enumerators.end(),
                                          [&](const auto &candidate) {
                                            return candidate.variantIndex ==
                                                   enumerator.variantIndex;
                                          })
                           : decltype(enumeration->enumerators.begin()){};
  const bool publishable =
      enumeration && variant != enumeration->enumerators.end() &&
      enumerator.symbol.type.arguments.empty() &&
      !containsOpenTypeParameter(enumerator.symbol.type) &&
      !typeTraits(enumerator.symbol.type).containsBorrowedState;
  std::vector<BorrowedFieldOrigin> textOrigins;
  for (std::size_t index = 0; index < common; ++index) {
    const SemanticType actual = analyzeInitializer(
        call.arguments()[index], enumerator.payloadTypes[index]);
    const auto &field = enumerator.payloadTypes[index];
    if ((field == SemanticType::StringView || field == SemanticType::CString) &&
        isOwnerDependentTextExpression(call.arguments()[index])) {
      const SymbolId payload =
          publishable && index < variant->payloadSymbols.size()
              ? variant->payloadSymbols[index]
              : 0;
      const auto dependencies =
          payload == 0
              ? std::vector<SemanticLoanDependency>{}
              : textFieldOwnerDependency(call.arguments()[index], payload);
      if (!dependencies.empty() && !textOrigins.empty())
        report(expressionToken(call.arguments()[index]),
               "A value can hold dynamic text from one owner; this argument "
               "would add a second owner dependency.",
               "GTI-S2045");
      else if (!dependencies.empty())
        textOrigins.push_back({.resultFields = {payload},
                               .origin = BorrowOriginKind::Argument,
                               .parameter = index,
                               .access = AccessMode::ReadOnly});
      else
        reportOwnerDependentTextStorage(call.arguments()[index],
                                        "stored in a payload-enum field");
    }
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
  }
  for (std::size_t index = common; index < call.arguments().size(); ++index) {
    analyze(call.arguments()[index]);
  }
  // A class whose own stored references are guarded by alternatives is not
  // yet an enum payload: the selected alternative would nest one guarded set
  // inside another, which verified MIR does not compose. Unconditional
  // borrowed classes, direct references and enum payloads remain supported.
  for (std::size_t index = 0; index < common; ++index) {
    const auto &field = enumerator.payloadTypes[index];
    if (field.kind != SemanticType::Class)
      continue;
    const auto inventory = storedReferenceInventory(field);
    if (!inventory ||
        std::none_of(inventory->begin(), inventory->end(),
                     [](const auto &path) { return !path.conditions.empty(); }))
      continue;
    report(expressionToken(call.arguments()[index]),
           "Payload field type '" + typeSpelling(field) +
               "' conditionally retains references; a class carrying a "
               "guarded dependency set cannot be an enum payload yet.",
           "GTI-S2067");
    // The rejected value has no checked dependency set, so storing or
    // assigning it would only fail again on this same payload.
    currentType = SemanticType::Unknown;
    return;
  }
  // A payload constructor evaluates its fields in the same ordered input
  // region as an ordinary call. In particular, retaining an earlier reference
  // cannot permit a later argument to mutate or move that referent.
  validateCallPlaceExclusivity(enumerator.payloadTypes, call.arguments(),
                               call.paren(), std::nullopt, AccessMode::ReadOnly,
                               false, {});
  semanticModel.recordPayloadConstruction(
      call, {.owner = owner,
             .type = enumerator.symbol.type,
             .declaration = enumerator.declaration,
             .variantIndex = enumerator.variantIndex,
             .parameterTypes = enumerator.payloadTypes,
             .borrowFields = textOrigins,
             .dependentTextPublication = !textOrigins.empty()});
  if (!textOrigins.empty()) {
    // The binding's retained loan carries the published owner, as for a
    // constructor's text field; no complete dependency set describes it.
    currentType = enumerator.symbol.type;
    return;
  }
  // This producer attaches exact selected-alternative evidence, including
  // every dependency path. Admission remains independently gated until the
  // loan/tag verifier supports conditional borrowing; metadata alone does
  // not enable reference-bearing payloads.
  if (auto complete = payloadBorrowDependencySet(call, enumerator))
    semanticModel.recordBorrowDependencySet(call, std::move(*complete));
  else if (std::any_of(enumerator.payloadTypes.begin(),
                       enumerator.payloadTypes.end(),
                       [&](const SemanticType &field) {
                         return field.kind == SemanticType::Reference ||
                                typeTraits(field).containsBorrowedState;
                       }))
    report(call.paren(),
           "Borrowed payload construction requires complete, stable dependency "
           "origins for every retained field.",
           "GTI-S2067");
  currentType = enumerator.symbol.type;
}

auto SemanticVisitor::Impl::analyzePayloadSwitchPattern(
    const ExprPtr &expression, const SemanticType &subjectType,
    bool consumingSubject) -> std::optional<ResolvedPayloadPatternInfo> {
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
  const auto enumerator = resolveEnumerator(*qualified);
  if (!enumerator || enumerator->symbol.type != subjectType) {
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
                                    .type = subjectType,
                                    .declaration = enumerator->declaration,
                                    .variantIndex = enumerator->variantIndex,
                                    .consuming = consumingSubject};
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
    const bool storedReference = payloadType.kind == SemanticType::Reference;
    if (storedReference && payloadType.arguments.size() != 1) {
      return std::nullopt;
    }
    const std::string tagOnlyPattern =
        "'case " + typeSpelling(subjectType) +
        "::" + enumerator->declaration->name.lexeme + ":'";
    // A by-value binding owns its field. When the switch consumes its subject
    // (a prvalue or an explicit std::move) the field moves out of the active
    // alternative; when it observes a named place the binding is a copy.
    const SemanticTypeTraits payloadTraits = typeTraits(payloadType);
    if (!storedReference && !binding->isReferencePattern() &&
        consumingSubject && !payloadTraits.movable && !payloadTraits.copyable) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics, binding->name(),
          "Payload field " + std::to_string(index + 1) + " of variant '" +
              enumerator->declaration->name.lexeme + "' has type '" +
              typeSpelling(payloadType) +
              "', which can be neither moved nor copied into a match "
              "binding.");
      diagnostic.hints.emplace_back("Match the variant without payload "
                                    "bindings (for example " +
                                    tagOnlyPattern + ").");
      diagnostics.emplace_back(std::move(diagnostic));
      return std::nullopt;
    }
    if (!storedReference && !binding->isReferencePattern() &&
        !consumingSubject && !payloadTraits.copyable) {
      Diagnostic diagnostic = makeDiagnostic(
          "GTI-S2067", DiagnosticPhase::Semantics, binding->name(),
          "Payload field " + std::to_string(index + 1) + " of variant '" +
              enumerator->declaration->name.lexeme + "' has move-only type '" +
              typeSpelling(payloadType) +
              "' and cannot be copied into a non-consuming match binding.");
      diagnostic.hints.emplace_back(
          "Match the variant without payload bindings (for example " +
          tagOnlyPattern + "), or borrow it with 'auto& " +
          binding->name().lexeme + "'.");
      diagnostic.hints.emplace_back(
          "A switch over a consumed enum, such as a temporary or "
          "'std::move' of an owned local, moves the payload into a "
          "by-value binding instead.");
      diagnostics.emplace_back(std::move(diagnostic));
      return std::nullopt;
    }
    if (storedReference && !consumingSubject &&
        !binding->isReferencePattern() &&
        payloadType.referenceAccess == AccessMode::Mutable) {
      report(binding->name(),
             "An exclusive reference payload cannot be copied from an "
             "unconsumed enum; reborrow it with 'auto&' or 'mut auto&', or "
             "consume the enum to transfer the reference.",
             "GTI-S2067");
      return std::nullopt;
    }
    if (storedReference && binding->isReferencePattern() && consumingSubject) {
      report(binding->name(),
             "A consumed reference payload uses a plain binding to transfer "
             "its reference; explicit reference patterns reborrow an "
             "unconsumed enum.",
             "GTI-S2067");
      return std::nullopt;
    }
    if (storedReference && binding->isReferencePattern() &&
        binding->isMutablePattern() &&
        payloadType.referenceAccess != AccessMode::Mutable) {
      report(binding->name(),
             "A read-only reference payload cannot produce an exclusive "
             "mutable reborrow.",
             "GTI-S2067");
      return std::nullopt;
    }
    const PayloadExtractionKind extractionKind =
        storedReference
            ? (binding->isReferencePattern()
                   ? PayloadExtractionKind::ReborrowReference
               : consumingSubject ? PayloadExtractionKind::TransferReference
                                  : PayloadExtractionKind::CopyReference)
            : (binding->isReferencePattern()
                   ? PayloadExtractionKind::BorrowValue
               : consumingSubject ? PayloadExtractionKind::MoveValue
                                  : PayloadExtractionKind::CopyValue);
    const SemanticType bindingType =
        binding->isReferencePattern()
            ? SemanticType::referenceTo(
                  storedReference ? payloadType.arguments.front() : payloadType,
                  binding->isMutablePattern() ? AccessMode::Mutable
                                              : AccessMode::ReadOnly)
            : payloadType;
    result.bindings.push_back(
        {.name = &binding->name(),
         .source = binding,
         .binding = bindingInfo(bindingType, binding->isMutablePattern()
                                                 ? AccessMode::Mutable
                                                 : AccessMode::ReadOnly),
         .payloadIndex = index,
         .extractionKind = extractionKind});
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

auto SemanticVisitor::Impl::appendReadOnlyPlaceContext(
    Diagnostic &diagnostic, const ExprPtr &expression) const -> void {
  if (!expression)
    return;
  if (const auto *grouping = exprAs<Grouping>(expression.get())) {
    appendReadOnlyPlaceContext(diagnostic, grouping->expression());
    return;
  }
  if (const auto *index = exprAs<Index>(expression.get())) {
    const auto *type = semanticModel.findType(*index->object());
    if (type && type->kind == SemanticType::Array)
      appendReadOnlyPlaceContext(diagnostic, index->object());
    return;
  }
  const Symbol *symbol = nullptr;
  if (const auto *variable = exprAs<Variable>(expression.get())) {
    symbol = resolve(variable->name());
  } else if (const auto *member = exprAs<Get>(expression.get())) {
    if (member->access().kind != TokenKind::DOT)
      return;
    const auto *resolved =
        findMember(memberAccessObjectType(*member), member->name());
    if (!resolved)
      return;
    if (resolved->symbol.assignable && !resolved->symbol.staticMember) {
      appendReadOnlyPlaceContext(diagnostic, member->object());
      return;
    }
    symbol = &resolved->symbol;
  }
  // Calls, overloaded projections and raw pointers do not prove that changing
  // a named binding's mutability would grant access to the result.
  if (!symbol || symbol->declaration.lexeme.empty())
    return;
  const bool readOnlyReference =
      symbol->type.kind == SemanticType::Reference &&
      symbol->type.referenceAccess == AccessMode::ReadOnly;
  if (symbol->assignable && !readOnlyReference)
    return;
  diagnostic.related.push_back(
      {tokenSpan(symbol->declaration),
       "'" + symbol->declaration.lexeme + "' is declared " +
           (readOnlyReference ? "as a read-only reference here."
                              : "without mutable access here.")});
  if (readOnlyReference) {
    diagnostic.hints.emplace_back(
        "Mutation requires a 'mut T&' reference and exclusive access to its "
        "owner; otherwise keep this operation read-only.");
  } else if (symbol->lambdaCapture) {
    diagnostic.hints.emplace_back(
        "This capture is an immutable value snapshot; changing its source "
        "binding does not make the captured value mutable.");
  } else if (symbol->constant) {
    diagnostic.hints.emplace_back(
        "A constant cannot be mutated; use separate mutable storage if a "
        "changing value is needed.");
  } else if (target.executionProfile == ExecutionProfile::Concurrent &&
             (symbol->bindingKind == SemanticBindingKind::GlobalVariable ||
              symbol->bindingKind == SemanticBindingKind::StaticField)) {
    diagnostic.hints.emplace_back(
        "Concurrent globals and static fields must stay immutable. Use local "
        "mutable storage or an immutable value with an approved atomic or "
        "mutex contract for synchronized mutation.");
  } else {
    diagnostic.hints.emplace_back(
        "Declare '" + symbol->declaration.lexeme +
        "' with 'mut' if mutation is intended. Mutable fields still require "
        "mutable access through their containing value.");
  }
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
    const ExprPtr &expression) const -> const Expr * {
  const Expr *producer = expression.get();
  while (producer != nullptr) {
    const Expr *source = semanticModel.directInitializationSource(*producer);
    if (source == nullptr) {
      const auto *grouping = exprAs<Grouping>(producer);
      source = grouping == nullptr ? nullptr : grouping->expression().get();
    }
    if (source == nullptr)
      break;
    producer = source;
  }
  return producer;
}

auto SemanticVisitor::Impl::payloadEnumType(const SemanticType &type) const
    -> bool {
  return type.kind == SemanticType::Enum && type.enumId != 0 &&
         type.enumId <= enums.size() && enums[type.enumId - 1].payload;
}

auto SemanticVisitor::Impl::freshTemporaryValue(const ExprPtr &expression) const
    -> FreshTemporaryValueAssessment {
  FreshTemporaryValueAssessment result;
  if (!expression) {
    return result;
  }
  const ExpressionInfo *info = semanticModel.findExpression(*expression);
  const bool payloadEnum = info != nullptr && payloadEnumType(info->type);
  const bool callable =
      info != nullptr && (info->type.kind == SemanticType::Lambda ||
                          info->type.kind == SemanticType::FunctionItem);
  if (info == nullptr || info->category != ValueCategory::Value ||
      (info->type.kind != SemanticType::Class && !payloadEnum && !callable)) {
    return result;
  }
  result.type = info->type;
  if (isExplicitMoveReceiver(expression)) {
    result.failure = FreshTemporaryValueFailure::ExplicitMove;
    return result;
  }
  if (!payloadEnum && !callable) {
    const ClassInfo *record = classInfo(info->type);
    if (record == nullptr || (record->kind != ClassKind::Class &&
                              record->kind != ClassKind::Struct)) {
      result.failure = FreshTemporaryValueFailure::NonConcreteRecord;
      return result;
    }
  }
  if (info->traits.containsBorrowedState && !callable) {
    result.failure = FreshTemporaryValueFailure::BorrowedState;
    return result;
  }

  const Expr *producer = transparentTemporaryProducer(expression);
  if (producer == nullptr) {
    result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
    return result;
  }
  if (callable) {
    const auto *lambda = exprAs<Lambda>(producer);
    const auto *declaration =
        lambda ? semanticModel.findLambda(*lambda) : nullptr;
    const auto *item = semanticModel.findFunctionItemSelection(*producer);
    result.eligible =
        (declaration && info->type.kind == SemanticType::Lambda &&
         declaration->id == info->type.lambdaId &&
         declaration->capability == CallableInvocationCapability::Read &&
         (!info->traits.containsBorrowedState ||
          !semanticModel.findCaptureBorrowDependencies(*producer).empty())) ||
        (item && info->type.kind == SemanticType::FunctionItem &&
         item->type == info->type && item->function != 0 && item->declaration &&
         item->capability == CallableInvocationCapability::Read &&
         !info->traits.containsBorrowedState);
    result.failure = result.eligible
                         ? FreshTemporaryValueFailure::None
                         : FreshTemporaryValueFailure::UnsupportedProducer;
    return result;
  }
  if (payloadEnum) {
    // Both owning and passive alternatives have the same exact construction
    // identity. Materialization and cleanup remain independently checked by
    // HIR/MIR; a factory continues through the ordinary selected-call proof.
    const auto enumeration = enumTypeFor(info->type);
    const auto *call = exprAs<Call>(producer);
    const auto *name = exprAs<QualifiedName>(producer);
    const auto *construction =
        call == nullptr ? nullptr
                        : semanticModel.findPayloadConstruction(*call);
    const auto *enumerator =
        name == nullptr ? nullptr : semanticModel.findEnumerator(*name);
    const auto exactAlternative = [&](const auto *resolved) {
      return enumeration && enumeration->payload &&
             !containsOpenTypeParameter(info->type) && resolved &&
             resolved->type == info->type &&
             resolved->owner == info->type.enumId &&
             resolved->variantIndex < enumeration->enumerators.size() &&
             resolved->declaration ==
                 enumeration->enumerators[resolved->variantIndex].declaration;
    };
    if (construction || enumerator) {
      result.eligible =
          exactAlternative(construction) || exactAlternative(enumerator);
      result.failure = result.eligible
                           ? FreshTemporaryValueFailure::None
                           : FreshTemporaryValueFailure::UnsupportedProducer;
      return result;
    }
    if (!enumeration || containsOpenTypeParameter(info->type)) {
      result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
      return result;
    }
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
  if (const ResolvedOperatorInfo *resolved =
          semanticModel.findOperator(*producer);
      resolved != nullptr && resolved->function != 0 &&
      resolved->returnType == info->type &&
      resolved->borrowOrigin == BorrowOriginKind::None) {
    const FunctionInfo *target = semanticModel.findFunction(resolved->function);
    if (target == nullptr && instanceBaseModel != nullptr)
      target = instanceBaseModel->findFunction(resolved->function);
    if (target != nullptr && target->linkage == LanguageLinkage::Gti) {
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
        target->bodyDeclaration()->body() != nullptr &&
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

auto SemanticVisitor::Impl::freshTemporaryReceiverValue(
    const ExprPtr &expression) -> FreshTemporaryValueAssessment {
  FreshTemporaryValueAssessment result = freshTemporaryValue(expression);
  if (result.failure != FreshTemporaryValueFailure::BorrowedState) {
    return result;
  }

  // A carrier is materialized only for its immediate receiver use. Its
  // established stored-borrow summary, not the public type name, supplies the
  // external dependency. Keep this separate from temporary argument admission.
  const Expr *producer = transparentTemporaryProducer(expression);
  if (producer == nullptr) {
    result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
    return result;
  }
  bool ordinaryProducer = false;
  if (const auto *construction = semanticModel.findConstruction(*producer)) {
    ordinaryProducer = construction->kind == ConstructorKind::Ordinary &&
                       construction->constructedType == result.type &&
                       construction->borrowOrigin == BorrowOriginKind::Argument;
  } else if (const auto *call = exprAs<Call>(producer)) {
    const ResolvedCallInfo *resolved = semanticModel.findCall(*call);
    const FunctionInfo *target =
        resolved == nullptr ? nullptr
                            : semanticModel.findFunction(resolved->function);
    if (target == nullptr && resolved != nullptr &&
        instanceBaseModel != nullptr)
      target = instanceBaseModel->findFunction(resolved->function);
    ordinaryProducer = resolved != nullptr && target != nullptr &&
                       target->linkage == LanguageLinkage::Gti &&
                       !target->virtualMethod &&
                       resolved->dispatch == CallDispatch::Static &&
                       resolved->declaration != nullptr &&
                       target->bodyDeclaration()->body() != nullptr &&
                       !resolved->declaration->runtimeBinding() &&
                       !resolved->declaration->hasCLinkage() &&
                       !resolved->declaration->operatorName() &&
                       resolved->intrinsic == IntrinsicKind::None &&
                       resolved->returnType == result.type &&
                       resolved->borrowOrigin != BorrowOriginKind::None;
  }
  if (!ordinaryProducer) {
    result.failure = FreshTemporaryValueFailure::UnsupportedProducer;
    return result;
  }
  const auto dependencies = storedBorrowDependencies(expression);
  if (!hasStableBorrowStorage(expression) || dependencies.empty() ||
      std::any_of(
          dependencies.begin(), dependencies.end(), [](const auto &dependency) {
            return dependency.place.root == 0 && !dependency.place.receiver;
          })) {
    result.failure = FreshTemporaryValueFailure::UnstableBorrowOwner;
    return result;
  }
  result.eligible = true;
  result.failure = FreshTemporaryValueFailure::None;
  return result;
}

auto SemanticVisitor::Impl::freshTemporaryMethodSupported(
    const CallableCandidate &function) const -> bool {
  if (function.ownerClass == 0 || function.staticMember ||
      function.declaration == nullptr) {
    return false;
  }
  // Dispatch selects which body runs, not where the receiver lives: every
  // override shares the selected member's receiver qualifier and result
  // contract, so a virtual member borrows the materialized temporary exactly
  // as a non-virtual one does.
  // A nonescaping read observation keeps the fresh owner alive through the
  // full expression. The ordinary borrow-origin checks still reject storing
  // or returning a view/reference whose temporary owner would expire.
  const bool readObservation =
      function.receiverMutability == ReceiverMutability::ReadOnly &&
      function.returnBorrowOrigin == BorrowOriginKind::Receiver &&
      function.returnBorrowAccess == AccessMode::ReadOnly &&
      (function.returnType.kind == SemanticType::StringView ||
       (function.returnType.kind == SemanticType::Reference &&
        function.returnType.referenceAccess == AccessMode::ReadOnly));
  return readObservation ||
         (function.returnType.kind != SemanticType::Reference &&
          function.returnBorrowOrigin == BorrowOriginKind::None &&
          !typeTraits(function.returnType).containsBorrowedState);
}

auto SemanticVisitor::Impl::receiverTextFieldRelay(
    const CallableCandidate &function) const -> bool {
  if (function.receiverMutability != ReceiverMutability::ReadOnly ||
      function.returnBorrowOrigin != BorrowOriginKind::Receiver ||
      (function.returnType.kind != SemanticType::StringView &&
       function.returnType.kind != SemanticType::CString) ||
      function.returnBorrowFields.size() != 1)
    return false;
  const BorrowedFieldOrigin &mapping = function.returnBorrowFields.front();
  return mapping.origin == BorrowOriginKind::Receiver &&
         mapping.sourceFields.size() == 1 && mapping.resultFields.empty() &&
         mapping.resultConditions.empty() && mapping.sourceConditions.empty();
}

auto SemanticVisitor::Impl::consumingReceiverValue(
    const ExprPtr &receiver) const -> bool {
  const ExpressionInfo *info =
      receiver ? semanticModel.findExpression(*receiver) : nullptr;
  return info != nullptr && info->category == ValueCategory::Value &&
         info->type.kind == SemanticType::Class && info->traits.movable &&
         (!info->traits.containsBorrowedState ||
          completeBorrowDependencySet(receiver).has_value());
}

auto SemanticVisitor::Impl::receiverModeForObject(
    const ExprPtr &receiver, const CallableCandidate &function,
    bool allowFreshTemporary, bool consumingReceiver) -> CallReceiverMode {
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
  // A text field relay returns what a dependent temporary holds in that
  // field, its owner's text, never a view of the temporary itself.
  if (allowFreshTemporary && freshTemporaryMethodSupported(function) &&
      (info == nullptr || !info->traits.containsBorrowedState ||
       ephemeralBorrowResultSupported(function) ||
       (isTextDependentValue(*info) && receiverTextFieldRelay(function))) &&
      (freshTemporaryReceiverValue(receiver).eligible ||
       consumingReceiverValue(receiver))) {
    return function.receiverMutability == ReceiverMutability::Mutable
               ? CallReceiverMode::FreshTemporaryMutable
               : CallReceiverMode::FreshTemporaryRead;
  }
  return CallReceiverMode::Value;
}

auto SemanticVisitor::Impl::callReceiverMode(const ExprPtr &callee,
                                             const CallableCandidate &function)
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
    return receiverModeForObject(
        member->object(), function, true,
        consumingReceiverValue(member->object()) &&
            (function.receiverMutability == ReceiverMutability::Consuming ||
             isExplicitMoveReceiver(member->object())));
  }
  return function.receiverMutability == ReceiverMutability::Mutable &&
                 receiverAllowsMutation(currentReceiverMutability)
             ? CallReceiverMode::MutablePlace
             : CallReceiverMode::ReadOnlyPlace;
}

auto SemanticVisitor::Impl::receiverModeAllowsMutation(CallReceiverMode mode)
    -> bool {
  return mode == CallReceiverMode::MutablePlace ||
         mode == CallReceiverMode::FreshTemporaryMutable ||
         mode == CallReceiverMode::Consuming;
}

auto SemanticVisitor::Impl::reportMutableReceiverFailure(
    const ExprPtr &receiver, const Token &location,
    const CallableCandidate &function, std::string_view operation) -> void {
  const FreshTemporaryValueAssessment assessment =
      freshTemporaryReceiverValue(receiver);
  const bool valueShaped = assessment.type.kind == SemanticType::Class;
  if (!valueShaped) {
    report(location, std::string(operation) + " requires a mutable receiver.");
    appendReadOnlyPlaceContext(diagnostics.back(), receiver);
    if (function.declaration != nullptr)
      diagnostics.back().related.push_back(
          {tokenSpan(function.declaration->name()),
           "This operation requires mutable receiver access."});
    return;
  }

  std::string reason;
  if (function.returnType.kind == SemanticType::Reference ||
      function.returnBorrowOrigin != BorrowOriginKind::None ||
      typeTraits(function.returnType).containsBorrowedState ||
      (typeTraits(assessment.type).containsBorrowedState &&
       !ephemeralBorrowResultSupported(function))) {
    reason = "A temporary receiver cannot produce a reference, pointer-like "
             "value, or value containing tracked borrowed state.";
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
      reason = "A temporary containing tracked borrowed state requires a "
               "complete set of stable owner dependencies for an immediate "
               "method call.";
      break;
    case FreshTemporaryValueFailure::UnstableBorrowOwner:
      reason = "A temporary borrowed receiver requires an owner whose storage "
               "remains alive for the complete expression.";
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
  if (assessment.failure == FreshTemporaryValueFailure::UnstableBorrowOwner) {
    diagnostic.hints.emplace_back(
        "Keep the borrowed owner in named storage that outlives the complete "
        "expression before creating its service or view.");
  } else if (typeTraits(assessment.type).containsBorrowedState) {
    diagnostic.hints.emplace_back(
        "Use a named service or view with a valid stored owner dependency; "
        "make the binding 'mut' when the operation requires mutable access.");
  } else {
    diagnostic.hints.emplace_back(
        "Bind the value to a 'mut' local before calling this operation when "
        "the object must come from existing or unsupported storage.");
  }
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
      !canAccessMember(member->access, member->symbol.ownerClass,
                       member->symbol.staticMember ? SemanticType::Unknown
                                                   : objectType) &&
      !(analyzingReflectedFieldAccess &&
        classInfo(member->symbol.ownerClass).reflectPrivate)) {
    const ClassInfo &declaringOwner = classInfo(member->symbol.ownerClass);
    Diagnostic diagnostic = makeDiagnostic(
        "GTI-S2007", DiagnosticPhase::Semantics, name,
        "Member '" + name.lexeme + "' of '" + declaringOwner.name.lexeme +
            "' is " + std::string(accessModifierSpelling(member->access)) +
            ".");
    diagnostic.related.push_back(
        {tokenSpan(member->symbol.declaration), "Member declared here."});
    diagnostics.emplace_back(std::move(diagnostic));
  }
  return member;
}

auto SemanticVisitor::Impl::typeSpelling(const SemanticType &type) const
    -> std::string {
  // Registration may report before FunctionInfo is complete. Supply the
  // canonical declaration record to the same printer used by finished models.
  return SemanticTypePrinter(semanticModel)
      .print(type, [&](GenericParameterId id) {
        for (const auto &[declaration, parameters] :
             functionGenericParameters) {
          if (const auto *parameter = findGenericParameter(parameters, id)) {
            return parameter;
          }
        }
        for (const auto &[declaration, parameters] :
             constructorGenericParameters) {
          if (const auto *parameter = findGenericParameter(parameters, id)) {
            return parameter;
          }
        }
        for (const auto &owner : classes) {
          if (const auto *parameter =
                  findGenericParameter(owner.genericParameters, id)) {
            return parameter;
          }
        }
        for (const auto &conformance : conformances) {
          if (const auto *parameter =
                  findGenericParameter(conformance.genericParameters, id)) {
            return parameter;
          }
        }
        if (registeringConformanceParameters != nullptr) {
          return findGenericParameter(*registeringConformanceParameters, id);
        }
        return static_cast<const GenericParameterInfo *>(nullptr);
      });
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
  Symbol state{.type = openClassType(*currentClass),
               .assignable = receiverAllowsMutation(currentReceiverMutability)};
  for (auto path : constructorDepth == 0
                       ? rawPointerFieldPaths(state.type)
                       : std::vector<std::vector<PlaceProjection>>{}) {
    auto originPlace = placeKey({.root = &state, .receiver = true});
    originPlace.projections = path;
    state.rawPointerFields.push_back(
        {std::move(path),
         {{.place = std::move(originPlace),
           .addressSite = tokenSpan(classInfo(*currentClass).name),
           .parameterPointee = true}}});
  }
  scopes.back().insert_or_assign(std::string(receiverStateName),
                                 std::move(state));
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
  if (const Expr *alias = semanticModel.findExpressionAlias(*expression)) {
    return semanticPlace(alias);
  }
  const ExprKind kind = expression->exprKind();
  if (kind == ExprKind::Grouping) {
    return semanticPlace(
        static_cast<const Grouping *>(expression)->expression().get());
  }
  if (kind == ExprKind::Binary) {
    // `(effects, place)`: the comma's place is its right operand's.
    const auto *binary = static_cast<const Binary *>(expression);
    if (binary->oper().kind == TokenKind::COMMA) {
      return semanticPlace(binary->right().get());
    }
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
      } else if (objectType->kind == SemanticType::RawPointer) {
        place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      }
    }
    return place;
  }
  if (kind == ExprKind::IndexSet) {
    return semanticPlace(*static_cast<const IndexSet *>(expression));
  }
  if (kind == ExprKind::Set) {
    return semanticPlace(*static_cast<const Set *>(expression));
  }
  if (kind == ExprKind::PlaceSet) {
    return semanticPlace(static_cast<const PlaceSet *>(expression)->target());
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
    } else if (objectType->kind == SemanticType::RawPointer) {
      place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
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
                                                   ValueState state,
                                                   const Token &origin)
    -> void {
  if (place.root == nullptr || (place.projections.empty() && !place.receiver)) {
    return;
  }
  invalidateBorrowDependencySet(place);
  if (state != ValueState::Available)
    invalidateRawPointerOrigins(place);
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
        {.projections = place.projections,
         .state = state,
         .moveOrigins = {tokenSpan(origin)}});
  } else {
    found->state = state;
    found->moveOrigins = {tokenSpan(origin)};
  }
}

auto SemanticVisitor::Impl::reinitializePlace(const SemanticPlace &place)
    -> void {
  if (place.root == nullptr) {
    return;
  }
  invalidateBorrowDependencySet(place);
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
  if (loanFlow.deferredGenericFlow) {
    return;
  }
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
  appendMoveContext(diagnostics.back(), state.moveOrigins);
}

auto SemanticVisitor::Impl::validateProjectedPlaceRead(const Expr &expression)
    -> void {
  if (std::find(suppressedProjectedPlaceReads.begin(),
                suppressedProjectedPlaceReads.end(),
                &expression) != suppressedProjectedPlaceReads.end() ||
      exprAs<Assign>(&expression) != nullptr ||
      exprAs<Set>(&expression) != nullptr ||
      exprAs<IndexSet>(&expression) != nullptr ||
      exprAs<DereferenceSet>(&expression) != nullptr ||
      exprAs<PlaceSet>(&expression) != nullptr) {
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
  if (currentReceiverMutability == ReceiverMutability::Consuming ||
      loanFlow.deferredGenericFlow) {
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
  for (const auto &origin : state.moveOrigins)
    diagnostics.back().related.push_back(
        {origin, "Ownership transferred here."});
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
  if (loanFlow.deferredGenericFlow) {
    return;
  }
  report(use,
         symbol.valueState == ValueState::Moved
             ? "Value '" + use.lexeme + "' has already been moved."
             : "Value '" + use.lexeme +
                   "' may have been moved on another control-flow path.",
         "GTI-S2018");
  appendMoveContext(diagnostics.back(), symbol.moveOrigins);
}

auto SemanticVisitor::Impl::mergedMoveOrigins(
    const std::vector<SourceSpan> &left, const std::vector<SourceSpan> &right)
    -> std::vector<SourceSpan> {
  auto result = left;
  result.insert(result.end(), right.begin(), right.end());
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    return std::tie(a.source, a.start, a.end) <
           std::tie(b.source, b.start, b.end);
  });
  result.erase(std::unique(result.begin(), result.end(),
                           [](const auto &a, const auto &b) {
                             return a.source == b.source &&
                                    a.start == b.start && a.end == b.end;
                           }),
               result.end());
  // Keep diagnostics bounded even when control-flow joins many transfers.
  if (result.size() > 4)
    result.resize(4);
  return result;
}

auto SemanticVisitor::Impl::appendMoveContext(
    Diagnostic &diagnostic, const std::vector<SourceSpan> &origins) -> void {
  for (const auto &origin : origins)
    diagnostic.related.push_back({origin, "Ownership transferred here."});
  diagnostic.hints.emplace_back(
      "Use the value before transferring it, or borrow it instead if the "
      "receiving API supports references. If independent ownership is needed, "
      "make an explicit clone before the transfer when the type supports it.");
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
      std::vector<SourceSpan> origins;
      for (const auto *symbol : {&left, &right}) {
        for (const auto &candidate : symbol->projectedValueStates) {
          if (candidate.projections == projections &&
              candidate.state != ValueState::Available)
            origins = mergedMoveOrigins(origins, candidate.moveOrigins);
        }
      }
      result.push_back({.projections = projections,
                        .state = state,
                        .moveOrigins = std::move(origins)});
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
          target->second.moveOrigins = mergedMoveOrigins(
              leftSymbol->second.valueState == ValueState::Available
                  ? std::vector<SourceSpan>{}
                  : leftSymbol->second.moveOrigins,
              rightSymbol->second.valueState == ValueState::Available
                  ? std::vector<SourceSpan>{}
                  : rightSymbol->second.moveOrigins);
        }
        target->second.projectedValueStates =
            mergedProjectedValueStates(leftSymbol->second, rightSymbol->second);
        target->second.rawPointerOrigins = leftSymbol->second.rawPointerOrigins;
        mergeRawPointerOrigins(target->second.rawPointerOrigins,
                               rightSymbol->second.rawPointerOrigins);
        target->second.rawPointerFields = leftSymbol->second.rawPointerFields;
        mergeRawPointerFields(target->second.rawPointerFields,
                              rightSymbol->second.rawPointerFields);
        // Unknown alternatives carry no local origins. Taint the surviving
        // facts even when the union is a singleton (or both paths agree).
        for (auto &origin : target->second.rawPointerOrigins)
          origin.possible = true;
        for (auto &field : target->second.rawPointerFields)
          for (auto &origin : field.origins)
            origin.possible = true;
        target->second.storageLoans = leftSymbol->second.storageLoans;
        for (const SemanticLoanId loan : rightSymbol->second.storageLoans) {
          appendUniqueLoan(target->second.storageLoans, loan);
        }
        target->second.retainedLoan =
            leftSymbol->second.retainedLoan == rightSymbol->second.retainedLoan
                ? leftSymbol->second.retainedLoan
                : baseSymbol.retainedLoan;
        target->second.completeBorrowSet = joinedBorrowDependencySets(
            leftSymbol->second.valueState == ValueState::Available
                ? leftSymbol->second.completeBorrowSet
                : std::nullopt,
            rightSymbol->second.valueState == ValueState::Available
                ? rightSymbol->second.completeBorrowSet
                : std::nullopt);
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
    restoreScopeState(base);
    mergeValueStates(base, merged, states[index]);
    merged = scopes;
  }
  return merged;
}

auto SemanticVisitor::Impl::restoreScopeState(const ScopeStack &source)
    -> void {
  if (scopes.size() > source.size()) {
    scopes.resize(source.size());
  }
  for (std::size_t scopeIndex = 0; scopeIndex < source.size(); ++scopeIndex) {
    const Scope &from = source[scopeIndex];
    if (scopeIndex == scopes.size()) {
      // Moving existing scopes during growth keeps their nodes, and therefore
      // their Symbol addresses.
      scopes.push_back(from);
      continue;
    }
    Scope &target = scopes[scopeIndex];
    std::erase_if(target, [&from](const auto &entry) {
      return !from.contains(entry.first);
    });
    for (const auto &[name, symbol] : from) {
      if (const auto found = target.find(name); found != target.end()) {
        found->second = symbol;
      } else {
        target.emplace(name, symbol);
      }
    }
  }
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
      target.moveOrigins = found->second.moveOrigins;
      target.rawPointerOrigins = found->second.rawPointerOrigins;
      target.rawPointerFields = found->second.rawPointerFields;
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
  recordProgramScopeBorrowCleanup();
  std::unordered_set<SymbolId> expired;
  for (auto &[_, symbol] : scopes.back()) {
    if (symbol.valueState == ValueState::Available) {
      const auto *loan = semanticModel.findLoan(symbol.retainedLoan);
      if (loan != nullptr && loan->lifecycleBound)
        recordSemanticLoanUse(toolingSymbolFor(symbol));
    }
    if (symbol.valueState == ValueState::Available)
      applyRawCleanupWrites(symbol.type, tokenSpan(symbol.declaration),
                            symbol.rawPointerFields);
    removeLoanCarrier(symbol);
    expired.insert(symbolForDeclaration(symbol.declaration));
  }
  for (auto &scope : scopes) {
    for (auto &[_, carrier] : scope) {
      const auto expire = [&](auto &origins) {
        for (auto &origin : origins)
          if (!origin.parameterPointee && origin.loopInput == 0 &&
              expired.contains(origin.place.root))
            origin.expired = true;
      };
      expire(carrier.rawPointerOrigins);
      for (auto &field : carrier.rawPointerFields)
        expire(field.origins);
    }
  }
  auto *globals = currentNamespaceSymbolsMutable();
  auto &bindings = globals ? *globals : valueFlow.namespaceSymbols;
  for (auto &[_, carrier] : bindings) {
    const auto expire = [&](auto &origins) {
      for (auto &origin : origins)
        if (!origin.parameterPointee && origin.loopInput == 0 &&
            expired.contains(origin.place.root))
          origin.expired = true;
    };
    expire(carrier.rawPointerOrigins);
    for (auto &field : carrier.rawPointerFields)
      expire(field.origins);
  }
  scopes.pop_back();
}

auto SemanticVisitor::Impl::report(const Token &token, std::string message,
                                   std::string code) -> void {
  reporter.report(token, std::move(message), std::move(code));
}

auto SemanticVisitor::Impl::mergeRawPointerOrigins(
    std::vector<RawPointerOrigin> &target,
    const std::vector<RawPointerOrigin> &source) -> void {
  for (const auto &origin : source) {
    const auto existing =
        std::find_if(target.begin(), target.end(), [&](const auto &existing) {
          return existing.place == origin.place &&
                 existing.throughLoan == origin.throughLoan &&
                 existing.parameterPointee == origin.parameterPointee &&
                 existing.loopInput == origin.loopInput &&
                 existing.pointeeType == origin.pointeeType &&
                 existing.sourceResultQuery == origin.sourceResultQuery;
        });
    if (existing == target.end())
      target.push_back(origin);
    else {
      existing->invalidated |= origin.invalidated;
      existing->expired |= origin.expired;
      existing->possible |= origin.possible;
      for (const auto &place : origin.lifetimeInvalidations)
        if (std::find(existing->lifetimeInvalidations.begin(),
                      existing->lifetimeInvalidations.end(),
                      place) == existing->lifetimeInvalidations.end())
          existing->lifetimeInvalidations.push_back(place);
    }
  }
}

auto SemanticVisitor::Impl::mergeRawPointerFields(
    std::vector<ProjectedRawPointerOrigins> &target,
    const std::vector<ProjectedRawPointerOrigins> &source) -> void {
  for (const auto &field : source) {
    const auto existing =
        std::find_if(target.begin(), target.end(), [&](const auto &candidate) {
          return candidate.projections == field.projections;
        });
    if (existing == target.end())
      target.push_back(field);
    else
      mergeRawPointerOrigins(existing->origins, field.origins);
  }
}

auto SemanticVisitor::Impl::rawPointerFields(const Expr *expression)
    -> std::vector<ProjectedRawPointerOrigins> {
  if (!rawAuditEnabled())
    return {};
  if (!expression)
    return {};
  if (const auto found = valueFlow.rawOperatorResults.find(expression);
      found != valueFlow.rawOperatorResults.end())
    return found->second;
  if (const auto *group = exprAs<Grouping>(expression))
    return rawPointerFields(group->expression().get());
  if (const auto *conversion = exprAs<Conversion>(expression))
    return rawPointerFields(conversion->value().get());
  if (const auto *call = exprAs<Call>(expression)) {
    if (semanticModel.findPayloadConstruction(*call)) {
      std::vector<ProjectedRawPointerOrigins> fields;
      for (const auto &argument : call->arguments())
        mergeRawPointerFields(fields, rawPointerFields(argument.get()));
      return fields;
    }
  }
  if (const auto *call = exprAs<Call>(expression);
      call && resolvedIntrinsicKind(*call) == IntrinsicKind::Move &&
      call->arguments().size() == 1)
    return rawPointerFields(call->arguments().front().get());
  const Expr *rawObject = nullptr;
  if (const auto *unary = exprAs<Unary>(expression);
      unary && unary->oper().kind == TokenKind::STAR &&
      semanticModel.typeOf(*unary->right()).kind == SemanticType::RawPointer)
    rawObject = unary->right().get();
  if (rawObject) {
    const auto place = semanticPlace(expression);
    if (place.root)
      return rawPointerFieldsAt(placeKey(place));
    std::vector<ProjectedRawPointerOrigins> result;
    for (const auto &origin : rawPointerOrigins(rawObject))
      if (!origin.parameterPointee && origin.loopInput == 0) {
        auto fields = rawPointerFieldsAt(origin.place);
        for (auto &field : fields)
          for (auto &forwarded : field.origins)
            forwarded.possible |= origin.possible;
        mergeRawPointerFields(result, fields);
      }
    return result;
  }
  if (semanticModel.typeOf(*expression).kind == SemanticType::RawPointer)
    return {{{}, rawPointerOrigins(expression)}};
  if (const auto *lambda = exprAs<Lambda>(expression)) {
    std::vector<ProjectedRawPointerOrigins> result;
    if (const auto *info = semanticModel.findLambda(*lambda))
      for (const auto &capture : info->captures) {
        auto fields = rawPointerFields(capture.initializer);
        for (auto &field : fields)
          field.projections.insert(field.projections.begin(),
                                   {.kind = PlaceProjectionKind::Field,
                                    .field = capture.bindingSymbol});
        mergeRawPointerFields(result, fields);
      }
    return result;
  }
  if (const auto *conditional = exprAs<ConditionalExpr>(expression)) {
    auto fields = rawPointerFields(conditional->thenExpression().get());
    mergeRawPointerFields(
        fields, rawPointerFields(conditional->elseExpression().get()));
    for (auto &field : fields)
      for (auto &origin : field.origins)
        origin.possible = true;
    return fields;
  }
  const auto place = semanticPlace(expression);
  if (place.root)
    return rawPointerFieldsAt(placeKey(place));
  if (const auto *member = exprAs<Get>(expression);
      member && member->access().kind != TokenKind::ARROW) {
    if (const auto *field =
            findMember(memberAccessObjectType(*member), member->name())) {
      std::vector<ProjectedRawPointerOrigins> result;
      for (auto input : rawPointerFields(member->object().get()))
        if (!input.projections.empty() &&
            input.projections.front().kind == PlaceProjectionKind::Field &&
            input.projections.front().field ==
                toolingSymbolFor(field->symbol)) {
          input.projections.erase(input.projections.begin());
          mergeRawPointerFields(result, {input});
        }
      return result;
    }
  }
  if (const auto *index = exprAs<Index>(expression)) {
    std::vector<ProjectedRawPointerOrigins> result;
    for (auto input : rawPointerFields(index->object().get()))
      if (!input.projections.empty() &&
          (input.projections.front().kind ==
               PlaceProjectionKind::ConstantIndex ||
           input.projections.front().kind ==
               PlaceProjectionKind::DynamicIndex)) {
        input.projections.erase(input.projections.begin());
        mergeRawPointerFields(result, {input});
      }
    // An array-valued temporary has no named place; conservatively join its
    // possible element fields using the ordinary index projection family.
    if (semanticModel.typeOf(*index->object()).kind == SemanticType::Array)
      return result;
  }
  if (const auto *construction = semanticModel.findConstruction(*expression)) {
    // Generated copy/move construction preserves the source's projection map.
    if (construction->directInitialization ||
        construction->kind == ConstructorKind::Copy ||
        construction->kind == ConstructorKind::Move) {
      if (const auto *call = exprAs<Call>(expression);
          call && call->arguments().size() == 1)
        return rawPointerFields(call->arguments().front().get());
      if (const auto *direct = exprAs<DirectInitializer>(expression);
          direct && direct->arguments().size() == 1)
        return rawPointerFields(direct->arguments().front().get());
    }
    if (!construction->designatedArguments.empty()) {
      const auto *owner = classInfo(construction->constructedType);
      std::vector<ProjectedRawPointerOrigins> result;
      if (!owner ||
          owner->fields.size() != construction->designatedArguments.size())
        return result;
      for (std::size_t index = 0; index < owner->fields.size(); ++index) {
        auto fields =
            rawPointerFields(construction->designatedArguments[index]);
        const auto &field = *owner->fields[index].declaration;
        const auto member = owner->members.find(field.name().lexeme);
        if (member == owner->members.end())
          continue;
        for (auto &entry : fields)
          entry.projections.insert(
              entry.projections.begin(),
              {.kind = PlaceProjectionKind::Field,
               .field = toolingSymbolFor(member->second.symbol)});
        mergeRawPointerFields(result, fields);
      }
      return result;
    }
  }
  if (const auto *array = exprAs<ArrayInitializer>(expression)) {
    std::vector<ProjectedRawPointerOrigins> result;
    for (std::size_t index = 0; index < array->elements().size(); ++index) {
      auto fields = rawPointerFields(array->elements()[index].get());
      for (auto &entry : fields)
        entry.projections.insert(
            entry.projections.begin(),
            {.kind = PlaceProjectionKind::ConstantIndex, .index = index});
      mergeRawPointerFields(result, fields);
    }
    return result;
  }
  if (exprAs<Call>(expression) || semanticModel.findConstruction(*expression)) {
    const auto *call = exprAs<Call>(expression);
    const auto *resolved = call ? semanticModel.findCall(*call) : nullptr;
    const auto *construction = semanticModel.findConstruction(*expression);
    const auto query = construction ? rawConstructorResultQuery(*construction)
                       : resolved   ? rawResultQuery(*call, *resolved)
                                    : nullptr;
    auto produced = rawProducedFields(*expression);
    const bool unresolved =
        std::any_of(produced.begin(), produced.end(), [](const auto &field) {
          return std::any_of(field.origins.begin(), field.origins.end(),
                             [](const auto &origin) {
                               return origin.sourceResultQuery != 0 ||
                                      origin.loopInput != 0;
                             });
        });
    // A completed ordinary query owns its projected output map. The older
    // input union is a fallback, not permission to contaminate every exact
    // output field with every unrelated argument or receiver-shell address.
    if (query && query->complete && query->valid && !unresolved)
      return produced;
    const auto origins = rawCallPointerOrigins(*expression, false);
    std::vector<ProjectedRawPointerOrigins> result;
    if (!origins.empty())
      for (auto path : rawPointerFieldPaths(semanticModel.typeOf(*expression)))
        result.push_back({std::move(path), origins});
    mergeRawPointerFields(result, produced);
    return result;
  }
  std::vector<ProjectedRawPointerOrigins> unresolved;
  std::vector<RawPointerOrigin> available;
  if (const auto *index = exprAs<Index>(expression))
    for (const auto &field : rawPointerFields(index->object().get()))
      mergeRawPointerOrigins(available, field.origins);
  else if (const auto *member = exprAs<Get>(expression))
    for (const auto &field : rawPointerFields(member->object().get()))
      mergeRawPointerOrigins(available, field.origins);
  else if (const auto *assignment = exprAs<Assign>(expression))
    for (const auto &field : rawPointerFields(assignment->value().get()))
      mergeRawPointerOrigins(available, field.origins);
  else if (const auto *assignment = exprAs<Set>(expression))
    for (const auto &field : rawPointerFields(assignment->value().get()))
      mergeRawPointerOrigins(available, field.origins);
  mergeRawPointerOrigins(
      available,
      {{.addressSite = tokenSpan(expressionToken(*expression)),
        .sourceResultQuery = requestSession.collectingRawResult
                                 ? requestSession.collectingRawResult->identity
                                 : std::numeric_limits<std::size_t>::max()}});
  for (auto &origin : available)
    origin.possible = true;
  for (auto path : rawPointerFieldPaths(semanticModel.typeOf(*expression)))
    unresolved.push_back({std::move(path), available});
  return unresolved;
}

auto SemanticVisitor::Impl::rawPointerFieldsAt(const PlaceKey &place)
    -> std::vector<ProjectedRawPointerOrigins> {
  if (!rawAuditEnabled())
    return {};
  std::vector<ProjectedRawPointerOrigins> result;
  Symbol *root = rawPointerRoot(place);
  if (!root)
    return result;
  const SemanticPlace selected{.root = root,
                               .rootIdentity = place.root,
                               .projections = place.projections,
                               .receiver = place.receiver};
  // Stored checked references borrow separate storage. Resolve a concrete
  // retained loan before looking in the carrier's owned field map.
  const auto referent = loanPlace(selected);
  if (referent.valid() &&
      (referent.root != place.root || referent.receiver != place.receiver ||
       referent.projections != place.projections))
    return rawPointerFieldsAt(referent);
  auto selectedType = root->type;
  bool borrowedStorage = false;
  for (const auto &projection : place.projections) {
    if (selectedType.kind == SemanticType::Reference &&
        selectedType.arguments.size() == 1)
      selectedType = SemanticType(selectedType.arguments.front());
    if (projection.kind == PlaceProjectionKind::Field) {
      const auto *field = semanticModel.database().findSymbol(projection.field);
      selectedType = field ? field->type : SemanticType::Unknown;
      borrowedStorage |= selectedType.kind == SemanticType::Reference;
    } else if (!selectedType.arguments.empty()) {
      selectedType = SemanticType(selectedType.arguments.front());
    } else {
      selectedType = SemanticType::Unknown;
    }
  }
  bool weak = false;
  for (const auto &storage :
       rawPointerStoragePlaces({.root = root,
                                .rootIdentity = place.root,
                                .projections = place.projections,
                                .receiver = place.receiver},
                               weak)) {
    if (storage.projections.empty() &&
        storage.root->type.kind == SemanticType::RawPointer) {
      mergeRawPointerFields(result, {{{}, storage.root->rawPointerOrigins}});
      continue;
    }
    for (const auto &field : storage.root->rawPointerFields) {
      if (field.projections.size() < storage.projections.size())
        continue;
      auto prefix = field.projections;
      prefix.resize(storage.projections.size());
      const auto relation =
          placeRelation(placeKey(storage), projectedPlaceKey(storage, prefix));
      weak |= relation.relation == PlaceRelation::MayAlias;
      if (relation.relation == PlaceRelation::Equal ||
          relation.relation == PlaceRelation::MayAlias)
        mergeRawPointerFields(
            result, {{{field.projections.begin() + storage.projections.size(),
                       field.projections.end()},
                      field.origins}});
    }
  }
  if (borrowedStorage && result.empty())
    for (auto path : rawPointerFieldPaths(selectedType))
      result.push_back({std::move(path),
                        {{.addressSite = tokenSpan(root->declaration),
                          .sourceResultQuery =
                              requestSession.collectingRawResult
                                  ? requestSession.collectingRawResult->identity
                                  : std::numeric_limits<std::size_t>::max()}}});
  if (weak)
    for (auto &field : result)
      for (auto &origin : field.origins)
        origin.possible = true;
  return result;
}

auto SemanticVisitor::Impl::rawPointerFieldPaths(const SemanticType &type)
    -> std::vector<std::vector<PlaceProjection>> {
  if (!rawAuditEnabled())
    return {};
  std::vector<SemanticType> active;
  const auto visit =
      [&](const auto &self, const SemanticType &current,
          bool root) -> std::vector<std::vector<PlaceProjection>> {
    if (current.kind == SemanticType::RawPointer)
      return {{}};
    // A reference parameter denotes its referent. References stored inside
    // that referent are separate borrowed storage, not fields owned by its
    // shell; following them also turns recursive reference graphs into cycles.
    if (current.kind == SemanticType::Reference)
      return root && current.arguments.size() == 1
                 ? self(self, current.arguments.front(), false)
                 : std::vector<std::vector<PlaceProjection>>{};
    if (current.kind == SemanticType::Array && current.arguments.size() == 1) {
      auto paths = self(self, current.arguments.front(), false);
      for (auto &path : paths)
        path.insert(path.begin(), {.kind = PlaceProjectionKind::DynamicIndex});
      return paths;
    }
    if (current.kind == SemanticType::Enum) {
      if (active.size() >= 128 ||
          std::find(active.begin(), active.end(), current) != active.end())
        return {};
      active.push_back(current);
      std::vector<std::vector<PlaceProjection>> result;
      const auto append = [&](const SemanticType &payload) {
        for (auto path : self(self, payload, false))
          if (std::find(result.begin(), result.end(), path) == result.end())
            result.push_back(std::move(path));
      };
      if (const auto info = enumTypeFor(current))
        for (const auto &variant : info->enumerators)
          for (const auto &payload : variant.payloadTypes)
            append(payload);
      active.pop_back();
      return result;
    }
    if (current.kind == SemanticType::Lambda && current.hasLambdaShape()) {
      std::vector<std::vector<PlaceProjection>> result;
      if (const auto *lambda = semanticModel.findLambda(current.lambdaId)) {
        const auto captures = current.lambdaCaptureTypes();
        for (std::size_t index = 0;
             index < captures.size() && index < lambda->captures.size();
             ++index) {
          auto paths = self(self, captures[index], false);
          for (auto &path : paths) {
            path.insert(path.begin(),
                        {.kind = PlaceProjectionKind::Field,
                         .field = lambda->captures[index].bindingSymbol});
            result.push_back(std::move(path));
          }
        }
      }
      return result;
    }
    if (current.kind != SemanticType::Class)
      return {};
    // Invalid by-value recursive types are diagnosed by type/lifecycle
    // analysis. Provenance collection must also terminate during recovery,
    // including a generic recursion that grows a fresh type each step.
    if (active.size() >= 128 ||
        std::find(active.begin(), active.end(), current) != active.end())
      return {};
    const auto *owner = classInfo(current);
    if (!owner)
      return {};
    active.push_back(current);
    std::vector<std::vector<PlaceProjection>> result;
    for (const auto &base : owner->bases)
      if (!base.interface) {
        auto paths = self(
            self, substituteType(base.type, classSubstitution(current)), false);
        result.insert(result.end(), paths.begin(), paths.end());
      }
    for (const auto &field : owner->fields) {
      const auto member = owner->members.find(field.declaration->name().lexeme);
      if (member == owner->members.end())
        continue;
      auto paths = self(
          self, substituteSymbol(member->second.symbol, current).type, false);
      for (auto &path : paths) {
        path.insert(path.begin(),
                    {.kind = PlaceProjectionKind::Field,
                     .field = toolingSymbolFor(member->second.symbol)});
        result.push_back(std::move(path));
      }
    }
    active.pop_back();
    return result;
  };
  return visit(visit, type, true);
}

auto SemanticVisitor::Impl::rawPointerTypesAt(
    const SemanticType &type, const std::vector<PlaceProjection> &projections)
    -> std::vector<SemanticType> {
  if (!rawAuditEnabled())
    return {};
  std::vector<std::pair<SemanticType, std::size_t>> visited;
  std::vector<SemanticType> result;
  const auto visit = [&](const auto &self, SemanticType current,
                         std::size_t index) -> void {
    if (current.kind == SemanticType::Reference &&
        current.arguments.size() == 1)
      current = SemanticType(current.arguments.front());
    const auto key = std::pair{current, index};
    if (visited.size() >= 128 ||
        std::find(visited.begin(), visited.end(), key) != visited.end())
      return;
    visited.push_back(key);
    if (current.kind == SemanticType::Enum) {
      if (const auto info = enumTypeFor(current))
        for (const auto &variant : info->enumerators)
          for (const auto &payload : variant.payloadTypes)
            self(self, payload, index);
      return;
    }
    if (index == projections.size()) {
      if (current.kind == SemanticType::RawPointer &&
          std::find(result.begin(), result.end(), current) == result.end())
        result.push_back(std::move(current));
      return;
    }
    const auto &projection = projections[index];
    if (projection.kind != PlaceProjectionKind::Field) {
      if (!current.arguments.empty())
        self(self, current.arguments.front(), index + 1);
    } else if (current.kind == SemanticType::Lambda &&
               current.hasLambdaShape()) {
      if (const auto *lambda = semanticModel.findLambda(current.lambdaId)) {
        const auto captures = current.lambdaCaptureTypes();
        for (std::size_t capture = 0;
             capture < captures.size() && capture < lambda->captures.size();
             ++capture)
          if (lambda->captures[capture].bindingSymbol == projection.field)
            self(self, captures[capture], index + 1);
      }
    } else if (const auto *owner = classInfo(current)) {
      for (const auto &[_, member] : owner->members)
        // This is a lookup of an already-identified field, not symbol
        // discovery. Registering every member here gives optional auditing
        // authority over later method/local SymbolIds and mandatory MIR.
        if (member.symbol.bindingKind == SemanticBindingKind::Field &&
            symbolForDeclaration(member.symbol.declaration) == projection.field)
          self(self, substituteSymbol(member.symbol, current).type, index + 1);
      for (const auto &base : owner->bases)
        if (!base.interface)
          self(self, substituteType(base.type, classSubstitution(current)),
               index);
    }
  };
  visit(visit, type, 0);
  return result;
}

auto SemanticVisitor::Impl::rawPointerRoot(const PlaceKey &place) -> Symbol * {
  if (place.receiver)
    return receiverStateSymbol();
  if (place.root == 0)
    return nullptr;
  // A binding names this root exactly when its declaration token carries the
  // key the identity was recorded under: the database maps each declaration
  // key (source unit, name span, generated lexeme) to one identity and keeps
  // that key on the identity's record. Comparing the record's fields costs an
  // integer compare per candidate, where `symbolForDeclaration` hashed a key
  // per candidate, and a miss visits every namespace symbol of every unit.
  const SymbolRecord *record = semanticModel.database().findSymbol(place.root);
  if (record == nullptr)
    return nullptr;
  const std::string_view generatedName =
      record->generated ? std::string_view{record->name} : std::string_view{};
  const auto find = [&](auto &bindings) -> Symbol * {
    for (auto &[_, symbol] : bindings) {
      const Token &name = symbol.declaration;
      if (name.position != record->nameSpan.start)
        continue;
      const std::string_view tokenGeneratedName =
          name.generated ? std::string_view{name.lexeme} : std::string_view{};
      if (tokenSpan(name).end == record->nameSpan.end &&
          tokenGeneratedName == generatedName &&
          sourceUnitFor(name) == record->sourceUnit)
        return &symbol;
    }
    return nullptr;
  };
  // A class static field's facts live in its flow-state carrier, not in the
  // class-scope copy that a member body also has in scope.
  if (record->staticMember)
    if (auto *symbol = find(valueFlow.staticFieldSymbols))
      return symbol;
  for (auto &scope : scopes)
    if (auto *symbol = find(scope))
      return symbol;
  // A source-unit view owns the current flow facts when source loading is
  // active. Its declaration-index copy may still contain the initializer.
  if (auto *bindings = currentNamespaceSymbolsMutable())
    if (auto *symbol = find(*bindings))
      return symbol;
  if (auto *symbol = find(valueFlow.namespaceSymbols))
    return symbol;
  for (auto &[_, bindings] : valueFlow.internalNamespaceSymbols)
    if (auto *symbol = find(bindings))
      return symbol;
  for (auto &[_, bindings] : valueFlow.visibleNamespaceSymbols)
    if (auto *symbol = find(bindings))
      return symbol;
  return nullptr;
}

auto SemanticVisitor::Impl::seedRawStorageInput(Symbol &symbol) -> void {
  const auto place = placeKey(placeForSymbol(symbol));
  if (!place.valid())
    return;
  symbol.rawPointerOrigins.clear();
  symbol.rawPointerFields.clear();
  for (auto path : rawPointerFieldPaths(symbol.type)) {
    auto input = place;
    input.projections.insert(input.projections.end(), path.begin(), path.end());
    RawPointerOrigin origin{.place = std::move(input),
                            .addressSite = tokenSpan(symbol.declaration),
                            .parameterPointee = true};
    if (path.empty() && symbol.type.kind == SemanticType::RawPointer)
      symbol.rawPointerOrigins.push_back(std::move(origin));
    else
      symbol.rawPointerFields.push_back({std::move(path), {std::move(origin)}});
  }
}

auto SemanticVisitor::Impl::recordRawResult(const ExprPtr &expression) -> void {
  if (!rawAuditEnabled())
    return;
  const auto query = requestSession.collectingRawResult;
  const auto *function = currentFunctionInfo();
  if (!query || !function || function->id != query->function ||
      lambdaDepth != 0)
    return;
  const auto fields =
      expression && recordsDeclarationConversion(*expression)
          ? contextualConstructionRawFields(*expression, currentReturnType)
          : rawPointerFields(expression.get());
  collectRawResultFields(fields, function->bodyDeclaration()->parameters(),
                         function->parameterTypes, false);
}

auto SemanticVisitor::Impl::recordRawConstructorResult(
    const ConstructorDecl &constructor) -> void {
  const auto query = requestSession.collectingRawResult;
  const auto *info = semanticModel.findConstructor(constructor);
  if (!query || !info || info->id != query->constructor)
    return;
  std::vector<SemanticType> parameterTypes;
  for (const auto &parameter : constructor.parameters())
    parameterTypes.push_back(typeOf(parameter));
  collectRawResultFields(rawPointerFieldsAt({.receiver = true}),
                         constructor.parameters(), parameterTypes, true);
}

auto SemanticVisitor::Impl::recordEmptyReturnRawResult(
    const ReturnStmt &statement) -> void {
  if (!rawAuditEnabled())
    return;
  const auto query = requestSession.collectingRawResult;
  const auto *function = currentFunctionInfo();
  if (!query || !function || function->id != query->function ||
      lambdaDepth != 0)
    return;
  // A default constructor may create arbitrary addresses. Until its body is
  // queried, absence of a source argument must not imply proven-safe fields.
  std::vector<ProjectedRawPointerOrigins> fields;
  for (auto path : rawPointerFieldPaths(currentReturnType))
    mergeRawPointerFields(
        fields,
        {{std::move(path),
          {{.addressSite = tokenSpan(statement.keyword()),
            .sourceResultQuery = std::numeric_limits<std::size_t>::max()}}}});
  collectRawResultFields(fields, function->bodyDeclaration()->parameters(),
                         function->parameterTypes, false);
}

auto SemanticVisitor::Impl::collectRawResultFields(
    const std::vector<ProjectedRawPointerOrigins> &fields,
    const std::vector<Parameter> &parameters,
    const std::vector<SemanticType> &parameterTypes, bool constructedReceiver,
    std::vector<RawResultField> *outputRecords) -> void {
  const auto query = requestSession.collectingRawResult;
  for (const auto &field : fields) {
    RawResultField output{.projections = field.projections};
    for (auto origin : field.origins) {
      RawResultOrigin result{.origin = origin};
      if (origin.loopInput != 0) {
        // A return observed before its loop closes is not a completed query.
        // Retain uncertainty until an actual access, rather than asserting a
        // foreign address or rejecting an inert return/copy.
        result.origin.loopInput = 0;
        result.origin.sourceResultQuery = query->identity;
        result.origin.place = {};
      } else if (origin.place.receiver) {
        if (constructedReceiver) {
          result.origin.place = {};
          result.origin.sourceResultQuery = query->identity;
        } else {
          result.receiver = true;
          result.pointerInput = origin.parameterPointee;
        }
      } else {
        for (std::size_t index = 0; index < parameters.size(); ++index) {
          if (symbolForDeclaration(parameters[index].name) != origin.place.root)
            continue;
          if (origin.parameterPointee ||
              (index < parameterTypes.size() &&
               parameterTypes[index].kind == SemanticType::Reference)) {
            result.parameter = index;
            result.pointerInput = origin.parameterPointee;
          }
          break;
        }
        if (!result.parameter && !origin.sourceResultQuery) {
          const auto *symbol =
              semanticModel.database().findSymbol(origin.place.root);
          const auto *binding = rawPointerRoot(origin.place);
          result.globalInput =
              origin.parameterPointee && binding &&
              (binding->bindingKind == SemanticBindingKind::GlobalVariable ||
               binding->bindingKind == SemanticBindingKind::StaticField);
          if (symbol && (symbol->kind == SymbolKind::LocalVariable ||
                         symbol->kind == SymbolKind::Parameter))
            result.origin.expired = true;
        }
      }
      // Loan identities belong to the producing body. Formal sources acquire
      // the actual caller's identity when the result is instantiated.
      result.origin.throughLoan = 0;
      output.origins.push_back(std::move(result));
    }
    (outputRecords ? *outputRecords : query->fields)
        .push_back(std::move(output));
  }
}

auto SemanticVisitor::Impl::rawResultQuery(const Call &call,
                                           const ResolvedCallInfo &resolved)
    -> std::shared_ptr<RawResultQuery> {
  auto owner = resolved.dispatchOwner;
  if (owner.kind != SemanticType::Class) {
    if (const auto *member = exprAs<Get>(call.callee().get()))
      owner = memberAccessObjectType(*member);
    else if (currentClass)
      owner = openClassType(*currentClass);
  }
  return rawResultQuery(resolved, std::move(owner));
}

auto SemanticVisitor::Impl::rawResultQuery(const ResolvedCallInfo &resolved,
                                           SemanticType owner)
    -> std::shared_ptr<RawResultQuery> {
  if (!rawAuditEnabled())
    return nullptr;
  const auto *function = semanticModel.findFunction(resolved.function);
  if (!function || !function->declaration ||
      function->linkage != LanguageLinkage::Gti ||
      function->intrinsic != IntrinsicKind::None ||
      function->declaration->runtimeBinding())
    return nullptr;
  // A bodyless source dispatch signature is not a foreign implementation.
  // Check before cache lookup: a static query for the same method must not
  // accidentally become a complete summary of its whole dispatch family.
  if (resolved.dispatch != CallDispatch::Static) {
    auto entry = std::make_shared<RawResultQuery>();
    entry->function = resolved.function;
    entry->identity = std::numeric_limits<std::size_t>::max();
    entry->complete = true;
    entry->valid = false;
    entry->writesComplete = false;
    return entry;
  }
  if (!function->bodyDeclaration()->body())
    return nullptr;
  if (function->ownerClass == 0)
    owner = SemanticType::Unknown;
  // Symbolic primary bodies are checked again with concrete arguments. Do not
  // cache a primary's provisional facts under a concrete instance's identity.
  if (containsOpenTypeParameter(resolved.returnType) ||
      containsOpenTypeParameter(owner) ||
      std::any_of(resolved.typeArguments.begin(), resolved.typeArguments.end(),
                  containsOpenTypeParameter) ||
      std::any_of(resolved.parameterTypes.begin(),
                  resolved.parameterTypes.end(), containsOpenTypeParameter))
    return nullptr;
  if (!requestSession.rawResultQueries)
    requestSession.rawResultQueries = std::make_shared<RawResultQueryState>();
  auto state = requestSession.rawResultQueries;
  const auto ownerTypes = owner.kind == SemanticType::Class
                              ? owner.arguments
                              : std::vector<SemanticType>{};
  const auto ownerValues = owner.kind == SemanticType::Class
                               ? owner.valueArguments
                               : std::vector<CompileTimeValue>{};
  const bool topLevel = state->depth == 0;
  const auto &candidates = topLevel ? state->cachedRoots : state->familyEntries;
  for (const auto &entry : candidates)
    if ((!topLevel || entry->topLevel) &&
        entry->function == resolved.function &&
        entry->ownerTypes == ownerTypes && entry->ownerValues == ownerValues &&
        entry->types == resolved.typeArguments &&
        entry->values == resolved.valueArguments)
      return entry;
  if (topLevel)
    state->familyEntries.clear();
  auto entry = std::make_shared<RawResultQuery>();
  entry->function = resolved.function;
  entry->linearWrites =
      rawWritesAreLinear(function->bodyDeclaration()->body()->statements());
  entry->ownerTypes = ownerTypes;
  entry->ownerValues = ownerValues;
  entry->types = resolved.typeArguments;
  entry->values = resolved.valueArguments;
  entry->identity = state->nextIdentity++;
  entry->topLevel = topLevel;
  if (state->familyEntries.size() >= 2048 || state->depth >= 32) {
    entry->complete = true;
    return entry;
  }
  state->familyEntries.push_back(entry);
  runRawResultQuery(entry);
  // The current projected-input contract has one slot per ordinary formal.
  // An expanded pack needs a per-element substitution before it can claim a
  // complete raw-output summary. Keep any proved origins, but not an empty
  // precision claim that could erase a packed input's known facts.
  if (function->parameterPack) {
    entry->valid = false;
    entry->writesComplete = false;
  }
  return entry;
}

auto SemanticVisitor::Impl::rawConstructorResultQuery(
    const ResolvedConstructionInfo &resolved)
    -> std::shared_ptr<RawResultQuery> {
  if (!rawAuditEnabled())
    return nullptr;
  if ((!resolved.generatedDefault &&
       (!resolved.declaration || !resolved.declaration->body() ||
        resolved.kind != ConstructorKind::Ordinary)) ||
      containsOpenTypeParameter(resolved.constructedType) ||
      std::any_of(resolved.typeArguments.begin(), resolved.typeArguments.end(),
                  containsOpenTypeParameter))
    return nullptr;
  if (!requestSession.rawResultQueries)
    requestSession.rawResultQueries = std::make_shared<RawResultQueryState>();
  auto state = requestSession.rawResultQueries;
  const bool topLevel = state->depth == 0;
  const auto &candidates = topLevel ? state->cachedRoots : state->familyEntries;
  for (const auto &entry : candidates)
    if ((!topLevel || entry->topLevel) &&
        entry->constructor == resolved.constructor && entry->function == 0 &&
        entry->defaultOwner == (resolved.generatedDefault
                                    ? resolved.constructedType.classId
                                    : 0) &&
        entry->ownerTypes == resolved.constructedType.arguments &&
        entry->ownerValues == resolved.constructedType.valueArguments &&
        entry->types == resolved.typeArguments &&
        entry->values == resolved.valueArguments)
      return entry;
  if (topLevel)
    state->familyEntries.clear();
  auto entry = std::make_shared<RawResultQuery>();
  entry->constructor = resolved.constructor;
  // Constructor writes remain weak: a body-only linearity test cannot prove
  // dominance across member/base/default-field initializer evaluation.
  entry->defaultOwner =
      resolved.generatedDefault ? resolved.constructedType.classId : 0;
  entry->ownerTypes = resolved.constructedType.arguments;
  entry->ownerValues = resolved.constructedType.valueArguments;
  entry->types = resolved.typeArguments;
  entry->values = resolved.valueArguments;
  entry->identity = state->nextIdentity++;
  entry->topLevel = topLevel;
  if (state->familyEntries.size() >= 2048 || state->depth >= 32) {
    entry->complete = true;
    return entry;
  }
  state->familyEntries.push_back(entry);
  runRawResultQuery(entry);
  if (resolved.declaration &&
      std::any_of(
          resolved.declaration->parameters().begin(),
          resolved.declaration->parameters().end(),
          [](const auto &parameter) { return parameter.pack.has_value(); }))
    entry->valid = false;
  return entry;
}

auto SemanticVisitor::Impl::runRawResultQuery(
    const std::shared_ptr<RawResultQuery> &entry) -> void {
  const auto state = requestSession.rawResultQueries;
  ++state->depth;
  if (workMetrics)
    ++workMetrics->semanticRawAuditQueries;
  // As in constexpr staging, concrete analysis must never reset an active
  // caller's scope/loan state. Reuse the ordinary analyzer in an isolated
  // snapshot; only the normalized result facts leave this worker.
  auto worker = std::make_unique<Impl>(target, sourceGraph, false, workMetrics,
                                       sources, RawPointerAuditMode::Warn);
  // Every snapshot of the sealed catalog is identical, and the instance
  // analysis below asserts the worker's catalog unchanged on exit, so a
  // snapshot released by an earlier query is the same as a fresh one.
  if (state->idleCatalogs.empty()) {
    worker->catalog = catalog.snapshotForInstanceAnalysis();
  } else {
    worker->catalog = std::move(state->idleCatalogs.back());
    state->idleCatalogs.pop_back();
  }
  worker->semanticModel = semanticModel;
  worker->valueFlow = valueFlow;
  // Cached summaries must describe global pointer inputs, not freeze the
  // first caller's current value. Direct addresses of globals stay ordinary
  // absolute places; only raw values loaded from their storage are symbolic.
  const auto seedGlobals = [&](auto &bindings) {
    for (auto &[_, symbol] : bindings)
      if (symbol.bindingKind == SemanticBindingKind::GlobalVariable ||
          symbol.bindingKind == SemanticBindingKind::StaticField)
        worker->seedRawStorageInput(symbol);
  };
  seedGlobals(worker->valueFlow.namespaceSymbols);
  seedGlobals(worker->valueFlow.staticFieldSymbols);
  for (auto &[_, bindings] : worker->valueFlow.internalNamespaceSymbols)
    seedGlobals(bindings);
  for (auto &[_, bindings] : worker->valueFlow.visibleNamespaceSymbols)
    seedGlobals(bindings);
  for (const FunctionDecl *definition : constexprDefinitionsAnalyzed) {
    if (declaredBeforeCurrent(definition))
      worker->constexprDefinitionsAnalyzed.insert(definition);
  }
  for (const auto *definition : constexprConstructorsAnalyzed) {
    if (declaredBeforeCurrent(definition))
      worker->constexprConstructorsAnalyzed.insert(definition);
  }
  worker->genericDefaultConstants = genericDefaultConstants;
  worker->defaultArgumentSyntax = defaultArgumentSyntax;
  worker->defaultArgumentDependencies = defaultArgumentDependencies;
  worker->nextLambdaId = nextLambdaId;
  // Source-summary analysis can require staged constants. Keep their bounded
  // work account across this isolated worker instead of restarting it.
  worker->requestSession.constexprWork = requestSession.constexprWork;
  worker->requestSession.rawResultQueries = state;
  worker->requestSession.collectingRawResult = entry;
  const auto analysis =
      entry->defaultOwner
          ? worker->analyzeClassFieldInitializers(
                entry->defaultOwner, entry->ownerTypes, entry->ownerValues)
      : entry->constructor
          ? worker->analyzeConstructorInstance(
                entry->constructor, entry->ownerTypes, entry->ownerValues,
                entry->types, entry->values)
          : worker->analyzeFunctionInstance(entry->function, entry->ownerTypes,
                                            entry->ownerValues, entry->types,
                                            entry->values);
  state->idleCatalogs.push_back(std::move(worker->catalog));
  entry->valid = analysis.valid();
  if (!entry->valid)
    for (const auto &diagnostic : analysis.diagnostics)
      if (diagnostic.severity == DiagnosticSeverity::Error) {
        entry->failure = diagnostic;
        break;
      }
  entry->complete = true;
  --state->depth;
  if (entry->failure) {
    // Normalized unresolved origins retain their query identity after the
    // private family is discarded. Intern only the diagnostic used by note
    // lookup, not a nested summary or one record per repeated root demand.
    auto found = std::find_if(
        state->entries.begin(), state->entries.end(),
        [&](const auto &evidence) {
          return evidence->failure->code == entry->failure->code &&
                 evidence->failure->message == entry->failure->message &&
                 evidence->failure->primary == entry->failure->primary;
        });
    std::size_t noteIdentity;
    if (found != state->entries.end()) {
      noteIdentity = (*found)->identity;
    } else {
      auto evidence = std::make_shared<RawResultQuery>();
      evidence->identity = state->nextIdentity++;
      evidence->failure = entry->failure;
      noteIdentity = evidence->identity;
      state->entries.push_back(std::move(evidence));
    }
    const auto priorIdentity = entry->identity;
    const auto normalizeOrigin = [&](RawResultOrigin &source) {
      if (source.origin.sourceResultQuery == priorIdentity)
        source.origin.sourceResultQuery = noteIdentity;
    };
    const auto normalizeFields = [&](auto &fields) {
      for (auto &field : fields)
        for (auto &source : field.origins)
          normalizeOrigin(source);
    };
    for (const auto &member : state->familyEntries) {
      normalizeFields(member->fields);
      for (auto &write : member->writes) {
        normalizeOrigin(write.destination);
        normalizeFields(write.fields);
      }
      for (auto &access : member->accesses)
        normalizeFields(access.fields);
    }
    // A separate, completed evidence identity cannot alias an active/successful
    // query. Subsequent import of this failed query uses that same note key.
    entry->identity = noteIdentity;
  }
  if (entry->topLevel && state->cachedRoots.size() < 2048)
    state->cachedRoots.push_back(entry);
  if (entry->topLevel)
    state->familyEntries.clear();
}

auto SemanticVisitor::Impl::rawProducedFields(
    const Expr &expression, const std::vector<RawResultField> *suppliedFields,
    const RawInvocation *invocation, const ResolvedConstructionInfo *contextual)
    -> std::vector<ProjectedRawPointerOrigins> {
  if (!rawAuditEnabled())
    return {};
  // A contextual construction's only written argument is the expression
  // itself; it is never a call or direct initializer of the constructed type.
  const auto *call = contextual ? nullptr : exprAs<Call>(&expression);
  const auto *direct =
      contextual ? nullptr : exprAs<DirectInitializer>(&expression);
  const auto *resolved = invocation ? &invocation->resolved
                         : call     ? semanticModel.findCall(*call)
                                    : nullptr;
  const auto *construction = invocation ? nullptr
                             : contextual
                                 ? contextual
                                 : semanticModel.findConstruction(expression);
  const auto type = construction ? construction->constructedType
                    : resolved   ? resolved->returnType
                                 : semanticModel.typeOf(expression);
  const auto paths = rawPointerFieldPaths(type);
  if (paths.empty() && !suppliedFields)
    return {};
  if (!invocation && call &&
      (semanticModel.findLambdaCall(*call) ||
       semanticModel.findDeferredCallableCall(*call))) {
    std::vector<ProjectedRawPointerOrigins> unresolved;
    for (auto path : paths)
      unresolved.push_back(
          {std::move(path),
           {{.addressSite = tokenSpan(expressionToken(expression)),
             .sourceResultQuery = std::numeric_limits<std::size_t>::max()}}});
    return unresolved;
  }
  const auto query = suppliedFields ? nullptr
                     : invocation
                         ? rawResultQuery(*resolved, invocation->receiverType)
                     : construction ? rawConstructorResultQuery(*construction)
                     : resolved     ? rawResultQuery(*call, *resolved)
                                    : nullptr;
  if (!query && !suppliedFields)
    return {};
  const ResolvedCallArguments arguments =
      invocation ? ResolvedCallArguments(invocation->arguments)
      : call     ? ResolvedCallArguments(*call, resolved)
      : direct   ? ResolvedCallArguments(direct->arguments())
                 : ResolvedCallArguments{};
  const std::size_t argumentCount = contextual ? 1 : arguments.size();
  const auto actualArgument = [&](std::size_t index) -> const Expr * {
    if (contextual)
      return index == 0 ? &expression : nullptr;
    return index < arguments.size() ? arguments[index].get() : nullptr;
  };
  const auto &defaults = construction ? construction->defaultArguments
                                      : resolved->defaultArguments;
  std::vector<ProjectedRawPointerOrigins> result;
  if (query && (!query->complete || !query->valid)) {
    for (auto path : paths)
      result.push_back({std::move(path),
                        {{.addressSite = tokenSpan(expressionToken(expression)),
                          .sourceResultQuery = query->identity}}});
    // Even an incomplete query may have already proved a contradictory local
    // origin. Keep these facts alongside its uncertainty marker.
  }
  for (const auto &field : suppliedFields ? *suppliedFields : query->fields) {
    std::vector<RawPointerOrigin> origins;
    for (const auto &source : field.origins) {
      const Expr *actual = nullptr;
      if (source.parameter && *source.parameter < argumentCount)
        actual = actualArgument(*source.parameter);
      else if (source.parameter &&
               *source.parameter - argumentCount < defaults.size())
        actual = defaults[*source.parameter - argumentCount];
      else if (source.receiver && invocation)
        actual = invocation->receiver;
      else if (source.receiver && call)
        if (const auto *member = exprAs<Get>(call->callee().get()))
          actual = member->object().get();
      if (source.globalInput) {
        auto place = source.origin.place;
        if (place.domain.snapshot != 0)
          place.domain.snapshot = semanticModel.placeSnapshot();
        for (const auto &input : rawPointerFieldsAt(place)) {
          auto forwarded = input.origins;
          for (auto &origin : forwarded) {
            origin.invalidated |= source.origin.invalidated;
            origin.expired |= source.origin.expired;
            applyRawPointerLifetimeInvalidations(
                origin, source.origin.lifetimeInvalidations);
          }
          mergeRawPointerOrigins(origins, forwarded);
        }
      } else if (!source.parameter && !source.receiver) {
        auto origin = source.origin;
        if (origin.place.domain.snapshot != 0)
          origin.place.domain.snapshot = semanticModel.placeSnapshot();
        mergeRawPointerOrigins(origins, {origin});
      } else if (source.pointerInput) {
        const auto fields =
            actual ? rawPointerFields(actual)
            : invocation && invocation->receiverPlace
                ? rawPointerFieldsAt(loanPlace(*invocation->receiverPlace))
                : rawPointerFieldsAt({.receiver = true});
        for (const auto &input : fields) {
          const PlaceKey expected{
              .receiver = true, .projections = source.origin.place.projections};
          const PlaceKey candidate{.receiver = true,
                                   .projections = input.projections};
          const auto relation = placeRelation(expected, candidate);
          if (relation.relation == PlaceRelation::Equal ||
              relation.relation == PlaceRelation::MayAlias) {
            auto forwarded = input.origins;
            for (auto &origin : forwarded) {
              origin.invalidated |= source.origin.invalidated;
              origin.expired |= source.origin.expired;
              applyRawPointerLifetimeInvalidations(
                  origin, source.origin.lifetimeInvalidations);
            }
            mergeRawPointerOrigins(origins, forwarded);
          }
        }
      } else {
        std::vector<RawPointerOrigin> bases;
        if (actual &&
            semanticModel.typeOf(*actual).kind == SemanticType::RawPointer)
          bases = rawPointerOrigins(actual);
        else {
          const auto place = actual ? loanPlace(semanticPlace(actual))
                             : invocation && invocation->receiverPlace
                                 ? loanPlace(*invocation->receiverPlace)
                                 : PlaceKey{.receiver = true};
          if (place.valid())
            bases.push_back(
                {.place = place,
                 .addressSite = source.origin.addressSite,
                 .throughLoan =
                     actual ? accessLoan(semanticPlace(actual)) : 0});
        }
        for (auto &origin : bases) {
          origin.place.projections.insert(
              origin.place.projections.end(),
              source.origin.place.projections.begin(),
              source.origin.place.projections.end());
          origin.invalidated |= source.origin.invalidated;
          origin.expired |= source.origin.expired;
          applyRawPointerLifetimeInvalidations(
              origin, source.origin.lifetimeInvalidations);
          mergeRawPointerOrigins(origins, {origin});
        }
      }
    }
    // Result/access summaries collect possible returns and executions, not
    // an exhaustive path proof. Retain facts without claiming definite use.
    for (auto &origin : origins)
      origin.possible = true;
    mergeRawPointerFields(result, {{field.projections, std::move(origins)}});
  }
  return result;
}

auto SemanticVisitor::Impl::rawCallPointerOrigins(const Expr &expression,
                                                  bool includeProduced)
    -> std::vector<RawPointerOrigin> {
  if (!rawAuditEnabled())
    return {};
  if (const auto *call = exprAs<Call>(&expression)) {
    // A statically selected source call whose result query completed has an
    // exact origin summary: its produced fields name every pointer the result
    // can carry, so the receiver and arguments are not possible origins.
    // Only a producing context may demand that query: a collecting worker
    // (includeProduced == false) must not re-enter its own family.
    const auto *resolved = semanticModel.findCall(*call);
    const auto exactQuery =
        includeProduced && resolved != nullptr &&
                resolved->intrinsic == IntrinsicKind::None &&
                resolved->dispatch == CallDispatch::Static &&
                !semanticModel.findLambdaCall(*call) &&
                !semanticModel.findDeferredCallableCall(*call)
            ? rawResultQuery(*call, *resolved)
            : nullptr;
    if (exactQuery && exactQuery->complete && exactQuery->valid) {
      std::vector<RawPointerOrigin> result;
      for (const auto &field : rawProducedFields(expression))
        mergeRawPointerOrigins(result, field.origins);
      return result;
    }
  }
  // A source call without an exact origin summary may forward any pointer
  // input (including pointer fields), or derive an address from a checked
  // reference/receiver. Keep this union rather than treating the result as a
  // foreign address and silently dropping contradictory known facts.
  std::vector<RawPointerOrigin> result;
  const auto addValue = [&](const Expr *value) {
    for (const auto &field : rawPointerFields(value))
      for (const auto &origin : field.origins) {
        // A call may select a differently typed subobject. The conservative
        // storage-origin union is not an exact output-pointee proof; those
        // type-only facts must come from the selected source-result query.
        if (!origin.place.valid() &&
            origin.pointeeType != SemanticType::Unknown &&
            origin.sourceResultQuery == 0)
          continue;
        mergeRawPointerOrigins(result, {origin});
      }
  };
  const auto addReference = [&](const ExprPtr &value) {
    const auto place = loanPlace(value);
    if (place.valid())
      mergeRawPointerOrigins(result, {{place, tokenSpan(expressionToken(value)),
                                       accessLoan(value)}});
  };
  ResolvedCallArguments arguments;
  const std::vector<SemanticType> *parameters = nullptr;
  if (const auto *call = exprAs<Call>(&expression)) {
    const auto *resolved = semanticModel.findCall(*call);
    arguments = ResolvedCallArguments(*call, resolved);
    if (resolved)
      parameters = &resolved->parameterTypes;
    if (const auto *member = exprAs<Get>(call->callee().get())) {
      addValue(member->object().get());
      if (semanticModel.typeOf(*member->object()).kind !=
          SemanticType::RawPointer)
        addReference(member->object());
    }
  } else if (const auto *direct = exprAs<DirectInitializer>(&expression)) {
    arguments = ResolvedCallArguments(direct->arguments());
  }
  if (const auto *construction = semanticModel.findConstruction(expression))
    parameters = &construction->parameterTypes;
  {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      addValue(arguments[index].get());
      if (parameters && index < parameters->size() &&
          (*parameters)[index].kind == SemanticType::Reference)
        addReference(arguments[index]);
    }
  }
  if (includeProduced)
    for (const auto &field : rawProducedFields(expression))
      mergeRawPointerOrigins(result, field.origins);
  for (auto &origin : result)
    origin.possible = true;
  return result;
}

auto SemanticVisitor::Impl::saveRawProgramStorage()
    -> std::vector<RawProgramStorageState> {
  if (!rawAuditEnabled())
    return {};
  std::vector<RawProgramStorageState> result;
  const auto save = [&](auto &bindings, RawProgramStorageState::Namespace space,
                        SourceUnitId unit = 0) {
    for (auto &[name, symbol] : bindings)
      if ((symbol.bindingKind == SemanticBindingKind::GlobalVariable ||
           symbol.bindingKind == SemanticBindingKind::StaticField) &&
          !rawPointerFieldPaths(symbol.type).empty())
        result.push_back({&symbol, space, unit, name, symbol.rawPointerOrigins,
                          symbol.rawPointerFields});
  };
  save(valueFlow.namespaceSymbols, RawProgramStorageState::Namespace::Global);
  for (auto &[unit, bindings] : valueFlow.internalNamespaceSymbols)
    save(bindings, RawProgramStorageState::Namespace::Internal, unit);
  for (auto &[unit, bindings] : valueFlow.visibleNamespaceSymbols)
    save(bindings, RawProgramStorageState::Namespace::Visible, unit);
  for (auto &[declaration, symbol] : valueFlow.staticFieldSymbols)
    if (!rawPointerFieldPaths(symbol.type).empty())
      result.push_back({.symbol = &symbol,
                        .space = RawProgramStorageState::Namespace::StaticField,
                        .origins = symbol.rawPointerOrigins,
                        .fields = symbol.rawPointerFields,
                        .staticField = declaration});
  return result;
}

auto SemanticVisitor::Impl::restoreRawProgramStorage(
    std::vector<RawProgramStorageState> state) -> void {
  if (!rawAuditEnabled())
    return;
  for (auto &saved : state) {
    if (saved.space == RawProgramStorageState::Namespace::StaticField) {
      const auto found = valueFlow.staticFieldSymbols.find(saved.staticField);
      if (found != valueFlow.staticFieldSymbols.end()) {
        found->second.rawPointerOrigins = std::move(saved.origins);
        found->second.rawPointerFields = std::move(saved.fields);
      }
      continue;
    }
    auto *bindings = &valueFlow.namespaceSymbols;
    if (saved.space != RawProgramStorageState::Namespace::Global) {
      auto &spaces = saved.space == RawProgramStorageState::Namespace::Internal
                         ? valueFlow.internalNamespaceSymbols
                         : valueFlow.visibleNamespaceSymbols;
      const auto found = spaces.find(saved.unit);
      if (found == spaces.end())
        continue;
      bindings = &found->second;
    }
    const auto found = bindings->find(saved.name);
    if (found != bindings->end()) {
      found->second.rawPointerOrigins = std::move(saved.origins);
      found->second.rawPointerFields = std::move(saved.fields);
    }
  }
}

auto SemanticVisitor::Impl::rawWritesAreLinear(const StmtList &statements)
    -> bool {
  // This is only a dominance bound for strong updates, not a body evaluator.
  // All targets and values below still come from ordinary semantic analysis.
  const auto inert = [&](const auto &self, const Expr *expression) -> bool {
    if (!expression)
      return true;
    if (exprAs<LiteralExpr>(expression) || exprAs<Variable>(expression) ||
        exprAs<QualifiedName>(expression))
      return true;
    if (const auto *group = exprAs<Grouping>(expression))
      return self(self, group->expression().get());
    if (const auto *get = exprAs<Get>(expression))
      return self(self, get->object().get());
    if (const auto *unary = exprAs<Unary>(expression))
      return unary->oper().kind == TokenKind::AMPERSAND &&
             self(self, unary->right().get());
    return false;
  };
  for (const auto &statement : statements) {
    if (const auto *block = stmtAs<BlockStmt>(statement.get())) {
      if (!rawWritesAreLinear(block->statements()))
        return false;
    } else if (const auto *variable = stmtAs<VariableDecl>(statement.get())) {
      if (!inert(inert, variable->initializer().get()))
        return false;
    } else if (const auto *expression =
                   stmtAs<ExpressionStmt>(statement.get())) {
      const auto *value = expression->expression().get();
      if (const auto *assign = exprAs<Assign>(value)) {
        if (assign->oper().kind != TokenKind::EQUAL ||
            !inert(inert, assign->value().get()))
          return false;
      } else if (const auto *set = exprAs<Set>(value)) {
        if (!inert(inert, set->object().get()) ||
            !inert(inert, set->value().get()))
          return false;
      } else if (const auto *call = exprAs<Call>(value)) {
        if (!inert(inert, call->callee().get()) ||
            !std::all_of(call->arguments().begin(), call->arguments().end(),
                         [&](const auto &argument) {
                           return inert(inert, argument.get());
                         }))
          return false;
      } else
        return false;
    } else if (!stmtAs<EmptyStmt>(statement.get()))
      return false;
  }
  return true;
}

auto SemanticVisitor::Impl::recordRawWrite(
    const SemanticPlace &place,
    const std::vector<ProjectedRawPointerOrigins> &fields, bool strong)
    -> void {
  if (!rawAuditEnabled())
    return;
  const auto query = requestSession.collectingRawResult;
  const auto *function = currentFunctionInfo();
  if (!query || lambdaDepth || destructorDepth || !place.root)
    return;
  if ((query->constructor || query->defaultOwner) && place.receiver)
    return;
  std::vector<Parameter> noParameters;
  std::vector<SemanticType> parameterTypes;
  const auto *parameters = &noParameters;
  if (query->function) {
    if (!function || query->function != function->id || constructorDepth)
      return;
    parameters = &function->bodyDeclaration()->parameters();
    parameterTypes = function->parameterTypes;
  } else if (query->constructor && currentClass) {
    const auto &constructors = classInfo(*currentClass).constructors;
    const auto constructor = std::find_if(
        constructors.begin(), constructors.end(), [&](const auto &candidate) {
          return candidate.id == query->constructor;
        });
    if (constructor == constructors.end() || !constructor->declaration)
      return;
    parameters = &constructor->declaration->parameters();
    for (const auto &parameter : *parameters)
      parameterTypes.push_back(typeOf(parameter));
  } else if (!query->defaultOwner)
    return;
  RawResultWrite write;
  write.destination.origin.place = placeKey(place);
  write.strong = strong && query->linearWrites;
  bool external = place.receiver;
  write.destination.receiver = place.receiver;
  const bool throughPointer =
      std::any_of(place.projections.begin(), place.projections.end(),
                  [](const auto &projection) {
                    return projection.kind == PlaceProjectionKind::Dereference;
                  });
  write.destination.pointerInput = place.receiver && throughPointer;
  for (std::size_t index = 0; index < parameters->size(); ++index) {
    if (symbolForDeclaration((*parameters)[index].name) !=
        write.destination.origin.place.root)
      continue;
    const auto &type = parameterTypes[index];
    if (type.kind == SemanticType::Reference || throughPointer) {
      write.destination.parameter = index;
      write.destination.pointerInput = throughPointer;
      external = true;
    }
    break;
  }
  external |= place.root->bindingKind == SemanticBindingKind::GlobalVariable ||
              place.root->bindingKind == SemanticBindingKind::StaticField;
  if (!external) {
    if (place.root->type.kind == SemanticType::RawPointer &&
        !place.projections.empty() &&
        place.projections.front().kind == PlaceProjectionKind::Dereference)
      for (const auto &origin : place.root->rawPointerOrigins)
        if (origin.parameterPointee) {
          auto symbolic = place;
          symbolic.root = rawPointerRoot(origin.place);
          symbolic.rootIdentity = origin.place.root;
          symbolic.receiver = origin.place.receiver;
          symbolic.projections.insert(symbolic.projections.begin(),
                                      origin.place.projections.begin(),
                                      origin.place.projections.end());
          if (symbolic.root && symbolic.root != place.root)
            recordRawWrite(symbolic, fields, false);
        }
    return;
  }
  auto type = place.root->type;
  for (const auto &projection : place.projections) {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    if (projection.kind == PlaceProjectionKind::Field) {
      const auto *field = semanticModel.database().findSymbol(projection.field);
      type = field ? field->type : SemanticType::Unknown;
    } else if (!type.arguments.empty())
      type = SemanticType(type.arguments.front());
    else
      type = SemanticType::Unknown;
  }
  if (rawPointerFieldPaths(type).empty())
    return;
  collectRawResultFields(fields, *parameters, parameterTypes,
                         query->constructor != 0, &write.fields);
  query->writes.push_back(std::move(write));
}

auto SemanticVisitor::Impl::applyRawCallWrites(const Expr &expression,
                                               const RawInvocation *invocation)
    -> void {
  if (!rawAuditEnabled())
    return;
  const auto *call = exprAs<Call>(&expression);
  const auto *direct = exprAs<DirectInitializer>(&expression);
  const auto *resolved = invocation ? &invocation->resolved
                         : call     ? semanticModel.findCall(*call)
                                    : nullptr;
  const auto *construction =
      invocation ? nullptr : semanticModel.findConstruction(expression);
  const auto *lambda =
      !invocation && call ? semanticModel.findLambdaCall(*call) : nullptr;
  const auto *deferred = !invocation && call
                             ? semanticModel.findDeferredCallableCall(*call)
                             : nullptr;
  if ((!resolved && !construction && !lambda && !deferred) ||
      (resolved && resolved->intrinsic != IntrinsicKind::None))
    return;
  const auto &parameterTypes = construction ? construction->parameterTypes
                               : resolved   ? resolved->parameterTypes
                               : lambda     ? lambda->parameterTypes
                                            : deferred->parameterTypes;
  const ResolvedCallArguments arguments =
      invocation ? ResolvedCallArguments(invocation->arguments)
      : call     ? ResolvedCallArguments(*call, resolved)
      : direct   ? ResolvedCallArguments(direct->arguments())
                 : ResolvedCallArguments{};
  const auto span =
      tokenSpan(invocation ? invocation->site : expressionToken(expression));
  if (!requestSession.rawResultQueries)
    requestSession.rawResultQueries = std::make_shared<RawResultQueryState>();
  const auto state = requestSession.rawResultQueries;
  // A declaration's inferred/global binding type may be completed after an
  // earlier source body was checked. A negative answer is not yet permanent.
  if (!state->hasRawGlobals || !*state->hasRawGlobals)
    state->hasRawGlobals = !saveRawProgramStorage().empty();
  const bool inputStorage = std::any_of(
      parameterTypes.begin(), parameterTypes.end(), [&](const auto &type) {
        return ((type.kind == SemanticType::Reference ||
                 type.kind == SemanticType::RawPointer) &&
                type.arguments.size() == 1 &&
                !rawPointerFieldPaths(type.arguments.front()).empty()) ||
               (type.kind != SemanticType::RawPointer &&
                !rawPointerFieldPaths(type).empty());
      });
  const auto *member = call ? exprAs<Get>(call->callee().get()) : nullptr;
  const bool receiverStorage =
      resolved && resolved->receiverMode != CallReceiverMode::None &&
      !rawPointerFieldPaths(invocation ? invocation->receiverType
                            : member   ? semanticModel.typeOf(*member->object())
                                       : resolved->dispatchOwner)
           .empty();
  const auto capturedFields = lambda || deferred
                                  ? rawPointerFields(call->callee().get())
                                  : std::vector<ProjectedRawPointerOrigins>{};
  if (!*state->hasRawGlobals && !inputStorage && !receiverStorage &&
      capturedFields.empty())
    return;
  const auto query = invocation
                         ? rawResultQuery(*resolved, invocation->receiverType)
                     : construction ? rawConstructorResultQuery(*construction)
                     : resolved     ? rawResultQuery(*call, *resolved)
                                    : nullptr;
  if (!query && !lambda && !deferred)
    return;
  const bool incomplete =
      !query || !query->complete || !query->valid || !query->writesComplete;
  struct InstantiatedWrite {
    SemanticPlace place;
    std::vector<ProjectedRawPointerOrigins> fields;
    bool strong;
  };
  std::vector<InstantiatedWrite> writes;
  const std::vector<RawResultWrite> noWrites;
  for (const auto &write : query ? query->writes : noWrites) {
    const auto &target = write.destination;
    const Expr *actual = nullptr;
    if (target.parameter && *target.parameter < arguments.size())
      actual = arguments[*target.parameter].get();
    else if (target.parameter) {
      const auto &defaults = construction ? construction->defaultArguments
                                          : resolved->defaultArguments;
      if (*target.parameter - arguments.size() < defaults.size())
        actual = defaults[*target.parameter - arguments.size()];
    } else if (target.receiver && invocation)
      actual = invocation->receiver;
    else if (target.receiver && call)
      if (const auto *member = exprAs<Get>(call->callee().get()))
        actual = member->object().get();
    std::vector<PlaceKey> bases;
    std::vector<RawPointerOrigin> deferredBases;
    auto projections = target.origin.place.projections;
    if (target.pointerInput) {
      const auto dereference = std::find_if(
          projections.begin(), projections.end(), [](const auto &projection) {
            return projection.kind == PlaceProjectionKind::Dereference;
          });
      const PlaceKey selected{
          .receiver = true, .projections = {projections.begin(), dereference}};
      if (dereference == projections.end())
        continue;
      projections.erase(projections.begin(), dereference + 1);
      std::vector<RawPointerOrigin> selectedOrigins;
      const auto inputs =
          actual ? rawPointerFields(actual)
          : invocation && invocation->receiverPlace
              ? rawPointerFieldsAt(loanPlace(*invocation->receiverPlace))
          : target.receiver ? rawPointerFieldsAt({.receiver = true})
                            : std::vector<ProjectedRawPointerOrigins>{};
      for (const auto &input : inputs) {
        const auto relation = placeRelation(
            selected, {.receiver = true, .projections = input.projections});
        if (relation.relation == PlaceRelation::Equal ||
            relation.relation == PlaceRelation::MayAlias)
          mergeRawPointerOrigins(selectedOrigins, input.origins);
      }
      for (const auto &origin : selectedOrigins)
        if (origin.place.valid()) {
          if (origin.loopInput != 0) {
            deferredBases.push_back(origin);
            continue;
          }
          auto base = origin.place;
          if (origin.parameterPointee)
            base.projections.push_back(
                {.kind = PlaceProjectionKind::Dereference});
          else if (auto *root = rawPointerRoot(base);
                   root && root->type.kind == SemanticType::Array &&
                   base.projections.empty())
            base.projections.push_back(
                {.kind = PlaceProjectionKind::DynamicIndex});
          bases.push_back(std::move(base));
        }
    } else if (target.parameter || target.receiver) {
      bases.push_back(actual ? loanPlace(semanticPlace(actual))
                      : invocation && invocation->receiverPlace
                          ? loanPlace(*invocation->receiverPlace)
                          : PlaceKey{.receiver = true});
    } else
      bases.push_back(target.origin.place);
    const auto fields =
        rawProducedFields(expression, &write.fields, invocation);
    for (const auto &origin : deferredBases)
      for (auto &loop : valueFlow.rawPointerLoops)
        if (loop.identity == origin.loopInput)
          loop.writes.push_back({origin, projections, fields});
    for (auto base : bases) {
      if (target.parameter || target.receiver)
        base.projections.insert(base.projections.end(), projections.begin(),
                                projections.end());
      if (auto *root = rawPointerRoot(base))
        writes.push_back({{.root = root,
                           .rootIdentity = base.root,
                           .projections = base.projections,
                           .receiver = base.receiver},
                          fields,
                          write.strong && bases.size() == 1 && !incomplete});
    }
  }
  for (const auto &write : writes)
    applyRawPointerFields(write.place, write.fields, write.strong);
  if (incomplete) {
    if (!capturedFields.empty())
      applyRawAliasWrites(capturedFields, semanticModel.typeOf(*call->callee()),
                          span);
    if (resolved && resolved->receiverMode != CallReceiverMode::None) {
      if (invocation)
        applyRawAliasWrites(
            invocation->receiver ? rawPointerFields(invocation->receiver)
            : invocation->receiverPlace
                ? rawPointerFieldsAt(loanPlace(*invocation->receiverPlace))
                : std::vector<ProjectedRawPointerOrigins>{},
            invocation->receiverType, span);
      else if (member)
        applyRawAliasWrites(rawPointerFields(member->object().get()),
                            semanticModel.typeOf(*member->object()), span);
      else if (auto *receiver = receiverStateSymbol())
        applyRawAliasWrites(receiver->rawPointerFields, receiver->type, span);
    }
    const auto poison = [&](const SemanticPlace &place,
                            const SemanticType &type) {
      applyUnknownRawWrite(place, type, span, query ? query->identity : 0);
    };
    if (*state->hasRawGlobals)
      for (const auto &global : saveRawProgramStorage())
        poison(placeForSymbol(*global.symbol), global.symbol->type);
    for (std::size_t index = 0;
         index < arguments.size() && index < parameterTypes.size(); ++index) {
      const auto &type = parameterTypes[index];
      applyRawAliasWrites(rawPointerFields(arguments[index].get()), type, span);
      if (type.kind == SemanticType::Reference &&
          type.referenceAccess == AccessMode::Mutable)
        poison(semanticPlace(arguments[index]), type);
      else if (type.kind == SemanticType::RawPointer &&
               type.pointerAccess == AccessMode::Mutable &&
               type.arguments.size() == 1)
        for (const auto &origin : rawPointerOrigins(arguments[index]))
          if (auto *root = rawPointerRoot(origin.place))
            poison({.root = root,
                    .rootIdentity = origin.place.root,
                    .projections = origin.place.projections,
                    .receiver = origin.place.receiver},
                   type.arguments.front());
    }
    if (resolved && resolved->receiverMode == CallReceiverMode::MutablePlace) {
      if (invocation)
        poison(invocation->receiver
                   ? semanticPlace(invocation->receiver)
                   : invocation->receiverPlace.value_or(SemanticPlace{}),
               invocation->receiverType);
      else if (const auto *member = exprAs<Get>(call->callee().get()))
        poison(semanticPlace(member->object()),
               semanticModel.typeOf(*member->object()));
      else if (auto *receiver = receiverStateSymbol())
        poison(placeForSymbol(*receiver), receiver->type);
    }
  }
}

auto SemanticVisitor::Impl::applyUnknownRawWrite(const SemanticPlace &place,
                                                 const SemanticType &type,
                                                 const SourceSpan &span,
                                                 std::size_t query) -> void {
  if (!rawAuditEnabled())
    return;
  std::vector<ProjectedRawPointerOrigins> fields;
  for (auto path : rawPointerFieldPaths(type))
    fields.push_back(
        {std::move(path),
         {{.addressSite = span,
           .sourceResultQuery =
               query ? query : std::numeric_limits<std::size_t>::max()}}});
  if (!fields.empty())
    applyRawPointerFields(place, fields, false);
}

auto SemanticVisitor::Impl::applyRawCleanupWrites(
    const SemanticType &type, const SourceSpan &span,
    const std::vector<ProjectedRawPointerOrigins> &fields) -> void {
  if (!rawAuditEnabled())
    return;
  // Cleanup is structurally known, but destructor write transport has no
  // normalized query yet. Preserve uncertainty only when source cleanup can
  // execute, never by executing a destructor body during declaration checking.
  if (!requiresActiveCleanupFor(type))
    return;
  const auto globals = saveRawProgramStorage();
  if (globals.empty() && fields.empty())
    return;
  std::vector<SemanticType> active;
  const auto hasSourceCleanup = [&](const auto &self,
                                    const SemanticType &current) -> bool {
    if (!requiresActiveCleanupFor(current) || active.size() >= 128 ||
        std::find(active.begin(), active.end(), current) != active.end())
      return false;
    active.push_back(current);
    bool result = false;
    if (current.kind == SemanticType::Class) {
      if (const auto *owner = classInfo(current)) {
        result = hasSourceDestructorBody(*owner);
        for (const auto &field : owner->fields) {
          const auto member =
              owner->members.find(field.declaration->name().lexeme);
          if (member != owner->members.end())
            result |= self(
                self, substituteSymbol(member->second.symbol, current).type);
        }
        for (const auto &base : owner->bases)
          if (!base.interface)
            result |= self(
                self, substituteType(base.type, classSubstitution(current)));
      }
    } else if (current.kind == SemanticType::Lambda &&
               current.hasLambdaShape()) {
      for (const auto &capture : current.lambdaCaptureTypes())
        result |= self(self, capture);
    } else if (current.kind == SemanticType::Array ||
               current.kind == SemanticType::UniqueOwner ||
               current.kind == SemanticType::SharedPointer) {
      for (const auto &argument : current.arguments)
        result |= self(self, argument);
    }
    active.pop_back();
    return result;
  };
  if (!hasSourceCleanup(hasSourceCleanup, type))
    return;
  applyRawAliasWrites(fields, type, span);
  for (const auto &global : globals)
    applyUnknownRawWrite(placeForSymbol(*global.symbol), global.symbol->type,
                         span);
}

auto SemanticVisitor::Impl::applyRawAliasWrites(
    std::vector<ProjectedRawPointerOrigins> fields, const SemanticType &type,
    const SourceSpan &span) -> void {
  if (!rawAuditEnabled())
    return;
  // Only aliases actually carried by this value contribute possible write
  // destinations. Copy the small map before updating any aliased carrier.
  for (const auto &field : fields) {
    for (const auto &pointerType : rawPointerTypesAt(type, field.projections)) {
      if (pointerType.kind != SemanticType::RawPointer ||
          pointerType.pointerAccess != AccessMode::Mutable ||
          pointerType.arguments.size() != 1)
        continue;
      for (const auto &origin : field.origins) {
        if (origin.loopInput != 0) {
          std::vector<ProjectedRawPointerOrigins> unresolved;
          for (auto path : rawPointerFieldPaths(pointerType.arguments.front()))
            unresolved.push_back(
                {std::move(path),
                 {{.addressSite = span,
                   .sourceResultQuery =
                       std::numeric_limits<std::size_t>::max()}}});
          for (auto &loop : valueFlow.rawPointerLoops)
            if (loop.identity == origin.loopInput && !unresolved.empty())
              loop.writes.push_back({origin, {}, unresolved});
          continue;
        }
        auto *root = rawPointerRoot(origin.place);
        if (!root)
          continue;
        SemanticPlace destination{.root = root,
                                  .rootIdentity = origin.place.root,
                                  .projections = origin.place.projections,
                                  .receiver = origin.place.receiver};
        if (origin.parameterPointee)
          destination.projections.push_back(
              {.kind = PlaceProjectionKind::Dereference});
        else if (root->type.kind == SemanticType::Array &&
                 destination.projections.empty())
          destination.projections.push_back(
              {.kind = PlaceProjectionKind::DynamicIndex});
        applyUnknownRawWrite(destination, pointerType.arguments.front(), span);
      }
    }
  }
}

auto SemanticVisitor::Impl::preserveRawCallWrites(const Call &call) -> void {
  if (!rawAuditEnabled())
    return;
  const auto *resolved = semanticModel.findCall(call);
  if (!resolved || resolved->intrinsic != IntrinsicKind::None)
    return;
  const bool mutableReference =
      std::any_of(resolved->parameterTypes.begin(),
                  resolved->parameterTypes.end(), [](const auto &type) {
                    return type.kind == SemanticType::Reference &&
                           type.referenceAccess == AccessMode::Mutable;
                  });
  if (!mutableReference &&
      resolved->receiverMode != CallReceiverMode::MutablePlace)
    return;
  if (const auto query = rawResultQuery(call, *resolved);
      query && query->complete && query->valid && query->writesComplete)
    return;
  const auto origins = rawCallPointerOrigins(call, false);
  if (origins.empty())
    return;
  const auto preserve = [&](const ExprPtr &expression,
                            const SemanticType &type) {
    const auto key = loanPlace(expression);
    if (!key.valid())
      return;
    Symbol *root = rawPointerRoot(key);
    if (!root)
      return;
    std::vector<ProjectedRawPointerOrigins> fields;
    for (auto path : rawPointerFieldPaths(type)) {
      path.insert(path.begin(), key.projections.begin(), key.projections.end());
      fields.push_back({std::move(path), origins});
    }
    mergeRawPointerFields(root->rawPointerFields, fields);
  };
  for (std::size_t index = 0;
       index < resolvedCallArgumentCount(call, resolved) &&
       index < resolved->parameterTypes.size();
       ++index) {
    const auto &parameter = resolved->parameterTypes[index];
    if (parameter.kind == SemanticType::Reference &&
        parameter.referenceAccess == AccessMode::Mutable)
      preserve(*resolvedCallArgument(call, resolved, index), parameter);
  }
  if (resolved->receiverMode == CallReceiverMode::MutablePlace)
    if (const auto *member = exprAs<Get>(call.callee().get()))
      preserve(member->object(), semanticModel.typeOf(*member->object()));
}

auto SemanticVisitor::Impl::invalidateRawCallOrigins(
    const Call *call, const RawInvocation *invocation) -> void {
  if (!rawAuditEnabled())
    return;
  const auto *resolved =
      invocation ? &invocation->resolved : semanticModel.findCall(*call);
  if (!resolved)
    return;
  // These exact capabilities only produce a checked borrow; they do not end
  // an object lifetime. A source-defined borrow-returning method is not proof
  // of purity: it may replace its allocation before returning the new object.
  switch (resolved->intrinsic) {
  case IntrinsicKind::RawBorrow:
  case IntrinsicKind::EraseReference:
  case IntrinsicKind::EraseReferenceMut:
  case IntrinsicKind::RestoreReference:
  case IntrinsicKind::RestoreReferenceMut:
  case IntrinsicKind::RawBorrowMut:
  case IntrinsicKind::UniqueOwnerBorrow:
  case IntrinsicKind::UniqueOwnerBorrowMut:
  case IntrinsicKind::SharedOwnerBorrow:
  case IntrinsicKind::OptionalStorageRead:
  case IntrinsicKind::OptionalStorageReadMut:
  case IntrinsicKind::PolymorphicStorageRead:
  case IntrinsicKind::PolymorphicStorageReadMut:
  case IntrinsicKind::MutexGuardRead:
  case IntrinsicKind::MutexGuardReadMut:
    return;
  default:
    break;
  }
  // A bounded source-effect proof, not a privilege attached to borrowed_from:
  // a direct assertion over a raw formal cannot replace any object. Inspect
  // only this normalized syntax shape so the proof is declaration-order
  // independent; ordinary semantic analysis still validates its raw assertion.
  // Even an otherwise empty body can run user code when value parameters drop.
  const auto inertRawAssertion = [&]() {
    const auto *function = semanticModel.findFunction(resolved->function);
    if (!function || !function->explicitReturnBorrowOrigin ||
        function->returnType.kind != SemanticType::Reference ||
        function->virtualMethod || function->linkage != LanguageLinkage::Gti ||
        resolved->dispatch != CallDispatch::Static || !function->declaration ||
        !function->bodyDeclaration()->body() ||
        function->declaration->runtimeBinding() ||
        function->intrinsic != IntrinsicKind::None)
      return false;
    for (const auto &parameter : resolved->parameterTypes)
      if (containsOpenTypeParameter(parameter) ||
          (parameter.kind != SemanticType::Reference &&
           typeTraits(parameter).drop != DropKind::Trivial))
        return false;
    const BlockStmt *block = function->bodyDeclaration()->body().get();
    bool unsafe = function->requiresUnsafe;
    const ReturnStmt *returned = nullptr;
    while (block) {
      unsafe |= block->isUnsafe();
      if (block->statements().size() != 1)
        return false;
      const Stmt *statement = block->statements().front().get();
      returned = stmtAs<ReturnStmt>(statement);
      if (returned)
        break;
      block = stmtAs<BlockStmt>(statement);
    }
    if (!unsafe || !returned)
      return false;
    const Expr *expression = returned->value().get();
    while (const auto *group = exprAs<Grouping>(expression))
      expression = group->expression().get();
    const auto *dereference = exprAs<Unary>(expression);
    if (!dereference || dereference->oper().kind != TokenKind::STAR)
      return false;
    expression = dereference->right().get();
    while (const auto *group = exprAs<Grouping>(expression))
      expression = group->expression().get();
    const auto *variable = exprAs<Variable>(expression);
    if (!variable)
      return false;
    const auto &parameters = function->bodyDeclaration()->parameters();
    for (std::size_t index = 0;
         index < parameters.size() && index < resolved->parameterTypes.size();
         ++index)
      if (parameters[index].name.lexeme == variable->name().lexeme &&
          resolved->parameterTypes[index].kind == SemanticType::RawPointer)
        return true;
    return false;
  };
  if (inertRawAssertion())
    return;
  const auto invalidate = [&](PlaceKey key, SemanticType type) {
    if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
      type = SemanticType(type.arguments.front());
    // Scalar writes update a value, not its object lifetime. Class/array and
    // compiler-owned resource mutation can replace objects or backing storage.
    if (!key.valid() ||
        (type.kind != SemanticType::Class && type.kind != SemanticType::Array &&
         type.kind != SemanticType::UniqueOwner &&
         type.kind != SemanticType::SharedPointer))
      return;
    Symbol *root = rawPointerRoot(key);
    if (root)
      invalidateRawPointerOrigins({.root = root,
                                   .rootIdentity = key.root,
                                   .projections = key.projections,
                                   .receiver = key.receiver});
  };
  const ResolvedCallArguments arguments =
      invocation ? ResolvedCallArguments(invocation->arguments)
                 : ResolvedCallArguments(*call, resolved);
  for (std::size_t index = 0;
       index < arguments.size() && index < resolved->parameterTypes.size();
       ++index) {
    const auto &type = resolved->parameterTypes[index];
    if (type.kind == SemanticType::Reference &&
        type.referenceAccess == AccessMode::Mutable)
      invalidate(loanPlace(arguments[index]), type);
  }
  if (resolved->receiverMode == CallReceiverMode::MutablePlace) {
    if (invocation)
      invalidate(
          invocation->receiver ? loanPlace(semanticPlace(invocation->receiver))
          : invocation->receiverPlace ? loanPlace(*invocation->receiverPlace)
                                      : PlaceKey{},
          invocation->receiverType);
    else if (const auto *member = exprAs<Get>(call->callee().get()))
      invalidate(loanPlace(member->object()),
                 semanticModel.typeOf(*member->object()));
    else if (const Symbol *receiver = receiverStateSymbol())
      invalidate({.receiver = true}, receiver->type);
  }
}

auto SemanticVisitor::Impl::rawPointerStoragePlaces(const SemanticPlace &place,
                                                    bool &weak)
    -> std::vector<SemanticPlace> {
  if (!rawAuditEnabled())
    return {};
  if (!place.root)
    return {};
  const auto typeAt = [&](const SemanticPlace &selected) {
    auto type = selected.root->type;
    for (const auto &projection : selected.projections) {
      if (type.kind == SemanticType::Reference && type.arguments.size() == 1)
        type = SemanticType(type.arguments.front());
      if (projection.kind == PlaceProjectionKind::Field) {
        const auto *field =
            semanticModel.database().findSymbol(projection.field);
        type = field ? field->type : SemanticType::Unknown;
      } else if (!type.arguments.empty()) {
        type = SemanticType(type.arguments.front());
      } else {
        type = SemanticType::Unknown;
      }
    }
    return type;
  };
  auto rootPlace = place;
  rootPlace.projections.clear();
  std::vector<SemanticPlace> places{rootPlace};
  for (const auto &projection : place.projections) {
    std::vector<SemanticPlace> next;
    for (auto selected : places) {
      if (projection.kind != PlaceProjectionKind::Dereference ||
          typeAt(selected).kind != SemanticType::RawPointer) {
        weak |= projection.kind == PlaceProjectionKind::DynamicIndex;
        selected.projections.push_back(projection);
        next.push_back(std::move(selected));
        continue;
      }
      std::vector<RawPointerOrigin> origins;
      if (selected.projections.empty()) {
        origins = selected.root->rawPointerOrigins;
      } else {
        for (const auto &field : selected.root->rawPointerFields) {
          if (field.projections.size() != selected.projections.size())
            continue;
          const auto relation =
              placeRelation(placeKey(selected),
                            projectedPlaceKey(selected, field.projections));
          if (relation.relation == PlaceRelation::Equal ||
              relation.relation == PlaceRelation::MayAlias)
            mergeRawPointerOrigins(origins, field.origins);
        }
      }
      weak |= origins.size() != 1;
      for (const auto &origin : origins) {
        weak |= origin.possible;
        if (origin.parameterPointee || origin.loopInput != 0 ||
            origin.expired || origin.invalidated) {
          weak = true;
          continue;
        }
        Symbol *root = rawPointerRoot(origin.place);
        if (!root) {
          weak = true;
          continue;
        }
        SemanticPlace destination{.root = root,
                                  .rootIdentity = origin.place.root,
                                  .projections = origin.place.projections,
                                  .receiver = origin.place.receiver};
        // Array addresses retain their allocation origin rather than the
        // initial element: arithmetic and raw indexing may select any element.
        if (typeAt(destination).kind == SemanticType::Array) {
          destination.projections.push_back(
              {.kind = PlaceProjectionKind::DynamicIndex});
          weak = true;
        }
        weak |=
            std::any_of(destination.projections.begin(),
                        destination.projections.end(), [](const auto &part) {
                          return part.kind == PlaceProjectionKind::DynamicIndex;
                        });
        next.push_back(std::move(destination));
      }
    }
    places = std::move(next);
  }
  weak |= places.size() != 1;
  return places;
}

auto SemanticVisitor::Impl::assignRawPointerFields(const SemanticPlace &place,
                                                   const Expr *value) -> void {
  applyRawPointerFields(place, rawPointerFields(value), true);
}

auto SemanticVisitor::Impl::applyRawPointerFields(
    const SemanticPlace &place,
    const std::vector<ProjectedRawPointerOrigins> &incoming, bool strong)
    -> void {
  if (!rawAuditEnabled())
    return;
  bool weak = !strong;
  const auto destinations = rawPointerStoragePlaces(place, weak);
  if (destinations.empty())
    recordRawWrite(place, incoming, strong);
  for (const auto &destination : destinations) {
    const bool programStorage =
        destination.root->bindingKind == SemanticBindingKind::GlobalVariable ||
        destination.root->bindingKind == SemanticBindingKind::StaticField;
    // Namespace state is not part of ScopeStack branch snapshots. Conditional
    // writes retain earlier paths, including a loop that never executes.
    const bool destinationWeak =
        weak ||
        (programStorage && (!loanFlow.activeConditionalArms.empty() ||
                            !loanFlow.activeSwitchArms.empty() ||
                            loanFlow.rawConditionalExpressionDepth != 0 ||
                            !valueFlow.rawPointerLoops.empty()));
    recordRawWrite(destination, incoming, !destinationWeak);
    if (destination.projections.empty() &&
        destination.root->type.kind == SemanticType::RawPointer) {
      if (!destinationWeak)
        destination.root->rawPointerOrigins.clear();
      for (const auto &field : incoming)
        if (field.projections.empty())
          mergeRawPointerOrigins(destination.root->rawPointerOrigins,
                                 field.origins);
      if (destinationWeak)
        for (auto &origin : destination.root->rawPointerOrigins)
          origin.possible = true;
      continue;
    }
    // Only an exact destination replaces its old subtree. A write that may
    // select several objects/elements cannot prove any previous fact cleared.
    if (!destinationWeak)
      std::erase_if(destination.root->rawPointerFields, [&](const auto &field) {
        return field.projections.size() >= destination.projections.size() &&
               std::equal(destination.projections.begin(),
                          destination.projections.end(),
                          field.projections.begin());
      });
    auto fields = incoming;
    for (auto &field : fields)
      field.projections.insert(field.projections.begin(),
                               destination.projections.begin(),
                               destination.projections.end());
    mergeRawPointerFields(destination.root->rawPointerFields, fields);
    if (destinationWeak)
      for (auto &field : destination.root->rawPointerFields)
        for (auto &origin : field.origins)
          origin.possible = true;
  }
}

auto SemanticVisitor::Impl::applyRawPointerLifetimeInvalidations(
    RawPointerOrigin &origin, const std::vector<PlaceKey> &places) -> void {
  if (origin.loopInput != 0 || origin.sourceResultQuery != 0) {
    for (const auto &place : places)
      if (std::find(origin.lifetimeInvalidations.begin(),
                    origin.lifetimeInvalidations.end(),
                    place) == origin.lifetimeInvalidations.end())
        origin.lifetimeInvalidations.push_back(place);
    return;
  }
  if (origin.parameterPointee || !origin.place.valid())
    return;
  for (const auto &place : places) {
    const auto relation = placeRelation(place, origin.place);
    if (!relation.compatibleDomain ||
        relation.relation != PlaceRelation::Disjoint)
      origin.invalidated = true;
  }
}

auto SemanticVisitor::Impl::invalidateRawPointerOrigins(
    const SemanticPlace &place) -> void {
  if (!rawAuditEnabled())
    return;
  // Raw origins use the checked-loan analysis-local domain. The snapshot
  // qualified storage identity belongs to ownership state, not this relation;
  // mixing the two makes unrelated local roots conservatively alias.
  const PlaceKey lifetimePlace = loanPlace(place);
  const auto invalidate = [&](auto &origins) {
    for (auto &origin : origins) {
      applyRawPointerLifetimeInvalidations(origin, {lifetimePlace});
    }
  };
  for (auto &scope : scopes)
    for (auto &[_, symbol] : scope) {
      invalidate(symbol.rawPointerOrigins);
      for (auto &field : symbol.rawPointerFields)
        invalidate(field.origins);
    }
  // Update the active source-unit flow, not stale declaration-index copies.
  auto *globals = currentNamespaceSymbolsMutable();
  auto &bindings = globals ? *globals : valueFlow.namespaceSymbols;
  for (auto &[_, symbol] : bindings) {
    invalidate(symbol.rawPointerOrigins);
    for (auto &field : symbol.rawPointerFields)
      invalidate(field.origins);
  }
}

auto SemanticVisitor::Impl::rawPointerOrigins(const ExprPtr &expression)
    -> std::vector<RawPointerOrigin> {
  return rawPointerOrigins(expression.get());
}

auto SemanticVisitor::Impl::rawPointerOrigins(const Expr *expression)
    -> std::vector<RawPointerOrigin> {
  if (!rawAuditEnabled())
    return {};
  if (!expression)
    return {};
  if (const auto found = valueFlow.rawOperatorResults.find(expression);
      found != valueFlow.rawOperatorResults.end()) {
    std::vector<RawPointerOrigin> origins;
    for (const auto &field : found->second)
      mergeRawPointerOrigins(origins, field.origins);
    return origins;
  }
  if (const auto *group = exprAs<Grouping>(expression))
    return rawPointerOrigins(group->expression());
  if (const auto *conversion = exprAs<Conversion>(expression))
    return rawPointerOrigins(conversion->value());
  if (const auto *call = exprAs<Call>(expression);
      call &&
      resolvedIntrinsicKind(*call) == IntrinsicKind::RawPointerConversion &&
      call->arguments().size() == 1) {
    auto origins = rawPointerOrigins(call->arguments().front());
    const auto &source = semanticModel.typeOf(*call->arguments().front());
    if (source.kind == SemanticType::RawPointer &&
        source.arguments.size() == 1 &&
        source.arguments.front() != SemanticType::Void) {
      mergeRawPointerOrigins(origins,
                             {{.addressSite = tokenSpan(call->paren()),
                               .pointeeType = source.arguments.front()}});
    }
    return origins;
  }
  if (const auto *postfix = exprAs<Postfix>(expression))
    return rawPointerOrigins(postfix->expression());
  if (const auto *assignment = exprAs<Assign>(expression)) {
    if (assignment->oper().kind == TokenKind::EQUAL)
      return rawPointerOrigins(assignment->value());
    const auto *symbol = assignment->path().segments.size() > 1
                             ? resolveQualifiedPlaceSymbol(assignment->path())
                             : resolvePlaceSymbol(assignment->name());
    return symbol ? symbol->rawPointerOrigins : std::vector<RawPointerOrigin>{};
  }
  if (const auto *unary = exprAs<Unary>(expression);
      unary && (unary->oper().kind == TokenKind::PLUS_PLUS ||
                unary->oper().kind == TokenKind::MINUS_MINUS))
    return rawPointerOrigins(unary->right());
  if (const auto *unary = exprAs<Unary>(expression);
      unary && unary->oper().kind == TokenKind::AMPERSAND) {
    if (const auto *indirect = exprAs<Unary>(unary->right().get());
        indirect && indirect->oper().kind == TokenKind::STAR &&
        semanticModel.typeOf(*indirect->right()).kind ==
            SemanticType::RawPointer)
      return rawPointerOrigins(indirect->right());
    if (const auto *index = exprAs<Index>(unary->right().get());
        index &&
        semanticModel.typeOf(*index->object()).kind == SemanticType::RawPointer)
      return rawPointerOrigins(index->object());
    if (const auto *member = exprAs<Get>(unary->right().get());
        member && member->access().kind == TokenKind::ARROW &&
        semanticModel.typeOf(*member->object()).kind ==
            SemanticType::RawPointer) {
      bool weak = false;
      const auto places =
          rawPointerStoragePlaces(semanticPlace(unary->right()), weak);
      if (!places.empty()) {
        std::vector<RawPointerOrigin> origins;
        for (const auto &place : places)
          origins.push_back({placeKey(place), tokenSpan(unary->oper()),
                             accessLoan(unary->right())});
        for (auto &origin : origins)
          origin.possible = weak;
        return origins;
      }
      auto origins = rawPointerOrigins(member->object());
      if (const auto *field =
              findMember(memberAccessObjectType(*member), member->name()))
        for (auto &origin : origins) {
          if (origin.place.valid())
            origin.place.projections.push_back(
                {.kind = PlaceProjectionKind::Field,
                 .field = toolingSymbolFor(field->symbol)});
          if (origin.pointeeType != SemanticType::Unknown) {
            const auto addressType = semanticModel.typeOf(*expression);
            if (addressType.kind == SemanticType::RawPointer &&
                addressType.arguments.size() == 1)
              origin.pointeeType = addressType.arguments.front();
          }
        }
      return origins;
    }
    auto place = loanPlace(unary->right());
    // Raw arithmetic can select another element of the same array. Retain
    // the array origin conservatively rather than claiming every later raw
    // access still selects the original constant index.
    if (!place.projections.empty() &&
        (place.projections.back().kind == PlaceProjectionKind::ConstantIndex ||
         place.projections.back().kind == PlaceProjectionKind::DynamicIndex))
      place.projections.pop_back();
    if (place.valid())
      return {{place, tokenSpan(unary->oper()), accessLoan(unary->right())}};
  }
  if (const auto *binary = exprAs<Binary>(expression);
      binary && (binary->oper().kind == TokenKind::PLUS ||
                 binary->oper().kind == TokenKind::MINUS)) {
    auto origins = rawPointerOrigins(binary->left());
    mergeRawPointerOrigins(origins, rawPointerOrigins(binary->right()));
    return origins;
  }
  if (const auto *conditional = exprAs<ConditionalExpr>(expression)) {
    auto origins = rawPointerOrigins(conditional->thenExpression());
    mergeRawPointerOrigins(origins,
                           rawPointerOrigins(conditional->elseExpression()));
    for (auto &origin : origins)
      origin.possible = true;
    return origins;
  }
  if (const auto *call = exprAs<Call>(expression);
      call && resolvedIntrinsicKind(*call) == IntrinsicKind::Move &&
      call->arguments().size() == 1)
    return rawPointerOrigins(call->arguments().front());
  if (exprAs<Call>(expression))
    return rawCallPointerOrigins(*expression);
  if (const auto *member = exprAs<Get>(expression)) {
    std::vector<RawPointerOrigin> result;
    const auto place = semanticPlace(expression);
    if (place.root) {
      for (const auto &field : rawPointerFieldsAt(placeKey(place)))
        if (field.projections.empty())
          mergeRawPointerOrigins(result, field.origins);
      return result;
    }
    const auto *field =
        findMember(memberAccessObjectType(*member), member->name());
    if (!field)
      return result;
    if (member->access().kind != TokenKind::ARROW) {
      for (const auto &candidate : rawPointerFields(member->object().get()))
        if (!candidate.projections.empty() &&
            candidate.projections.front().kind == PlaceProjectionKind::Field &&
            candidate.projections.front().field ==
                toolingSymbolFor(field->symbol))
          mergeRawPointerOrigins(result, candidate.origins);
      return result;
    }
    for (const auto &owner : rawPointerOrigins(member->object())) {
      if (owner.parameterPointee || owner.loopInput != 0)
        continue;
      auto selected = owner.place;
      selected.projections.push_back(
          {.kind = PlaceProjectionKind::Field,
           .field = toolingSymbolFor(field->symbol)});
      for (auto candidate : rawPointerFieldsAt(selected)) {
        for (auto &forwarded : candidate.origins)
          forwarded.possible |= owner.possible;
        mergeRawPointerOrigins(result, candidate.origins);
      }
    }
    return result;
  }
  const auto place = semanticPlace(expression);
  if (place.root) {
    std::vector<RawPointerOrigin> result;
    for (const auto &field : rawPointerFieldsAt(placeKey(place)))
      if (field.projections.empty())
        mergeRawPointerOrigins(result, field.origins);
    return result;
  }
  // Foreign pointers and unsummarized calls carry no local proof. They still
  // require the caller's unsafe validity assertion, not an invented owner.
  return {};
}

auto SemanticVisitor::Impl::rawPointerLifetimeDiagnostic(
    const RawPointerOrigin &origin, const Token &site)
    -> std::optional<Diagnostic> {
  if (!rawAuditEnabled())
    return std::nullopt;
  if (origin.parameterPointee || origin.loopInput != 0)
    return std::nullopt;
  if (origin.sourceResultQuery != 0) {
    auto diagnostic =
        makeDiagnostic(DiagnosticId::RawAuditIncomplete, site,
                       {"Raw pointer audit has no completed source-result "
                        "provenance query for this access."});
    diagnostic.related.push_back(
        {origin.addressSite, "Source result remains unresolved here."});
    diagnostic.hints.emplace_back(
        "Recursive, indirect, unresolved borrowed-storage, or analysis-limited "
        "source results exceed this optional analysis. This is not proof of an "
        "invalid pointer; the unsafe caller remains responsible for validity.");
    if (requestSession.rawResultQueries)
      for (const auto &query : requestSession.rawResultQueries->entries)
        if (query->identity == origin.sourceResultQuery && query->failure)
          diagnostic.hints.push_back("Source-result analysis: " +
                                     query->failure->message);
    return diagnostic;
  }
  Symbol *root = origin.place.receiver ? receiverStateSymbol() : nullptr;
  for (auto &scope : scopes) {
    if (root)
      break;
    for (auto &[_, symbol] : scope) {
      if (symbolForDeclaration(symbol.declaration) == origin.place.root) {
        root = &symbol;
        break;
      }
    }
    if (root)
      break;
  }
  const auto *declaration =
      semanticModel.database().findSymbol(origin.place.root);
  // Nonlocal storage is not invalid merely because it is outside this
  // body's lexical scope stack.
  const bool local =
      declaration && (declaration->kind == SymbolKind::LocalVariable ||
                      declaration->kind == SymbolKind::Parameter);
  const bool unavailable =
      root &&
      (root->valueState != ValueState::Available ||
       unavailableProjectedState({.root = root,
                                  .rootIdentity = origin.place.root,
                                  .projections = origin.place.projections,
                                  .receiver = origin.place.receiver}));
  if ((local && !root) || unavailable || origin.invalidated || origin.expired) {
    auto diagnostic = makeDiagnostic(
        unavailable || origin.invalidated || origin.possible
            ? DiagnosticId::RawAuditPotential
            : DiagnosticId::RawAuditViolation,
        site,
        {origin.possible && (origin.expired || (local && !root))
             ? "Raw pointer access may refer to storage whose scope has ended."
         : origin.expired
             ? "Raw pointer access refers to storage whose scope has ended."
         : unavailable
             ? "Raw pointer access refers to an owner that was moved on a "
               "possible path."
         : origin.invalidated
             ? "Raw pointer access may refer to an object whose "
               "lifetime was replaced by an intervening operation."
             : "Raw pointer access refers to storage whose scope has ended."});
    diagnostic.related.push_back(
        {origin.addressSite, "Raw address obtained here."});
    diagnostic.hints.emplace_back(
        "Keep the source object live through this operation. Unsafe validity "
        "is a programmer obligation; this optional audit is not a complete "
        "proof.");
    return diagnostic;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::publishRawLifetimeDiagnostic(
    const Diagnostic &diagnostic) -> void {
  // A direct primitive and its checked-owner summary can check the same
  // access. Only identical reports in this analysis context are duplicates;
  // different origin/access spans or generic context notes stay visible.
  const bool published = std::any_of(
      diagnostics.begin(), diagnostics.end(), [&](const Diagnostic &previous) {
        return previous.code == diagnostic.code &&
               previous.severity == diagnostic.severity &&
               previous.primary == diagnostic.primary &&
               previous.message == diagnostic.message &&
               previous.hints == diagnostic.hints &&
               previous.related.size() == diagnostic.related.size() &&
               std::equal(previous.related.begin(), previous.related.end(),
                          diagnostic.related.begin(),
                          [](const RelatedDiagnostic &left,
                             const RelatedDiagnostic &right) {
                            return left.span == right.span &&
                                   left.message == right.message;
                          });
      });
  if (!published)
    diagnostics.push_back(diagnostic);
}

auto SemanticVisitor::Impl::validateRawPointerAccess(const ExprPtr &pointer,
                                                     const Token &site,
                                                     AccessMode access)
    -> void {
  if (!rawAuditEnabled())
    return;
  validateRawPointerOrigins(rawPointerOrigins(pointer), site, access);
}

auto SemanticVisitor::Impl::recordRawAccess(
    const std::vector<RawPointerOrigin> &origins, const Token &site,
    AccessMode access, bool checksLoans) -> void {
  if (!rawAuditEnabled())
    return;
  const auto query = requestSession.collectingRawResult;
  if (!query || lambdaDepth || destructorDepth)
    return;
  std::vector<Parameter> noParameters;
  const auto *parameters = &noParameters;
  std::vector<SemanticType> parameterTypes;
  if (query->function) {
    const auto *function = currentFunctionInfo();
    if (!function || function->id != query->function || constructorDepth)
      return;
    parameters = &function->bodyDeclaration()->parameters();
    parameterTypes = function->parameterTypes;
  } else if (query->constructor && currentClass) {
    const auto &constructors = classInfo(*currentClass).constructors;
    const auto constructor = std::find_if(
        constructors.begin(), constructors.end(), [&](const auto &candidate) {
          return candidate.id == query->constructor;
        });
    if (constructor == constructors.end() || !constructor->declaration)
      return;
    parameters = &constructor->declaration->parameters();
    for (const auto &parameter : *parameters)
      parameterTypes.push_back(typeOf(parameter));
  } else if (!query->defaultOwner)
    return;
  auto inputs = origins;
  // Loop inputs are expanded by the existing backedge protocol before being
  // recorded. A marker names a pointer carrier, not an accessed allocation.
  std::erase_if(inputs,
                [](const auto &origin) { return origin.loopInput != 0; });
  RawResultAccess requirement{
      .access = access, .checksLoans = checksLoans, .site = tokenSpan(site)};
  collectRawResultFields({{{}, std::move(inputs)}}, *parameters, parameterTypes,
                         query->constructor || query->defaultOwner,
                         &requirement.fields);
  for (auto &field : requirement.fields)
    std::erase_if(field.origins, [&](const auto &source) {
      if (source.parameter || source.receiver || source.globalInput ||
          source.origin.sourceResultQuery)
        return false;
      const auto *root = rawPointerRoot(source.origin.place);
      // Callee-local accesses were checked while that storage was live. They
      // are not caller obligations (unlike an address returned after death).
      return !root ||
             (root->bindingKind != SemanticBindingKind::GlobalVariable &&
              root->bindingKind != SemanticBindingKind::StaticField);
    });
  std::erase_if(requirement.fields,
                [](const auto &field) { return field.origins.empty(); });
  if (!requirement.fields.empty())
    query->accesses.push_back(std::move(requirement));
}

auto SemanticVisitor::Impl::rawDispatchAccessFree(
    const ResolvedCallInfo &resolved, const SemanticPlace &receiver) -> bool {
  if (!rawAuditEnabled())
    return false;
  if (resolved.dispatch == CallDispatch::Static)
    return false;
  const auto *selected = semanticModel.findFunction(resolved.function);
  if (!selected || selected->virtualRoots.empty())
    return false;
  const auto roots = selected->virtualRoots;
  const auto matches = [&](const CallableCandidate &candidate) {
    return candidate.virtualMethod &&
           std::any_of(candidate.virtualRoots.begin(),
                       candidate.virtualRoots.end(), [&](FunctionId root) {
                         return std::find(roots.begin(), roots.end(), root) !=
                                roots.end();
                       });
  };
  const auto prove = [&](const ClassInfo &owner, const SemanticType &type) {
    if (containsOpenTypeParameter(type))
      return false;
    bool found = false;
    for (const auto &[_, member] : owner.members) {
      const auto concrete = substituteSymbol(member.symbol, type);
      for (const auto &candidate : concrete.overloads) {
        if (!matches(candidate))
          continue;
        if (candidate.pureVirtual)
          return false;
        found = true;
        ResolvedCallInfo target{.function = candidate.id,
                                .declaration = candidate.declaration,
                                .returnType = candidate.returnType,
                                .parameterTypes = candidate.parameterTypes,
                                .dispatch = CallDispatch::Static,
                                .dispatchOwner = candidate.dispatchOwner};
        const auto query = rawResultQuery(
            target, candidate.dispatchOwner.kind == SemanticType::Class
                        ? candidate.dispatchOwner
                        : type);
        if (!query || !query->complete || !query->valid ||
            !query->accessesComplete || !query->accesses.empty())
          return false;
      }
    }
    return found;
  };
  // A checked reference may name an exact owning local. Never infer a dynamic
  // type merely from the base-typed reference declaration or an arbitrary
  // projection: only the existing canonical whole-object place is a proof.
  const auto place = loanPlace(receiver);
  if (place.valid() && !place.receiver && place.projections.empty())
    if (const auto *root = rawPointerRoot(place);
        root && root->type.kind == SemanticType::Class &&
        !containsOpenTypeParameter(root->type))
      if (const auto *owner = classInfo(root->type); owner && !owner->abstract)
        return prove(*owner, root->type);

  // The declaration catalog is sealed before body analysis. Its inherited
  // member sets describe every nongeneric implementation in this program.
  // An open family is not bounded by whichever instances happen to exist yet.
  bool found = false;
  for (const auto &owner : classes) {
    if (owner.abstract)
      continue;
    bool possible = false;
    for (const auto &[_, member] : owner.members)
      possible |= std::any_of(member.symbol.overloads.begin(),
                              member.symbol.overloads.end(), matches);
    if (!possible)
      continue;
    if (!owner.genericParameters.empty() ||
        !prove(owner, openClassType(owner.id)))
      return false;
    found = true;
  }
  return found;
}

auto SemanticVisitor::Impl::validateRawCallAccesses(
    const Expr &expression, const RawInvocation *invocation) -> void {
  if (!rawAuditEnabled())
    return;
  const auto *call = exprAs<Call>(&expression);
  const auto *resolved = invocation ? &invocation->resolved
                         : call     ? semanticModel.findCall(*call)
                                    : nullptr;
  const auto *construction =
      invocation ? nullptr : semanticModel.findConstruction(expression);
  const auto *lambda =
      !invocation && call ? semanticModel.findLambdaCall(*call) : nullptr;
  const auto *deferred = !invocation && call
                             ? semanticModel.findDeferredCallableCall(*call)
                             : nullptr;
  const auto unknownAccess = [&]() {
    ++requestSession.rawAccessObservation;
    if (requestSession.collectingRawResult && lambdaDepth == 0 &&
        destructorDepth == 0)
      requestSession.collectingRawResult->accessesComplete = false;
  };
  if (!resolved && !construction && !lambda && !deferred)
    return;
  if (resolved && resolved->intrinsic != IntrinsicKind::None && !construction) {
    // Identity-bound address transport and checked-carrier observations do
    // not access the raw pointee. Other primitives cannot certify purity of
    // an enclosing source callable merely by bypassing source-body queries.
    switch (resolved->intrinsic) {
    case IntrinsicKind::Move:
    case IntrinsicKind::RawPointerConversion:
    case IntrinsicKind::NumericTypeParameterConversion:
    case IntrinsicKind::NumericAliasConversion:
    case IntrinsicKind::DefaultTypeParameterConstruction:
    case IntrinsicKind::ArraySize:
    case IntrinsicKind::IntegerWrappingAdd:
    case IntrinsicKind::IntegerWrappingSubtract:
    case IntrinsicKind::IntegerWrappingMultiply:
    case IntrinsicKind::IntegerSaturatingAdd:
    case IntrinsicKind::IntegerSaturatingSubtract:
    case IntrinsicKind::IntegerSaturatingMultiply:
    case IntrinsicKind::IntegerAddOverflows:
    case IntrinsicKind::IntegerSubtractOverflows:
    case IntrinsicKind::IntegerMultiplyOverflows:
    case IntrinsicKind::EnumUnderlyingValue:
    case IntrinsicKind::ObjectSize:
    case IntrinsicKind::ObjectAlignment:
    // A terminal failure never returns, so it cannot access a raw pointee.
    case IntrinsicKind::Panic:
    case IntrinsicKind::FailEmptyOwner:
    case IntrinsicKind::FailInactiveValue:
    case IntrinsicKind::FailInactiveError:
      return;
    case IntrinsicKind::RestoringSwap:
    case IntrinsicKind::RestoringExchange:
    case IntrinsicKind::RestoringIndexedSwap:
      if (resolved->typeArguments.size() == 1 &&
          !containsOpenTypeParameter(resolved->typeArguments.front())) {
        const auto traits = typeTraits(resolved->typeArguments.front());
        if (traits.relocation != RelocationKind::Unavailable &&
            !requiresActiveCleanupFor(resolved->typeArguments.front()))
          return;
      }
      unknownAccess();
      return;
    default:
      unknownAccess();
      return;
    }
  }
  if (lambda)
    if (const auto *info = semanticModel.findLambda(lambda->lambda);
        info && info->rawAccessFreeType != SemanticType::Unknown &&
        info->rawAccessFreeType == semanticModel.typeOf(*call->callee()))
      return;
  // Demand ordinary source summaries even for scalar signatures: a source
  // body can form a raw address of program storage without taking a pointer.
  const auto query = invocation
                         ? rawResultQuery(*resolved, invocation->receiverType)
                     : construction ? rawConstructorResultQuery(*construction)
                     : resolved     ? rawResultQuery(*call, *resolved)
                                    : nullptr;
  if (!query && !lambda && !deferred) {
    if (resolved) {
      unknownAccess();
      return; // Preserve the explicit foreign-call boundary.
    }
    if (!construction || !construction->declaration ||
        !construction->declaration->body())
      return; // Generated copy/move is inert transport, not a source body.
    // An unsupported selected source constructor is still possible execution.
    // Let the ordinary incomplete-input check below validate its actuals.
  }
  const Token site =
      invocation ? invocation->site : expressionToken(expression);
  const auto accesses =
      query ? query->accesses : std::vector<RawResultAccess>{};
  for (const auto &requirement : accesses) {
    std::vector<RawPointerOrigin> origins;
    for (const auto &field :
         rawProducedFields(expression, &requirement.fields, invocation))
      mergeRawPointerOrigins(origins, field.origins);
    validateRawPointerOrigins(origins, site, requirement.access,
                              requirement.checksLoans);
  }
  if (query && query->complete && query->valid && query->accessesComplete)
    return;
  if (resolved && resolved->dispatch != CallDispatch::Static) {
    SemanticPlace receiver;
    if (invocation)
      receiver = invocation->receiverPlace.value_or(
          semanticPlace(invocation->receiver));
    else if (call)
      if (const auto *member = exprAs<Get>(call->callee().get()))
        receiver = semanticPlace(member->object());
    if (rawDispatchAccessFree(*resolved, receiver))
      return;
  }
  unknownAccess();
  const auto possibleAccess = [&](const auto &fields,
                                  const SemanticType &type) {
    for (const auto &field : fields) {
      const auto pointers = rawPointerTypesAt(type, field.projections);
      if (pointers.empty())
        validateRawPointerOrigins(field.origins, site, AccessMode::Mutable);
      for (const auto &pointer : pointers)
        validateRawPointerOrigins(field.origins, site, pointer.pointerAccess);
    }
  };
  if (invocation) {
    for (const auto &argument : invocation->arguments)
      possibleAccess(rawPointerFields(argument.get()),
                     semanticModel.typeOf(*argument));
    possibleAccess(
        invocation->receiver ? rawPointerFields(invocation->receiver)
        : invocation->receiverPlace
            ? rawPointerFieldsAt(loanPlace(*invocation->receiverPlace))
            : std::vector<ProjectedRawPointerOrigins>{},
        invocation->receiverType);
  } else if (call) {
    for (std::size_t index = 0;
         index < resolvedCallArgumentCount(*call, resolved); ++index) {
      const auto &argument = *resolvedCallArgument(*call, resolved, index);
      possibleAccess(rawPointerFields(argument.get()),
                     semanticModel.typeOf(*argument));
    }
    if (lambda || deferred)
      possibleAccess(rawPointerFields(call->callee().get()),
                     semanticModel.typeOf(*call->callee()));
    else if (const auto *member = exprAs<Get>(call->callee().get()))
      possibleAccess(rawPointerFields(member->object().get()),
                     semanticModel.typeOf(*member->object()));
    else if (resolved && resolved->receiverMode != CallReceiverMode::None)
      if (auto *receiver = receiverStateSymbol())
        possibleAccess(receiver->rawPointerFields, receiver->type);
  } else if (const auto *direct = exprAs<DirectInitializer>(&expression)) {
    for (const auto &argument : direct->arguments())
      possibleAccess(rawPointerFields(argument.get()),
                     semanticModel.typeOf(*argument));
  }
  for (const auto &global : saveRawProgramStorage()) {
    auto fields = global.fields;
    if (global.symbol->type.kind == SemanticType::RawPointer)
      fields.push_back({{}, global.origins});
    possibleAccess(fields, global.symbol->type);
  }
}

auto SemanticVisitor::Impl::applyRawOperatorInvocation(
    const Expr &expression, const RawInvocation &invocation, bool contextual,
    bool projection) -> void {
  if (!rawAuditEnabled())
    return;
  validateRawCallAccesses(expression, &invocation);
  invalidateRawCallOrigins(nullptr, &invocation);
  // Capture the selected result before applying caller writes. Its normalized
  // sources describe the invocation inputs, not an unrelated later value of a
  // mutable argument. Each contextual edge has effects but no replacement for
  // the value on which contextual conversion was requested.
  auto fields = rawProducedFields(expression, nullptr, &invocation);
  if (!rawResultQuery(invocation.resolved, invocation.receiverType)) {
    // Foreign operator results retain contradictory known inputs just as
    // ordinary foreign calls do, without inventing a foreign lifetime fact.
    std::vector<RawPointerOrigin> inputs;
    const auto retain = [&](const auto &carried) {
      for (const auto &field : carried)
        for (const auto &origin : field.origins)
          if (origin.place.valid() || origin.sourceResultQuery ||
              origin.loopInput)
            mergeRawPointerOrigins(inputs, {origin});
    };
    for (const auto &argument : invocation.arguments)
      retain(rawPointerFields(argument.get()));
    if (invocation.receiver)
      retain(rawPointerFields(invocation.receiver));
    else if (invocation.receiverPlace)
      retain(rawPointerFieldsAt(loanPlace(*invocation.receiverPlace)));
    for (auto path : rawPointerFieldPaths(invocation.resolved.returnType))
      mergeRawPointerFields(fields, {{std::move(path), inputs}});
    for (auto &field : fields)
      for (auto &origin : field.origins)
        origin.possible = true;
  }
  applyRawCallWrites(expression, &invocation);
  if (projection) {
    // operator-> is attached to the enclosing member expression, not a
    // standalone AST value. Keep only the selected field's relative paths.
    const auto *get = exprAs<Get>(&expression);
    const auto &type = invocation.resolved.returnType;
    const auto *owner =
        type.kind == SemanticType::Reference && type.arguments.size() == 1
            ? classInfo(type.arguments.front())
            : nullptr;
    if (!get || !owner)
      return;
    const auto member = owner->members.find(get->name().lexeme);
    if (member == owner->members.end() ||
        member->second.symbol.type == SemanticType::Function)
      return;
    const auto field = toolingSymbolFor(member->second.symbol);
    std::erase_if(fields, [&](const auto &entry) {
      return entry.projections.empty() ||
             entry.projections.front().kind != PlaceProjectionKind::Field ||
             entry.projections.front().field != field;
    });
    for (auto &entry : fields)
      entry.projections.erase(entry.projections.begin());
  }
  if (!contextual)
    valueFlow.rawOperatorResults[&expression] = std::move(fields);
}

auto SemanticVisitor::Impl::validateRawPointerOrigins(
    const std::vector<RawPointerOrigin> &origins, const Token &site,
    AccessMode access, bool checksLoans) -> void {
  if (!rawAuditEnabled())
    return;
  ++requestSession.rawAccessObservation;
  recordRawAccess(origins, site, access, checksLoans);
  std::shared_ptr<LoanFlowState> context;
  if (checksLoans && !valueFlow.rawPointerLoops.empty() &&
      std::any_of(origins.begin(), origins.end(),
                  [](const auto &origin) { return origin.loopInput != 0; })) {
    context = std::make_shared<LoanFlowState>(loanFlow);
  }
  for (auto &loop : valueFlow.rawPointerLoops)
    loop.accesses.push_back(
        {.origins = origins,
         .site = site,
         .memoryAccess = checksLoans ? std::optional{access} : std::nullopt,
         .loanContext = context});
  for (const auto &origin : origins) {
    if (origin.parameterPointee || origin.loopInput != 0)
      continue;
    if (auto diagnostic = rawPointerLifetimeDiagnostic(origin, site)) {
      publishRawLifetimeDiagnostic(*diagnostic);
      continue;
    }
    if (checksLoans)
      reportRawLoanConflict(site, origin.place, access, origin.throughLoan);
  }
}

auto SemanticVisitor::Impl::beginRawPointerLoop() -> void {
  if (!rawAuditEnabled())
    return;
  const auto identity = valueFlow.nextRawPointerLoop++;
  valueFlow.rawPointerLoops.push_back({.identity = identity});
  const auto seed = [&](Symbol &symbol) {
    const auto place = placeKey(placeForSymbol(symbol));
    if (!place.valid())
      return;
    if (symbol.type.kind == SemanticType::RawPointer)
      symbol.rawPointerOrigins.push_back(
          {.place = place, .loopInput = identity});
    for (auto &field : symbol.rawPointerFields) {
      auto fieldPlace = place;
      fieldPlace.projections.insert(fieldPlace.projections.end(),
                                    field.projections.begin(),
                                    field.projections.end());
      field.origins.push_back(
          {.place = std::move(fieldPlace), .loopInput = identity});
    }
  };
  for (auto &scope : scopes) {
    for (auto &[_, symbol] : scope)
      seed(symbol);
  }
  for (const auto &global : saveRawProgramStorage())
    seed(*global.symbol);
}

auto SemanticVisitor::Impl::deferredRawPointerLoopUse(const ExprPtr &pointer,
                                                      const Token &site)
    -> std::shared_ptr<RawPointerLoopUse> {
  return deferredRawPointerLoopUse(rawPointerOrigins(pointer), site);
}

auto SemanticVisitor::Impl::deferredRawPointerLoopUse(
    const std::vector<RawPointerOrigin> &origins, const Token &site)
    -> std::shared_ptr<RawPointerLoopUse> {
  if (!rawAuditEnabled())
    return {};
  auto use = std::make_shared<RawPointerLoopUse>();
  use->pendingLoops = valueFlow.rawPointerLoops.size();
  for (auto &loop : valueFlow.rawPointerLoops)
    loop.accesses.push_back({origins, site, use});
  return use;
}

auto SemanticVisitor::Impl::endRawPointerLoop(const ScopeStack &backedge,
                                              bool reachable) -> void {
  if (!rawAuditEnabled())
    return;
  if (valueFlow.rawPointerLoops.empty())
    return;
  auto loop = std::move(valueFlow.rawPointerLoops.back());
  valueFlow.rawPointerLoops.pop_back();
  auto resolvedBackedge = backedge;
  resolvedBackedge.emplace_back();
  for (const auto &global : saveRawProgramStorage())
    resolvedBackedge.back().emplace(
        std::to_string(resolvedBackedge.back().size()), *global.symbol);
  const auto substitute = [&](const std::vector<RawPointerOrigin> &inputs,
                              bool markersOnly) {
    std::vector<RawPointerOrigin> result;
    std::vector<RawPointerOrigin> visited;
    const auto expand = [&](const auto &self,
                            const RawPointerOrigin &origin) -> void {
      if (origin.loopInput != loop.identity) {
        mergeRawPointerOrigins(result, {origin});
        return;
      }
      auto previous = std::find_if(visited.begin(), visited.end(),
                                   [&](const auto &candidate) {
                                     return candidate.place == origin.place;
                                   });
      if (previous != visited.end()) {
        const bool covered = std::all_of(
            origin.lifetimeInvalidations.begin(),
            origin.lifetimeInvalidations.end(), [&](const auto &place) {
              return std::find(previous->lifetimeInvalidations.begin(),
                               previous->lifetimeInvalidations.end(),
                               place) != previous->lifetimeInvalidations.end();
            });
        if (covered)
          return;
      }
      mergeRawPointerOrigins(visited, {origin});
      const auto forward = [&](const RawPointerOrigin &candidate) {
        auto next = candidate;
        next.possible = true;
        applyRawPointerLifetimeInvalidations(next,
                                             origin.lifetimeInvalidations);
        self(self, next);
      };
      for (const auto &scope : resolvedBackedge) {
        for (const auto &[_, symbol] : scope) {
          if (symbolForDeclaration(symbol.declaration) != origin.place.root)
            continue;
          if (origin.place.projections.empty()) {
            for (const auto &next : symbol.rawPointerOrigins)
              forward(next);
          } else {
            for (const auto &field : symbol.rawPointerFields)
              if (field.projections == origin.place.projections)
                for (const auto &next : field.origins)
                  forward(next);
          }
        }
      }
    };
    for (const auto &origin : inputs)
      if (!markersOnly || origin.loopInput == loop.identity)
        expand(expand, origin);
    return result;
  };
  // A write through a loop-carried pointer can target storage first selected
  // by a later iteration. Close the same finite origin transfer over writes
  // before checking uses; do not execute or re-analyze the source body.
  if (reachable && !loop.writes.empty()) {
    const auto destinationPlace = [&](const RawPointerOrigin &origin,
                                      const auto &projections) {
      auto place = origin.place;
      if (origin.parameterPointee)
        place.projections.push_back({.kind = PlaceProjectionKind::Dereference});
      else if (auto *root = rawPointerRoot(place);
               root && root->type.kind == SemanticType::Array &&
               place.projections.empty())
        place.projections.push_back(
            {.kind = PlaceProjectionKind::DynamicIndex});
      place.projections.insert(place.projections.end(), projections.begin(),
                               projections.end());
      return place;
    };
    bool changed;
    do {
      changed = false;
      for (const auto &write : loop.writes) {
        auto fields = write.fields;
        for (auto &field : fields)
          field.origins = substitute(field.origins, false);
        for (const auto &origin : substitute({write.destination}, false)) {
          if (!origin.place.valid() || origin.loopInput != 0)
            continue;
          const auto place = destinationPlace(origin, write.projections);
          auto projected = fields;
          for (auto &field : projected)
            field.projections.insert(field.projections.begin(),
                                     place.projections.begin(),
                                     place.projections.end());
          for (auto &scope : resolvedBackedge)
            for (auto &[_, symbol] : scope) {
              if (symbolForDeclaration(symbol.declaration) != place.root)
                continue;
              if (place.projections.empty() &&
                  symbol.type.kind == SemanticType::RawPointer) {
                const auto before = symbol.rawPointerOrigins;
                for (const auto &field : projected)
                  if (field.projections.empty())
                    mergeRawPointerOrigins(symbol.rawPointerOrigins,
                                           field.origins);
                changed |= before != symbol.rawPointerOrigins;
              } else {
                const auto before = symbol.rawPointerFields;
                mergeRawPointerFields(symbol.rawPointerFields, projected);
                changed |= before != symbol.rawPointerFields;
              }
            }
        }
      }
    } while (changed);
    for (const auto &write : loop.writes) {
      auto fields = write.fields;
      for (auto &field : fields)
        field.origins = substitute(field.origins, false);
      for (const auto &origin : substitute({write.destination}, false)) {
        if (origin.loopInput != 0) {
          for (auto &outer : valueFlow.rawPointerLoops)
            if (outer.identity == origin.loopInput)
              outer.writes.push_back({origin, write.projections, fields});
          continue;
        }
        const auto place = destinationPlace(origin, write.projections);
        if (auto *root = rawPointerRoot(place))
          applyRawPointerFields({.root = root,
                                 .rootIdentity = place.root,
                                 .projections = place.projections,
                                 .receiver = place.receiver},
                                fields, false);
      }
    }
  }
  for (const auto &use : loop.accesses) {
    if (!use.deferred)
      continue;
    if (reachable)
      use.deferred->origins = substitute(use.deferred->origins, false);
    else
      std::erase_if(use.deferred->origins, [&](const auto &origin) {
        return origin.loopInput == loop.identity;
      });
  }
  if (reachable) {
    for (const auto &use : loop.accesses) {
      for (const auto &origin : substitute(use.origins, true)) {
        if (use.expectedPointee != SemanticType::Unknown) {
          if (origin.pointeeType != SemanticType::Unknown &&
              !containsOpenTypeParameter(origin.pointeeType) &&
              origin.pointeeType != use.expectedPointee) {
            diagnostics.push_back(makeDiagnostic(
                DiagnosticId::RawAuditPotential, use.site,
                {"Raw pointer conversion may contradict a known pointee type "
                 "from a later loop iteration."}));
            diagnostics.back().related.push_back(
                {origin.addressSite,
                 "Pointee type retained at this conversion."});
          }
          continue;
        }
        if (use.deferred)
          mergeRawPointerOrigins(use.deferred->origins, {origin});
        recordRawAccess({origin}, use.site,
                        use.memoryAccess.value_or(AccessMode::ReadOnly),
                        use.memoryAccess.has_value());
        if (auto diagnostic = rawPointerLifetimeDiagnostic(origin, use.site)) {
          diagnostic->hints.emplace_back(
              "This origin can reach the access through a later loop "
              "iteration; reseat the pointer before accessing it.");
          if (use.deferred)
            use.deferred->diagnostics.emplace_back(std::move(*diagnostic));
          else
            publishRawLifetimeDiagnostic(*diagnostic);
        } else if (use.memoryAccess && use.loanContext &&
                   !origin.parameterPointee && origin.loopInput == 0) {
          // Resolve the new place against the loans and sequence at the use,
          // not the (potentially already cleaned-up) loop-exit loan state.
          auto enclosing = std::move(loanFlow);
          loanFlow = *use.loanContext;
          const auto first = loanFlow.rawAuditConflicts.size();
          reportRawLoanConflict(use.site, origin.place, *use.memoryAccess,
                                origin.throughLoan);
          std::vector<PendingBorrowConflict> conflicts;
          for (std::size_t index = first;
               index < loanFlow.rawAuditConflicts.size(); ++index) {
            loanFlow.rawAuditConflicts[index].sequence =
                use.loanContext->nextSequence;
            conflicts.push_back(std::move(loanFlow.rawAuditConflicts[index]));
          }
          loanFlow = std::move(enclosing);
          for (auto &conflict : conflicts)
            loanFlow.rawAuditConflicts.push_back(std::move(conflict));
        }
      }
    }
  }
  for (const auto &use : loop.accesses)
    if (use.deferred && use.deferred->pendingLoops != 0)
      --use.deferred->pendingLoops;
  const auto removeMarkers = [&](auto &origins) {
    std::erase_if(origins, [&](const auto &origin) {
      return origin.loopInput == loop.identity;
    });
  };
  for (auto &scope : scopes)
    for (auto &[_, symbol] : scope) {
      removeMarkers(symbol.rawPointerOrigins);
      for (auto &field : symbol.rawPointerFields)
        removeMarkers(field.origins);
    }
  for (const auto &global : saveRawProgramStorage()) {
    removeMarkers(global.symbol->rawPointerOrigins);
    for (auto &field : global.symbol->rawPointerFields)
      removeMarkers(field.origins);
  }
  for (auto &outer : valueFlow.rawPointerLoops)
    for (auto &use : outer.accesses)
      use.origins = substitute(use.origins, false);
}

auto SemanticVisitor::Impl::requireUnsafe(const Expr &expression,
                                          UnsafeOperationKind operation,
                                          const Token &token,
                                          std::string_view description)
    -> void {
  semanticModel.recordUnsafeOperation(expression, operation);
  if (operation == UnsafeOperationKind::PointerArithmetic) {
    // Arithmetic asserts an allocation/lifetime obligation but does not read
    // the pointee or acquire a checked loan. Inert copying/equality never enter
    // this path.
    validateRawPointerOrigins(rawPointerOrigins(&expression), token,
                              AccessMode::ReadOnly, false);
  }
  if (unsafeDepth != 0) {
    if (!requestSession.activeUnsafeBlocks.empty()) {
      // Nested blocks discharge their own obligations. Lambda bodies start
      // unsafeDepth at zero, so they never use an enclosing block's permission.
      requestSession.usedUnsafeBlocks.insert(
          requestSession.activeUnsafeBlocks.back());
    }
    return;
  }
  Diagnostic diagnostic =
      makeDiagnostic("GTI-S2055", DiagnosticPhase::Semantics, token,
                     std::string(description) + " requires an unsafe block.");
  diagnostic.hints.emplace_back(
      (operation == UnsafeOperationKind::DeclaredUnsafeCall ||
       operation == UnsafeOperationKind::DeclaredUnsafeConstruction)
          ? "Establish the function's documented caller obligations before "
            "calling it inside 'unsafe { ... }'."
          : "Wrap only the operation whose pointer validity you have "
            "established "
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
  // A numeric generic needs the same literal-operand contract as an
  // integral generic during symbolic checking (notably for equality).
  // Concrete floating operations retain their ordinary integer-to-float
  // conversion: do not relabel an integer literal as a floating constant.
  // Concrete instance reanalysis applies the existing integer range check
  // or floating conversion, without changing promotions or call matching.
  if (!isIntegral(target) &&
      !(target.kind == SemanticType::TypeParameter && isNumeric(target))) {
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
  // An overloaded operator is a member of its left operand and takes the
  // right one as its argument; a built-in operator reads both. A comma
  // discards its left operand and yields its right one.
  const std::optional<OverloadedOperator> operation =
      binaryOperator(expression.oper().kind);
  // `left` is the left operand's type, or null for an integer literal.
  const auto analyzeRight = [&](const SemanticType *left) {
    const ExprPtr &right = expression.right();
    std::optional<ResultUseScope> use;
    if (consultsResultUse(right.get())) {
      if (expression.oper().kind == TokenKind::COMMA) {
        use.emplace(*this, right.get(), resultUseFor(expression));
      } else if (operation && left != nullptr && classInfo(*left) != nullptr) {
        std::vector<SemanticType> parameters;
        for (const CallableCandidate &candidate :
             structuralOperatorCandidates(*left, *operation))
          if (candidate.parameterTypes.size() == 1)
            parameters.push_back(candidate.parameterTypes.front());
        if (!parameters.empty())
          use.emplace(*this, right.get(),
                      std::vector{bindResultUseStep(std::move(parameters))},
                      ResultUseAccess::Unknown);
      } else if (operation && (left == nullptr ||
                               (*left != SemanticType::Unknown &&
                                left->kind != SemanticType::TypeParameter))) {
        use.emplace(*this, right.get(),
                    std::vector{operatorResultUseStep(*operation)},
                    ResultUseAccess::ReadOnly);
      }
    }
    return analyze(right);
  };
  const auto analyzeLeft = [&] {
    std::optional<ResultUseScope> use;
    if (operation && consultsResultUse(expression.left().get()))
      use.emplace(*this, expression.left().get(),
                  std::vector{operatorResultUseStep(*operation)},
                  ResultUseAccess::ReadOnly);
    return analyze(expression.left());
  };
  if (!acceptsContextualIntegerOperand(expression.oper().kind)) {
    const SemanticType left = analyzeLeft();
    return {left, analyzeRight(&left)};
  }

  const bool leftLiteral = isContextualIntegerLiteral(expression.left().get());
  const bool rightLiteral =
      isContextualIntegerLiteral(expression.right().get());
  if (leftLiteral == rightLiteral) {
    const SemanticType left = analyzeLeft();
    return {left, analyzeRight(&left)};
  }
  if (rightLiteral) {
    const SemanticType left = analyzeLeft();
    return {left, analyzeContextualIntegerOperand(expression.right(), left)};
  }

  const SemanticType right = analyzeRight(nullptr);
  return {analyzeContextualIntegerOperand(expression.left(), right), right};
}

auto SemanticVisitor::Impl::requireNumeric(SemanticType left,
                                           SemanticType right,
                                           const Token &token) -> void {
  if (left != SemanticType::Unknown && right != SemanticType::Unknown &&
      (!isNumeric(left) || !isNumeric(right))) {
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
  if (left != SemanticType::Unknown && right != SemanticType::Unknown &&
      (!isOrdered(left) || !isOrdered(right))) {
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
  if (left != SemanticType::Unknown && right != SemanticType::Unknown &&
      (!isIntegral(left) || !isIntegral(right))) {
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
  return isSignedInteger(type) || isUnsignedInteger(type);
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
  if (required == GenericConstraintKind::Relocatable &&
      hasConstraint(actual, GenericConstraintKind::BitwiseRelocatable)) {
    return true;
  }
  if (required == GenericConstraintKind::MoveConstructible &&
      (hasConstraint(actual, GenericConstraintKind::Movable) ||
       hasConstraint(actual, GenericConstraintKind::Relocatable) ||
       hasConstraint(actual, GenericConstraintKind::BitwiseRelocatable))) {
    return true;
  }
  if (!hasConstraint(actual, GenericConstraintKind::AtomicScalar)) {
    return false;
  }
  switch (required) {
  case GenericConstraintKind::CopyConstructible:
  case GenericConstraintKind::MoveConstructible:
  case GenericConstraintKind::Copyable:
  case GenericConstraintKind::Movable:
  case GenericConstraintKind::Relocatable:
  case GenericConstraintKind::BitwiseRelocatable:
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
  return type.kind == SemanticType::Int8 || type.kind == SemanticType::Int16 ||
         type.kind == SemanticType::Int32 || type.kind == SemanticType::Int64;
}

auto SemanticVisitor::Impl::isUnsignedInteger(SemanticType type) -> bool {
  return type.kind == SemanticType::UInt8 ||
         type.kind == SemanticType::UInt16 ||
         type.kind == SemanticType::UInt32 || type.kind == SemanticType::UInt64;
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
  if (type.kind == SemanticType::Int8 || type.kind == SemanticType::Int16 ||
      type.kind == SemanticType::UInt8 || type.kind == SemanticType::UInt16) {
    return SemanticType::Int32;
  }
  return type;
}

auto SemanticVisitor::Impl::widerInteger(SemanticType left,
                                         SemanticType right) const
    -> SemanticType {
  if (integerRank(left) != integerRank(right)) {
    return integerRank(left) > integerRank(right) ? left : right;
  }
  // Equal value domains do not imply identical native declarations. Preserve
  // C's family ordering independently of operand order: long long > long > int.
  const auto familyRank = [&](const SemanticType &type) {
    NativeCScalarKind family = type.nativeCScalar;
    if (family == NativeCScalarKind::None) {
      const TargetScalarKind value =
          type.kind == SemanticType::Int64    ? TargetScalarKind::Int64
          : type.kind == SemanticType::UInt64 ? TargetScalarKind::UInt64
          : type.kind == SemanticType::Int32  ? TargetScalarKind::Int32
                                              : TargetScalarKind::UInt32;
      family =
          target.fixedCScalarIdentity(value).value_or(NativeCScalarKind::None);
    }
    switch (family) {
    case NativeCScalarKind::LongLong:
    case NativeCScalarKind::UnsignedLongLong:
      return 3;
    case NativeCScalarKind::Long:
    case NativeCScalarKind::UnsignedLong:
      return 2;
    default:
      return 1;
    }
  };
  return familyRank(left) >= familyRank(right) ? left : right;
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
  return type == SemanticType::Bool;
}

auto SemanticVisitor::Impl::numericResult(SemanticType left, SemanticType right,
                                          const Expr *leftExpression,
                                          const Expr *rightExpression) const
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
      target.acceptsNullInitialization() && value == SemanticType::NullPtr) {
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
  // Initial creation/publication of a fresh result does not move an existing
  // object. Assignment separately requires its real assignment operation.
  if (semanticModel.isFreshClassResult(*expression))
    return true;
  const SemanticTypeTraits traits = typeTraits(target);
  return info->category == ValueCategory::Value ? traits.movable
                                                : traits.copyable;
}

auto SemanticVisitor::Impl::isOwnershipAssignment(
    const SemanticType &target, const SemanticType &value,
    const ExprPtr &expression, const SemanticPlace &destination) -> bool {
  const bool indirect = std::any_of(
      destination.projections.begin(), destination.projections.end(),
      [](const PlaceProjection &projection) {
        return projection.kind == PlaceProjectionKind::Dereference;
      });
  const SymbolId root = destination.root == nullptr
                            ? destination.rootIdentity
                            : toolingSymbolFor(*destination.root);
  if (!indirect && semanticModel.findForeignStorage(root) != nullptr) {
    // External storage has no GTI replacement authority. A whole-record store
    // must respect every embedded field's access policy, while a local value
    // copied from it keeps the ordinary GTI replacement rules (ADR 013).
    const auto writable = [&](const auto &self,
                              const SemanticType &type) -> bool {
      if (type.kind == SemanticType::Array && type.arguments.size() == 1)
        return self(self, type.arguments.front());
      if (type.kind != SemanticType::Class)
        return true;
      if (type.classId == 0 || type.classId > classes.size())
        return false;
      const ClassInfo &record = classInfo(type.classId);
      if (!record.cAbiRecord)
        return false;
      const GenericSubstitution substitution = classSubstitution(type);
      for (const FieldInfo &field : record.fields) {
        if (field.declaration == nullptr || !field.declaration->isMutable())
          return false;
        const auto member =
            record.members.find(field.declaration->name().lexeme);
        if (member == record.members.end() ||
            !self(self,
                  substituteType(member->second.symbol.type, substitution)))
          return false;
      }
      return true;
    };
    if (!writable(writable, target))
      return false;
  }
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
  if (target.kind != SemanticType::UniqueOwner &&
      isImplicitScalarInitializationConversion(target, value)) {
    return true;
  }
  if (target.kind == SemanticType::RawPointer) {
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
  return target == value;
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
  if (type.isNativeFunction() || type.isFunctionSignature()) {
    const std::size_t parameterCount =
        type.isFunctionSignature() ? *type.functionSignatureParameterCount
                                   : *type.nativeFunctionParameterCount;
    if (type.arguments.size() != parameterCount + 1) {
      return SemanticType::Unknown;
    }
    std::vector<SemanticType> parameters;
    parameters.reserve(parameterCount);
    for (std::size_t index = 0; index < parameterCount; ++index) {
      SemanticType::appendExpandedArgument(
          parameters, typeOf(type.arguments[index], fromScope));
    }
    if (type.isFunctionSignature()) {
      const ReceiverMutability receiver =
          !type.functionSignatureQualifier ? ReceiverMutability::ReadOnly
          : type.functionSignatureQualifier->kind == TokenKind::MUT
              ? ReceiverMutability::Mutable
              : ReceiverMutability::Consuming;
      return SemanticType::functionSignature(
          typeOf(type.arguments.back(), fromScope), std::move(parameters),
          receiver);
    }
    return SemanticType::nativeFunctionType(
        typeOf(type.arguments.back(), fromScope), parameters);
  }
  if (type.qualifiedOwner) {
    if (const std::optional<ConceptId> protocol =
            protocolConceptOwner(*type.qualifiedOwner, fromScope)) {
      const RegisteredConcept &conceptInfo = concepts[*protocol - 1];
      const auto member =
          std::find(conceptInfo.associatedTypes.begin(),
                    conceptInfo.associatedTypes.end(), type.name.last().lexeme);
      if (member == conceptInfo.associatedTypes.end() ||
          type.qualifiedOwner->arguments.size() !=
              conceptInfo.parameterIds.size() ||
          !type.arguments.empty()) {
        return SemanticType::Unknown;
      }
      const auto index = static_cast<std::uint32_t>(
          member - conceptInfo.associatedTypes.begin());
      std::vector<SemanticType> arguments;
      for (const TypeRef &argument : type.qualifiedOwner->arguments) {
        arguments.push_back(typeOf(argument, fromScope));
      }
      if (const std::optional<SemanticType> own =
              ownBlockProjection(*protocol, index, arguments, fromScope)) {
        return *own;
      }
      return substituteType(
          associatedTypeProjection(*protocol, index, std::move(arguments)),
          GenericSubstitution{.types = instanceTypeSubstitution,
                              .values = instanceValueSubstitution});
    }
    const auto alias = resolveTypeAliasPath(type, fromScope);
    if (!alias || !type.arguments.empty() || alias->id == 0 ||
        alias->id > typeAliases.size() ||
        typeAliases[alias->id - 1].resolution !=
            TypeAliasResolution::Resolved) {
      return SemanticType::Unknown;
    }
    return substituteType(
        alias->type, GenericSubstitution{.types = instanceTypeSubstitution,
                                         .values = instanceValueSubstitution});
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
  if (compilerCapability == CompilerCapabilityTypeKind::NativeCScalar) {
    const auto selected = resolveClassPath(type.name, fromScope);
    if (!selected || !type.arguments.empty()) {
      return SemanticType::Unknown;
    }
    return SemanticType::nativeCScalarType(classInfo(*selected).nativeCScalar,
                                           target)
        .value_or(SemanticType::Unknown);
  }
  if (compilerCapability == CompilerCapabilityTypeKind::UniqueOwner) {
    return type.arguments.size() == 1 ? SemanticType::uniqueOwnerOf(typeOf(
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
  default:
    if (const std::optional<SemanticType> parameter =
            resolveTypeParameter(type.name)) {
      return type.arguments.empty() ? *parameter : SemanticType::Unknown;
    }
    if (type.name.segments.size() == 1) {
      if (const std::optional<SemanticType> associated =
              blockAssociatedType(type.name.last().lexeme, fromScope)) {
        return type.arguments.empty() ? *associated : SemanticType::Unknown;
      }
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
      if (*id == 0 || *id > enums.size())
        return SemanticType::Unknown;
      const auto arguments = nominalArguments(enums[*id - 1].genericParameters,
                                              type.arguments, fromScope);
      return arguments ? SemanticType::enumType(*id, arguments->types,
                                                arguments->values)
                       : SemanticType::Unknown;
    }
    if (const std::optional<ClassId> id =
            resolveClassPath(type.name, fromScope)) {
      const ClassInfo &owner = classInfo(*id);
      if (!owner.declaration->genericArgumentPatterns().empty() ||
          !owner.definitionFamilyMembers.empty() ||
          owner.definitionFamilyPrimary != 0) {
        std::vector<SemanticType> arguments;
        for (const TypeRef &argument : type.arguments) {
          arguments.push_back(typeOf(argument, fromScope));
        }
        return selectExactClassSpecialization(
            SemanticType::classType(*id, std::move(arguments)));
      }
      if (!owner.genericParameters.empty() &&
          owner.genericParameters.back().pack) {
        if (type.arguments.size() + 1 < owner.genericParameters.size()) {
          return SemanticType::Unknown;
        }
        std::vector<SemanticType> arguments;
        for (const TypeRef &argument : type.arguments) {
          SemanticType::appendExpandedArgument(arguments,
                                               typeOf(argument, fromScope));
        }
        return selectExactClassSpecialization(
            SemanticType::classType(*id, std::move(arguments)));
      }
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
  for (const auto &pointer : type.pointers) {
    result = SemanticType::rawPointerTo(
        std::move(result),
        pointer.pointeeConst ? AccessMode::ReadOnly : AccessMode::Mutable);
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
    if (type.referenceMutability) {
      result.referenceAccess = AccessMode::Mutable;
    }
  }
  if (type.packExpansion) {
    if (result.kind == SemanticType::TypeParameter) {
      result = SemanticType::typePack(result.genericParameterId);
    } else if (result.kind != SemanticType::TypePack) {
      return SemanticType::Unknown;
    }
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
  if (result.kind == SemanticType::Reference && type.reference) {
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
  case ExprKind::PlaceSet:
    return static_cast<const PlaceSet &>(expression).oper();
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
  case ExprKind::MemberRequirement:
    return static_cast<const MemberRequirement &>(expression).keyword();
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
