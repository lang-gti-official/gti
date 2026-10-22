#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::analyzeDeferredSelection(
    const Call &call, const CallableCandidate &selected,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments) -> bool {
  if (!selected.record().deferredEvaluation)
    return false;
  const bool probe = deferredProbe && deferredProbeCall == &call;
  const bool openStatement =
      deferredStatementCall == &call &&
      std::any_of(selected.parameterTypes.begin(),
                  selected.parameterTypes.end(), [&](const SemanticType &type) {
                    return containsOpenTypeParameter(type);
                  });
  if (!probe && !openStatement) {
    diagnostics.push_back(
        makeDiagnostic(DiagnosticId::DeferredEvaluation, call.paren(),
                       {"A deferred helper must be invoked as a complete "
                        "expression statement."}));
    currentType = SemanticType::Unknown;
    return true;
  }
  // There is no eager parameter boundary. Both the single-candidate and
  // overload-set paths must select the same statement-template contract.
  ResolvedCallInfo info{.function = selected.id,
                        .declaration = selected.declaration,
                        .returnType = selected.returnType,
                        .parameterTypes = selected.parameterTypes,
                        .typeArguments = typeArguments,
                        .valueArguments = valueArguments,
                        .requirements = selected.requirements,
                        .tracksCaller = selected.record().tracksCaller};
  if (probe)
    semanticModel.record(call, std::move(info));
  else
    recordSelectedCallOccurrence(call, std::move(info));
  currentType = callExpressionType(selected.returnType);
  return true;
}

auto SemanticVisitor::Impl::deferredContext(const Token &token) const
    -> const DeferredLookupContext * {
  const auto found = deferredLookup.find(token.expansion);
  return found == deferredLookup.end() ? nullptr : &found->second;
}

SemanticVisitor::Impl::DeferredLookupScope::DeferredLookupScope(
    Impl &owner, const DeferredLookupContext &context)
    : owner(owner), namespaceScope(std::move(owner.currentNamespace)),
      sourceUnit(owner.currentSourceUnit), ownerClass(owner.currentClass),
      staticMember(owner.currentStaticMemberFunction),
      declarationOrder(owner.currentDeclarationOrder) {
  owner.currentNamespace = context.namespaceScope;
  owner.currentSourceUnit = context.sourceUnit;
  owner.currentClass = context.ownerClass;
  owner.currentStaticMemberFunction = context.staticMember;
  owner.currentDeclarationOrder = context.declarationOrder;
}

SemanticVisitor::Impl::DeferredLookupScope::~DeferredLookupScope() {
  owner.currentNamespace = std::move(namespaceScope);
  owner.currentSourceUnit = sourceUnit;
  owner.currentClass = ownerClass;
  owner.currentStaticMemberFunction = staticMember;
  owner.currentDeclarationOrder = declarationOrder;
}

auto SemanticVisitor::Impl::validDeferredBody(const Stmt &statement) const
    -> bool {
  const auto expressionAllowed = [](const Expr &expression) {
    bool allowed = true;
    AstCloneOptions inspection;
    inspection.expression = [&](const Expr &node) -> ExprPtr {
      switch (node.exprKind()) {
      case ExprKind::Lambda:
      case ExprKind::MemberRequirement:
      case ExprKind::Assign:
      case ExprKind::PlaceSet:
      case ExprKind::Set:
      case ExprKind::IndexSet:
      case ExprKind::DereferenceSet:
      case ExprKind::Postfix:
      case ExprKind::PackFold:
        allowed = false;
        break;
      case ExprKind::Unary: {
        const auto kind = static_cast<const Unary &>(node).oper().kind;
        if (kind == TokenKind::PLUS_PLUS || kind == TokenKind::MINUS_MINUS)
          allowed = false;
        break;
      }
      case ExprKind::ArrayInitializer:
      case ExprKind::Binary:
      case ExprKind::Call:
      case ExprKind::ConditionalExpr:
      case ExprKind::Conversion:
      case ExprKind::DirectInitializer:
      case ExprKind::Get:
      case ExprKind::Grouping:
      case ExprKind::Index:
      case ExprKind::LayoutQuery:
      case ExprKind::LiteralExpr:
      case ExprKind::Logical:
      case ExprKind::PackExpansion:
      case ExprKind::QualifiedName:
      case ExprKind::This:
      case ExprKind::Variable:
        break;
      }
      return nullptr;
    };
    (void)cloneExpr(expression, 0, inspection);
    return allowed;
  };
  if (const auto *block = stmtAs<BlockStmt>(&statement))
    return !block->isUnsafe() &&
           std::all_of(block->statements().begin(), block->statements().end(),
                       [&](const StmtPtr &child) {
                         return child && validDeferredBody(*child);
                       });
  if (const auto *branch = stmtAs<IfStmt>(&statement))
    return !branch->isConstexpr() && expressionAllowed(*branch->condition()) &&
           branch->thenBranch() && validDeferredBody(*branch->thenBranch()) &&
           (!branch->elseBranch() || validDeferredBody(*branch->elseBranch()));
  if (const auto *branch = stmtAs<ConditionalStmt>(&statement)) {
    const StmtList *active = branch->activeBranch(target);
    return !active || std::all_of(active->begin(), active->end(),
                                  [&](const StmtPtr &child) {
                                    return child && validDeferredBody(*child);
                                  });
  }
  if (const auto *expression = stmtAs<ExpressionStmt>(&statement))
    return expressionAllowed(*expression->expression());
  return stmtAs<EmptyStmt>(&statement);
}

auto SemanticVisitor::Impl::analyzeDeferredStatement(
    const ExpressionStmt &statement) -> bool {
  if (currentFunctionDeclaration &&
      currentFunctionDeclaration->hasFunctionAttribute("deferred"))
    return false;
  const Call *call = directCall(statement.expression());
  if (!call)
    return false;
  if (deferredProbe && deferredProbeCall == call)
    return false;
  std::optional<DeferredLookupScope> lookupScope;
  if (const auto *context = deferredContext(expressionToken(*call)))
    lookupScope.emplace(*this, *context);
  const auto symbol = resolveExpressionSymbol(call->callee());
  if (!symbol || symbol->type != SemanticType::Function ||
      std::none_of(symbol->overloads.begin(), symbol->overloads.end(),
                   [](const CallableCandidate &candidate) {
                     return candidate.record().deferredEvaluation;
                   }))
    return false;
  const auto reject = [&](std::string message) {
    diagnostics.push_back(makeDiagnostic(DiagnosticId::DeferredEvaluation,
                                         call->paren(), {message}));
  };
  if (deferredExpansionDepth >= 64 || deferredExpandedBodies >= 4096) {
    reject(
        "Deferred expansion exceeds the 64-call or 4096-body resource limit.");
    return true;
  }
  // Resolve and type-check the complete written call in an isolated semantic
  // snapshot. In particular, a disabled helper still checks its arguments,
  // but their speculative moves, loans and writes never affect caller flow.
  Impl probe(target, sourceGraph, toolingOccurrences, nullptr, sources,
             rawPointerAudit, requireFunctionDefinitions, trustedBodies);
  probe.requestSession = requestSession;
  probe.semanticModel.setFunctionDemandSink(nullptr);
  probe.semanticModel.setCallJournal(nullptr);
  probe.diagnostics.clear();
  // Session copies may share recursive query journals. Preserve their active
  // cycle/budget state, but never publish speculative results or writes back
  // into a caller's journal (including the query currently collecting facts).
  if (requestSession.openReanalyses)
    probe.requestSession.openReanalyses =
        std::make_shared<std::vector<InstanceAnalysisKey>>(
            *requestSession.openReanalyses);
  if (requestSession.programConcreteEffects)
    probe.requestSession.programConcreteEffects =
        std::make_shared<ProgramConcreteEffectState>(
            *requestSession.programConcreteEffects);
  if (const auto &queries = requestSession.completeReturnQueries) {
    auto fork = std::make_shared<CompleteReturnQueryState>();
    fork->depth = queries->depth;
    fork->discovered = queries->discovered;
    fork->completed = queries->completed;
    for (const auto &[key, entry] : queries->entries)
      fork->entries.emplace(key, std::make_shared<CompleteReturnQuery>(*entry));
    probe.requestSession.completeReturnQueries = std::move(fork);
  }
  if (const auto &queries = requestSession.rawResultQueries) {
    auto fork = std::make_shared<RawResultQueryState>();
    fork->depth = queries->depth;
    fork->nextIdentity = queries->nextIdentity;
    fork->hasRawGlobals = queries->hasRawGlobals;
    std::unordered_map<const RawResultQuery *, std::shared_ptr<RawResultQuery>>
        copies;
    const auto copy = [&](const std::shared_ptr<RawResultQuery> &entry) {
      auto [found, inserted] = copies.try_emplace(entry.get());
      if (inserted)
        found->second = std::make_shared<RawResultQuery>(*entry);
      return found->second;
    };
    for (const auto &entry : queries->entries)
      fork->entries.push_back(copy(entry));
    for (const auto &entry : queries->cachedRoots)
      fork->cachedRoots.push_back(copy(entry));
    for (const auto &entry : queries->familyEntries)
      fork->familyEntries.push_back(copy(entry));
    if (requestSession.collectingRawResult)
      probe.requestSession.collectingRawResult =
          copy(requestSession.collectingRawResult);
    probe.requestSession.rawResultQueries = std::move(fork);
  } else if (requestSession.collectingRawResult) {
    probe.requestSession.collectingRawResult =
        std::make_shared<RawResultQuery>(*requestSession.collectingRawResult);
  }
  probe.requestSession.trackAssertionDiagnostics = false;
  probe.requestSession.trackedAssertionFailed = false;
  probe.requestSession.assertionTrackingStart = 0;
  probe.requestSession.assertionDiagnostics.clear();
  probe.deferredProbe = true;
  probe.deferredProbeCall = call;
  for (const auto &argument : call->arguments())
    probe.deferredProbeArguments.insert(argument.get());
  probe.deferredLookup = deferredLookup;
  probe.nextExpansion = nextExpansion;
  probe.deferredExpansionDepth = deferredExpansionDepth;
  probe.deferredExpandedBodies = deferredExpandedBodies;
  probe.deferredActiveBodies = deferredActiveBodies;
  probe.instanceAnalysisActive = instanceAnalysisActive;
  probe.instanceBaseModel = instanceBaseModel;
  probe.instanceTypeSubstitution = instanceTypeSubstitution;
  probe.instanceValueSubstitution = instanceValueSubstitution;
  probe.designatedMoves = designatedMoves;
  probe.analyze(*call);
  if (hasErrorDiagnostics(probe.diagnostics)) {
    diagnostics.insert(diagnostics.end(), probe.diagnostics.begin(),
                       probe.diagnostics.end());
    return true;
  }
  const auto *selection = probe.semanticModel.findCall(*call);
  const FunctionInfo *function =
      selection ? semanticModel.findFunction(selection->function) : nullptr;
  if (!function || !function->declaration || !function->deferredEvaluation)
    return false;
  const auto unstableType = [&](const auto &self,
                                const SemanticType &type) -> bool {
    if (type.kind == SemanticType::Lambda &&
        !semanticModel.findLambda(type.lambdaId))
      return true;
    return std::any_of(
        type.arguments.begin(), type.arguments.end(),
        [&](const auto &argument) { return self(self, argument); });
  };
  if (std::any_of(
          selection->typeArguments.begin(), selection->typeArguments.end(),
          [&](const auto &type) { return unstableType(unstableType, type); })) {
    reject("A fresh callable-valued deferred argument requires a named binding "
           "so its concrete type identity is established before expansion.");
    return true;
  }
  if (std::any_of(selection->parameterTypes.begin(),
                  selection->parameterTypes.end(),
                  [&](const SemanticType &type) {
                    return containsOpenTypeParameter(type);
                  })) {
    // Like a dependent compile-time branch, this statement has no primary
    // executable flow. Concrete semantic reanalysis must expand and check it;
    // do not accidentally apply ordinary eager-call moves to a generic body.
    recordSelectedCallOccurrence(*call, *selection);
    semanticModel.record(*call, ExpressionInfo{.type = SemanticType::Void});
    loanFlow.deferredGenericFlow = true;
    requestSession.deferredFlowOwners.insert(currentProgramEffectOwner);
    return true;
  }
  for (std::size_t index = 0; index < call->arguments().size() &&
                              index < selection->parameterTypes.size();
       ++index) {
    const Expr &argument = *call->arguments()[index];
    if (exprAs<PackExpansion>(&argument))
      continue;
    const auto *actual = probe.semanticModel.findType(argument);
    const auto &expected = selection->parameterTypes[index];
    const auto &value = actual && actual->kind == SemanticType::Reference &&
                                actual->arguments.size() == 1
                            ? actual->arguments.front()
                        : actual ? *actual
                                 : SemanticType::Unknown;
    if (value != SemanticType::Unknown && value != expected) {
      reject("Deferred arguments require exact expression types; write the "
             "conversion explicitly at the call site.");
      return true;
    }
  }
  const FunctionDecl &definition = *function->bodyDeclaration();
  if (std::find(deferredActiveBodies.begin(), deferredActiveBodies.end(),
                &definition) != deferredActiveBodies.end()) {
    reject("Deferred expansion reaches its recursion resource limit; statement "
           "templates cannot expand a recursive helper cycle.");
    return true;
  }
  if (!definition.body() || !validDeferredBody(*definition.body())) {
    reject("The selected deferred helper has no eligible statement body.");
    return true;
  }
  if (statement.discardAttribute()) {
    report(*statement.discardAttribute(),
           "'[[discard]]' cannot be applied to a void deferred helper.");
    return true;
  }
  const auto &parameters = definition.parameters();
  std::unordered_map<std::string, const Expr *> arguments;
  std::unordered_set<std::string> defaultArguments;
  std::unordered_map<std::string, std::vector<const Expr *>> packs;
  DeferredLookupContext caller{.namespaceScope = currentNamespace,
                               .sourceUnit = currentSourceUnit,
                               .ownerClass = currentClass,
                               .staticMember = currentStaticMemberFunction,
                               .declarationOrder = currentDeclarationOrder};
  if (const auto *parent = deferredContext(expressionToken(*call)))
    caller = *parent;
  Token invocation = expressionToken(*call);
  if (caller.definition && caller.invocation)
    invocation = *caller.invocation;
  CallerLocationOrigin origin;
  if (caller.caller)
    origin = *caller.caller;
  else {
    const Token location = expressionToken(*call);
    const SourceSpan span = tokenSpan(location);
    origin = {.sourceUnit = sourceUnitFor(location),
              .start = span.start,
              .end = span.end,
              .line = span.line};
  }
  const std::uint32_t bodyExpansion = ++nextExpansion;
  deferredLookup.emplace(
      bodyExpansion,
      DeferredLookupContext{.namespaceScope = function->namespaceScope,
                            .sourceUnit = function->sourceUnit,
                            .definition = true,
                            .declarationOrder =
                                declarationOrders.contains(&definition)
                                    ? declarationOrders.at(&definition)
                                    : currentDeclarationOrder,
                            .caller = origin,
                            .invocation = invocation});
  auto &bodyContext = deferredLookup.at(bodyExpansion);
  std::size_t typeIndex = 0;
  std::size_t valueIndex = 0;
  for (const auto &parameter : function->genericParameters) {
    if (parameter.value && valueIndex < selection->valueArguments.size())
      bodyContext.values.emplace(parameter.name.lexeme,
                                 selection->valueArguments[valueIndex++]);
    else if (parameter.pack) {
      std::vector<SemanticType> elements(
          selection->typeArguments.begin() +
              static_cast<std::ptrdiff_t>(typeIndex),
          selection->typeArguments.end());
      bodyContext.types.emplace(
          parameter.name.lexeme,
          SemanticType::concreteTypePack(parameter.id, std::move(elements)));
      typeIndex = selection->typeArguments.size();
    } else if (!parameter.value && typeIndex < selection->typeArguments.size())
      bodyContext.types.emplace(parameter.name.lexeme,
                                selection->typeArguments[typeIndex++]);
  }
  const auto cloneArgument = [&](const Expr &expression,
                                 const DeferredLookupContext *fallback =
                                     nullptr) {
    // An argument of a nested expansion may itself contain substituted
    // caller expressions. Preserve each token's lookup domain independently.
    std::unordered_map<std::uint32_t, std::uint32_t> identities;
    AstCloneOptions options;
    options.token = [&](const Token &token) {
      Token copy = token;
      auto [entry, inserted] = identities.try_emplace(token.expansion, 0);
      if (inserted) {
        entry->second = ++nextExpansion;
        const auto *context = deferredContext(token);
        deferredLookup.emplace(entry->second, context    ? *context
                                              : fallback ? *fallback
                                                         : caller);
      }
      copy.expansion = entry->second;
      return copy;
    };
    return cloneExpr(expression, 0, options);
  };
  for (std::size_t index = 0; index < parameters.size(); ++index) {
    const Parameter &parameter = parameters[index];
    if (parameter.pack) {
      auto &values = packs[parameter.name.lexeme];
      for (std::size_t actual = index; actual < call->arguments().size();
           ++actual)
        values.push_back(call->arguments()[actual].get());
    } else if (index < call->arguments().size()) {
      arguments.emplace(parameter.name.lexeme, call->arguments()[index].get());
    } else if (parameter.defaultArgument) {
      arguments.emplace(parameter.name.lexeme,
                        parameter.defaultArgument->expression.get());
      defaultArguments.insert(parameter.name.lexeme);
    } else {
      reject("A deferred call requires explicit fixed arguments before a "
             "forwarded pack.");
      return true;
    }
  }
  AstCloneOptions options;
  options.expression = [&](const Expr &expression) -> ExprPtr {
    const auto *variable = exprAs<Variable>(&expression);
    if (!variable)
      return nullptr;
    const auto argument = arguments.find(variable->name().lexeme);
    if (argument == arguments.end())
      return nullptr;
    const bool defaulted = defaultArguments.contains(variable->name().lexeme);
    ExprPtr value =
        cloneArgument(*argument->second,
                      defaulted ? &deferredLookup.at(bodyExpansion) : nullptr);
    if (defaulted)
      semanticModel.recordDeferredDefault(*value, invocation);
    return value;
  };
  options.pack = [&](const PackExpansion &pack) -> std::optional<ExprList> {
    const auto found = packs.find(pack.name().lexeme);
    if (found == packs.end())
      return std::nullopt;
    ExprList values;
    for (const Expr *value : found->second)
      values.push_back(cloneArgument(*value));
    return values;
  };
  AstCloneResult expansion =
      cloneStmt(*definition.body(), bodyExpansion, options);
  if (!expansion.statement) {
    reject("The deferred helper body could not be expanded.");
    return true;
  }
  ResolvedCallInfo selected = *selection;
  selected.defaultArguments.clear(); // Probe-owned syntax is not executable.
  recordSelectedCallOccurrence(*call, std::move(selected));
  semanticModel.record(*call, ExpressionInfo{.type = SemanticType::Void});
  // Keep written references available to editor queries even when no expanded
  // path evaluates them. Executable expression/loan facts stay in the probe.
  const SourceSpan callSpan = tokenSpan(expressionToken(*call));
  for (const auto &writtenOccurrence :
       probe.semanticModel.database().occurrences(currentSourceUnit)) {
    const auto *occurrence = &writtenOccurrence;
    if (occurrence->span.source == callSpan.source &&
        occurrence->span.start >= callSpan.start &&
        occurrence->span.end <= tokenSpan(call->paren()).end &&
        (!occurrence->symbol ||
         semanticModel.database().findSymbol(occurrence->symbol)))
      semanticModel.recordOccurrence(*occurrence);
  }
  ++deferredExpansionDepth;
  ++deferredExpandedBodies;
  deferredActiveBodies.push_back(&definition);
  const std::size_t diagnosticStart = diagnostics.size();
  analyze(expansion.statement);
  deferredActiveBodies.pop_back();
  --deferredExpansionDepth;
  for (std::size_t index = diagnosticStart; index < diagnostics.size();
       ++index) {
    auto &diagnostic = diagnostics[index];
    if (definition.extent() &&
        diagnostic.primary.source == definition.extent()->source &&
        diagnostic.primary.start >= definition.extent()->start &&
        diagnostic.primary.end <= definition.extent()->end) {
      diagnostic.related.push_back(
          {diagnostic.primary, "In the expanded deferred helper body."});
      diagnostic.primary = tokenSpan(expressionToken(*call));
    }
  }
  semanticModel.recordDeferredExpansion(statement,
                                        std::move(expansion.statement));
  currentType = SemanticType::Void;
  return true;
}

} // namespace lang
