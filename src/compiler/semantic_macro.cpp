#include "semantic_analyzer_impl.h"

#include <type_traits>
#include <utility>

// The single typed statement-expansion engine, selected by #macro syntax.

namespace lang {
namespace {

void macroRelated(Diagnostic &diagnostic, SourceSpan span,
                  std::string message) {
  if (std::any_of(diagnostic.related.begin(), diagnostic.related.end(),
                  [&](const auto &related) {
                    return related.span.source == span.source &&
                           related.span.start == span.start &&
                           related.span.end == span.end &&
                           related.message == message;
                  }))
    return;
  if (diagnostic.related.size() < 128) {
    diagnostic.related.push_back({span, std::move(message)});
    return;
  }
  constexpr std::string_view truncated =
      "Additional macro expansion context was truncated at 128 related sites.";
  if (std::find(diagnostic.hints.begin(), diagnostic.hints.end(), truncated) ==
      diagnostic.hints.end())
    diagnostic.hints.emplace_back(truncated);
}

// Resolve only the syntactic shadowing of expression parameters. Actual local
// identities, types, places, loans and cleanup remain ordinary semantic facts.
// This pass prevents substitution from replacing uses of a macro-local binder.
struct MacroParameterUses {
  std::vector<std::unordered_set<std::string>> scopes{{}};
  std::unordered_set<const Expr *> unbound;
  std::function<const StmtList *(const ConditionalStmt &)> activeBranch;

  bool bound(std::string_view name) const {
    return std::any_of(scopes.rbegin(), scopes.rend(), [&](const auto &scope) {
      return scope.contains(std::string(name));
    });
  }

  void expression(const ExprPtr &value) {
    if (!value)
      return;
    AstCloneOptions inspection;
    inspection.expression = [&](const Expr &node) -> ExprPtr {
      if (const auto *variable = exprAs<Variable>(&node);
          variable && !bound(variable->name().lexeme))
        unbound.insert(&node);
      if (const auto *assignment = exprAs<Assign>(&node);
          assignment && assignment->path().segments.size() == 1 &&
          !bound(assignment->name().lexeme))
        unbound.insert(&node);
      if (const auto *pack = exprAs<PackExpansion>(&node);
          pack && !bound(pack->name().lexeme))
        unbound.insert(&node);
      return nullptr;
    };
    (void)cloneExpr(*value, 0, inspection);
  }

  void scoped(const StmtPtr &value) {
    scopes.emplace_back();
    statement(value.get());
    scopes.pop_back();
  }

  void statement(const Stmt *value) {
    if (!value)
      return;
    switch (value->stmtKind()) {
    case StmtKind::BlockStmt: {
      scopes.emplace_back();
      for (const auto &child :
           static_cast<const BlockStmt &>(*value).statements())
        statement(child.get());
      scopes.pop_back();
      break;
    }
    case StmtKind::VariableDecl: {
      const auto &local = static_cast<const VariableDecl &>(*value);
      // Ordinary GTI initializes before introducing the new binding.
      expression(local.initializer());
      scopes.back().insert(local.name().lexeme);
      break;
    }
    case StmtKind::StructuredBindingDecl: {
      const auto &local = static_cast<const StructuredBindingDecl &>(*value);
      expression(local.initializer());
      for (const auto &binding : local.bindings())
        scopes.back().insert(binding.name().lexeme);
      break;
    }
    case StmtKind::ExpressionStmt:
      expression(static_cast<const ExpressionStmt &>(*value).expression());
      break;
    case StmtKind::ReturnStmt:
      expression(static_cast<const ReturnStmt &>(*value).value());
      break;
    case StmtKind::IfStmt: {
      const auto &branch = static_cast<const IfStmt &>(*value);
      expression(branch.condition());
      scoped(branch.thenBranch());
      scoped(branch.elseBranch());
      break;
    }
    case StmtKind::ForStmt: {
      const auto &loop = static_cast<const ForStmt &>(*value);
      scopes.emplace_back();
      statement(loop.initializer().get());
      expression(loop.condition());
      expression(loop.increment());
      scoped(loop.body());
      scopes.pop_back();
      break;
    }
    case StmtKind::WhileStmt: {
      const auto &loop = static_cast<const WhileStmt &>(*value);
      expression(loop.condition());
      scoped(loop.body());
      break;
    }
    case StmtKind::DoWhileStmt: {
      const auto &loop = static_cast<const DoWhileStmt &>(*value);
      scoped(loop.body());
      expression(loop.condition());
      break;
    }
    case StmtKind::RangeForStmt:
      statement(static_cast<const RangeForStmt &>(*value).lowered().get());
      break;
    case StmtKind::ConditionalStmt:
      if (const auto *selected =
              activeBranch(static_cast<const ConditionalStmt &>(*value)))
        for (const auto &child : *selected)
          statement(child.get());
      break;
    default:
      break; // Eligibility validation rejects unsupported statement families.
    }
  }
};
} // namespace

auto SemanticVisitor::Impl::analyzeMacroSelection(
    const Call &call, const CallableCandidate &selected,
    const std::vector<SemanticType> &typeArguments,
    const std::vector<CompileTimeValue> &valueArguments) -> bool {
  if (!selected.record().macroExpansion)
    return false;
  const bool probe = macroProbe && macroProbeCall == &call;
  const bool openStatement =
      macroStatementCall == &call &&
      std::any_of(selected.parameterTypes.begin(),
                  selected.parameterTypes.end(), [&](const SemanticType &type) {
                    return containsOpenTypeParameter(type);
                  });
  if (!probe && !openStatement) {
    diagnostics.push_back(
        makeDiagnostic(DiagnosticId::MacroExpansion, call.paren(),
                       {"A typed macro must be invoked as a complete "
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

auto SemanticVisitor::Impl::macroContext(const Token &token) const
    -> const MacroLookupContext * {
  const auto found = macroLookup.find(token.expansion);
  if (found != macroLookup.end())
    return &found->second;
  return macroLookupBase ? macroLookupBase->macroContext(token) : nullptr;
}

auto SemanticVisitor::Impl::macroLocalScopeRange(const Token &token) const
    -> MacroLocalScopeRange {
  const auto *context = macroContext(token);
  // Lambda captures are initialized in the enclosing domain, but the body
  // owns a fresh scope stack. An enclosing argument's numeric bounds must
  // neither hide lambda locals nor grant access to discarded outer frames.
  if (context && context->lexicalLambdaDepth != lambdaDepth)
    return {0, scopes.size()};
  const std::size_t end = context && context->localScopeEnd
                              ? std::min(scopes.size(), *context->localScopeEnd)
                              : scopes.size();
  const std::size_t begin = context && context->definition
                                ? std::min(end, context->localScopeBegin)
                                : 0;
  return {begin, end,
          context ? std::span(context->queryScopes)
                  : std::span<const MacroLookupContext::ScopeOverlay>()};
}

SemanticVisitor::Impl::MacroLookupScope::MacroLookupScope(
    Impl &owner, const MacroLookupContext &context)
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

SemanticVisitor::Impl::MacroLookupScope::~MacroLookupScope() {
  owner.currentNamespace = std::move(namespaceScope);
  owner.currentSourceUnit = sourceUnit;
  owner.currentClass = ownerClass;
  owner.currentStaticMemberFunction = staticMember;
  owner.currentDeclarationOrder = declarationOrder;
}

auto SemanticVisitor::Impl::validMacroBody(const Stmt &statement,
                                           std::size_t ownLoopDepth) const
    -> bool {
  const auto expressionAllowed = [](const Expr &expression) {
    bool allowed = true;
    AstCloneOptions inspection;
    inspection.expression = [&](const Expr &node) -> ExprPtr {
      switch (node.exprKind()) {
      case ExprKind::Lambda:
      case ExprKind::MemberRequirement:
      case ExprKind::PackFold:
        allowed = false;
        break;
      case ExprKind::Unary: {
        const auto kind = static_cast<const Unary &>(node).oper().kind;
        // Caller-written propagation retains its ordinary meaning, but a void
        // template cannot silently introduce a return from its caller.
        if (kind == TokenKind::QUESTION)
          allowed = false;
        break;
      }
      case ExprKind::ArrayInitializer:
      case ExprKind::Assign:
      case ExprKind::Binary:
      case ExprKind::Call:
      case ExprKind::ConditionalExpr:
      case ExprKind::Conversion:
      case ExprKind::DirectInitializer:
      case ExprKind::DereferenceSet:
      case ExprKind::Get:
      case ExprKind::Grouping:
      case ExprKind::Index:
      case ExprKind::IndexSet:
      case ExprKind::LayoutQuery:
      case ExprKind::LiteralExpr:
      case ExprKind::Logical:
      case ExprKind::PackExpansion:
      case ExprKind::PlaceSet:
      case ExprKind::Postfix:
      case ExprKind::QualifiedName:
      case ExprKind::This:
      case ExprKind::Set:
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
                         return child && validMacroBody(*child, ownLoopDepth);
                       });
  if (const auto *branch = stmtAs<IfStmt>(&statement))
    return !branch->isConstexpr() && expressionAllowed(*branch->condition()) &&
           branch->thenBranch() &&
           validMacroBody(*branch->thenBranch(), ownLoopDepth) &&
           (!branch->elseBranch() ||
            validMacroBody(*branch->elseBranch(), ownLoopDepth));
  if (const auto *branch = stmtAs<ConditionalStmt>(&statement)) {
    const StmtList *active = branch->activeBranch(target);
    return !active ||
           std::all_of(active->begin(), active->end(),
                       [&](const StmtPtr &child) {
                         return child && validMacroBody(*child, ownLoopDepth);
                       });
  }
  if (const auto *expression = stmtAs<ExpressionStmt>(&statement))
    return expressionAllowed(*expression->expression());
  if (const auto *returned = stmtAs<ReturnStmt>(&statement))
    return !returned->value() || expressionAllowed(*returned->value());
  if (const auto *local = stmtAs<VariableDecl>(&statement))
    return !local->isStatic() &&
           (!local->initializer() || expressionAllowed(*local->initializer()));
  if (const auto *local = stmtAs<StructuredBindingDecl>(&statement))
    return expressionAllowed(*local->initializer());
  if (const auto *loop = stmtAs<ForStmt>(&statement))
    return (!loop->initializer() ||
            validMacroBody(*loop->initializer(), ownLoopDepth)) &&
           (!loop->condition() || expressionAllowed(*loop->condition())) &&
           (!loop->increment() || expressionAllowed(*loop->increment())) &&
           loop->body() && validMacroBody(*loop->body(), ownLoopDepth + 1);
  if (const auto *loop = stmtAs<WhileStmt>(&statement))
    return expressionAllowed(*loop->condition()) && loop->body() &&
           validMacroBody(*loop->body(), ownLoopDepth + 1);
  if (const auto *loop = stmtAs<DoWhileStmt>(&statement))
    return expressionAllowed(*loop->condition()) && loop->body() &&
           validMacroBody(*loop->body(), ownLoopDepth + 1);
  if (const auto *loop = stmtAs<RangeForStmt>(&statement))
    return loop->lowered() && validMacroBody(*loop->lowered(), ownLoopDepth);
  if (stmtAs<LoopControlStmt>(&statement))
    return ownLoopDepth != 0;
  return stmtAs<EmptyStmt>(&statement);
}

auto SemanticVisitor::Impl::copyRequestSessionForSpeculativeModel(Impl &probe)
    -> void {
  assert(&probe != this);
  // A speculative model has its own expression order and executes none of
  // the caller's pending cleanup. Avoid copying those growing caches, but
  // keep ordinary matched session/model copies unchanged. No semantic work
  // runs while this guard temporarily removes the caller's expression state.
  struct RestoreCallerExpressionState {
    RequestSessionContext &caller;
    ProgramExpressionCache expressionCache;
    std::vector<ProgramFullExpressionState> frames;

    explicit RestoreCallerExpressionState(RequestSessionContext &caller)
        : caller(caller) {
      static_assert(std::is_nothrow_swappable_v<ProgramExpressionCache>);
      static_assert(noexcept(frames.swap(caller.fullExpressionStates)));
      std::swap(expressionCache, caller.programExpressionCache);
      frames.swap(caller.fullExpressionStates);
    }
    RestoreCallerExpressionState(const RestoreCallerExpressionState &) = delete;
    RestoreCallerExpressionState &
    operator=(const RestoreCallerExpressionState &) = delete;
    ~RestoreCallerExpressionState() noexcept {
      std::swap(expressionCache, caller.programExpressionCache);
      frames.swap(caller.fullExpressionStates);
    }
  } restore(requestSession);
  // Copying context is value/container cloning, not semantic analysis. Its
  // active loan identities, flow and counters are retained in the probe.
  static_cast<RequestSessionContext &>(probe.requestSession) = requestSession;
} // Restore the caller before creating or executing any probe model.

auto SemanticVisitor::Impl::adoptCallableTypeDiscovery(const Impl &probe,
                                                       const SemanticType &type)
    -> void {
  if (type.kind == SemanticType::Lambda) {
    for (const auto &[key, identity] :
         probe.requestSession.sourceLambdaIdentities) {
      if (identity.id != type.lambdaId)
        continue;
      requestSession.sourceLambdaIdentities.try_emplace(key, identity);
      nextLambdaId = std::max(nextLambdaId, identity.id + 1);
      if (!semanticModel.findLambda(identity.id)) {
        if (const auto *discovered =
                probe.semanticModel.findLambda(identity.id)) {
          LambdaInfo shape = *discovered;
          shape.declaration = identity.declaration;
          // Body/initializer pointers and binding identities never become
          // executable AST facts merely because a type was discovered.
          semanticModel.recordLambdaTypeDiscovery(std::move(shape));
        }
      }
      break;
    }
  }
  for (const auto &argument : type.arguments)
    adoptCallableTypeDiscovery(probe, argument);
  for (const auto &argument : type.lambdaClassTypeArguments())
    adoptCallableTypeDiscovery(probe, argument);
  for (const auto &argument : type.lambdaFunctionTypeArguments())
    adoptCallableTypeDiscovery(probe, argument);
}

auto SemanticVisitor::Impl::makeMacroProbe(const Call &call)
    -> std::unique_ptr<Impl> {
  // Resolve and type-check the complete written call in an isolated semantic
  // snapshot. In particular, a disabled helper still checks its arguments,
  // but their speculative moves, loans and writes never affect caller flow.
  auto result = std::make_unique<Impl>(
      target, sourceGraph, toolingOccurrences, nullptr, sources,
      rawPointerAudit, requireFunctionDefinitions, trustedBodies);
  Impl &probe = *result;
  copyRequestSessionForSpeculativeModel(probe);
  // Constraint queries may select another typed macro before this probe has
  // entered any expansion body. Preserve their independent recursion budget.
  probe.expressionRequirementDepth = expressionRequirementDepth;
  probe.activeRequirementQueries = activeRequirementQueries;
  probe.semanticModel.beginSpeculativeDelta(semanticModel);
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
  probe.macroProbe = true;
  probe.macroProbeCall = &call;
  for (const auto &argument : call.arguments())
    probe.macroProbeArguments.insert(argument.get());
  probe.macroLookupBase = this;
  probe.nextExpansion = nextExpansion;
  probe.macroExpansionDepth = macroExpansionDepth;
  probe.macroExpandedBodies = macroExpandedBodies;
  probe.macroActiveBodies = macroActiveBodies;
  probe.instanceAnalysisActive = instanceAnalysisActive;
  probe.instanceBaseModel = instanceBaseModel;
  probe.instanceTypeSubstitution = instanceTypeSubstitution;
  probe.instanceValueSubstitution = instanceValueSubstitution;
  probe.designatedMoves = designatedMoves;
  return result;
}

auto SemanticVisitor::Impl::analyzeMacroCall(const Call &writtenCall,
                                             const ExpressionStmt *statement)
    -> bool {
  if (currentFunctionDeclaration && currentFunctionDeclaration->isMacro())
    return false;
  const Call *call = &writtenCall;
  if (macroProbe && macroProbeCall == call)
    return false;
  struct RestoreDefinitionChain {
    decltype(macroActiveBodies) &active;
    std::optional<decltype(macroActiveBodies)> saved;
    ~RestoreDefinitionChain() {
      if (saved)
        active = std::move(*saved);
    }
  } definitionChain{macroActiveBodies, std::nullopt};
  std::optional<MacroLookupScope> lookupScope;
  if (const auto *context = macroContext(expressionToken(*call))) {
    lookupScope.emplace(*this, *context);
    // Written argument nesting is finite syntax, not a cycle in a helper's
    // definition. Keep the global expansion budget, but start a fresh
    // definition-cycle chain when forcing an invocation-owned expression.
    if (!context->definition)
      definitionChain.saved = std::exchange(macroActiveBodies, {});
  }
  const auto symbol = resolveExpressionSymbol(call->callee());
  if (!symbol || symbol->type != SemanticType::Function ||
      std::none_of(symbol->overloads.begin(), symbol->overloads.end(),
                   [](const CallableCandidate &candidate) {
                     return candidate.record().macroExpansion;
                   }))
    return false;
  const auto reject = [&](std::string message) {
    diagnostics.push_back(
        makeDiagnostic(DiagnosticId::MacroExpansion, call->paren(), {message}));
  };
  if (macroExpansionDepth >= 64 || macroExpandedBodies >= 4096) {
    reject("Macro expansion exceeds the 64-call or 4096-body resource limit.");
    return true;
  }
  auto probeOwner = makeMacroProbe(*call);
  Impl &probe = *probeOwner;
  probe.analyze(*call);
  if (hasErrorDiagnostics(probe.diagnostics)) {
    for (auto &diagnostic : probe.diagnostics) {
      macroRelated(diagnostic, tokenSpan(callableToken(call->callee())),
                   "While checking this macro invocation.");
      if (symbol->overloads.size() == 1 &&
          symbol->overloads.front().declaration)
        macroRelated(diagnostic,
                     tokenSpan(symbol->overloads.front().declaration->name()),
                     "The macro is declared here.");
    }
    diagnostics.insert(diagnostics.end(), probe.diagnostics.begin(),
                       probe.diagnostics.end());
    return true;
  }
  const auto *selection = probe.semanticModel.findCall(*call);
  const FunctionInfo *function =
      selection ? semanticModel.findFunction(selection->function) : nullptr;
  if (!function || !function->declaration || !function->macroExpansion)
    return false;
  // A selected source contract is meaningful even when its open generic
  // expansion is deferred. It is not an eager executable call boundary.
  semanticModel.recordMacroDeclaration(*call, *function->declaration);
  // A successful probe publishes only lexical callable identity and type
  // shape. Captures, loans, body facts and executable journals are established
  // afresh if/when an expanded occurrence evaluates the caller expression.
  for (const auto &type : selection->typeArguments)
    adoptCallableTypeDiscovery(probe, type);
  for (const auto &type : selection->parameterTypes)
    adoptCallableTypeDiscovery(probe, type);
  if (std::any_of(selection->parameterTypes.begin(),
                  selection->parameterTypes.end(),
                  [&](const SemanticType &type) {
                    return containsOpenTypeParameter(type);
                  })) {
    // Like a dependent compile-time branch, this statement has no primary
    // executable flow. Concrete semantic reanalysis must expand and check it;
    // do not accidentally apply ordinary eager-call moves to a generic body.
    recordSelectedCallOccurrence(*call, *selection);
    currentType = callExpressionType(selection->returnType);
    semanticModel.record(*call, expressionInfo(currentType));
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
      reject("Macro arguments require exact expression types; write the "
             "conversion explicitly at the call site.");
      return true;
    }
  }
  const FunctionDecl &definition = *function->bodyDeclaration();
  // A caller-written lambda has its own execution body. The same macro in
  // that body is not a recursive expansion of the enclosing macro template.
  const auto activeBody = std::pair{&definition, lambdaDepth};
  if (std::find(macroActiveBodies.begin(), macroActiveBodies.end(),
                activeBody) != macroActiveBodies.end()) {
    reject("Macro expansion reaches its recursion resource limit; statement "
           "templates cannot expand a recursive helper cycle.");
    return true;
  }
  if (!definition.body() || !validMacroBody(*definition.body())) {
    reject("The selected typed macro has no eligible statement body.");
    return true;
  }
  const SemanticType resultType = selection->returnType;
  if (!containsOpenTypeParameter(resultType)) {
    const auto traits = typeTraits(resultType);
    if (resultType.kind == SemanticType::Reference ||
        traits.containsBorrowedState) {
      reject("A macro result cannot export a reference or tracked borrowed "
             "state from its local result region.");
      return true;
    }
    if (resultType != SemanticType::Void && !traits.movable &&
        !traits.copyable) {
      reject("A macro result must be movable or copyable to leave its local "
             "result region; return an owning indirection for an immovable "
             "payload.");
      return true;
    }
  }
  if (!statement && resultType == SemanticType::Void) {
    reject("A void macro must be invoked as a complete expression statement.");
    return true;
  }
  if (statement && statement->discardAttribute() &&
      resultType == SemanticType::Void) {
    report(*statement->discardAttribute(),
           "'[[discard]]' cannot be applied to a void typed macro.");
    return true;
  }
  const auto &parameters = definition.parameters();
  std::unordered_map<std::string, const Expr *> arguments;
  std::unordered_set<std::string> defaultArguments;
  std::unordered_map<std::string, std::vector<const Expr *>> packs;
  MacroLookupContext caller{.namespaceScope = currentNamespace,
                            .sourceUnit = currentSourceUnit,
                            .ownerClass = currentClass,
                            .staticMember = currentStaticMemberFunction,
                            .declarationOrder = currentDeclarationOrder};
  if (const auto *parent = macroContext(expressionToken(*call)))
    caller = *parent;
  if (caller.lexicalLambdaDepth != lambdaDepth) {
    caller.localScopeBegin = 0;
    caller.localScopeEnd.reset();
  }
  caller.lexicalLambdaDepth = lambdaDepth;
  caller.localScopeEnd =
      std::min(caller.localScopeEnd.value_or(scopes.size()), scopes.size());
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
  macroLookup.emplace(
      bodyExpansion,
      MacroLookupContext{.namespaceScope = function->namespaceScope,
                         .sourceUnit = function->sourceUnit,
                         .definition = true,
                         .localScopeBegin = scopes.size(),
                         .lexicalLambdaDepth = lambdaDepth,
                         .declarationOrder =
                             declarationOrders.contains(&definition)
                                 ? declarationOrders.at(&definition)
                                 : currentDeclarationOrder,
                         .caller = origin,
                         .invocation = invocation});
  auto &bodyContext = macroLookup.at(bodyExpansion);
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
                                 const MacroLookupContext *fallback = nullptr) {
    // An argument of a nested expansion may itself contain substituted
    // caller expressions. Preserve each token's lookup domain independently.
    std::unordered_map<std::uint32_t, std::uint32_t> identities;
    AstCloneOptions options;
    options.token = [&](const Token &token) {
      Token copy = token;
      auto [entry, inserted] = identities.try_emplace(token.expansion, 0);
      if (inserted) {
        entry->second = ++nextExpansion;
        const auto *context = macroContext(token);
        MacroLookupContext domain = context    ? *context
                                    : fallback ? *fallback
                                               : caller;
        domain.callableSourceExpansion =
            context && context->callableSourceExpansion
                ? *context->callableSourceExpansion
                : token.expansion;
        // Syntax carried through an enclosing expansion may now be inside
        // its argument's lambda body. Capture this invocation's fresh stack,
        // not the numeric bounds of the discarded enclosing callable stack.
        if (domain.lexicalLambdaDepth != lambdaDepth) {
          domain.localScopeBegin = 0;
          domain.localScopeEnd.reset();
        }
        domain.lexicalLambdaDepth = lambdaDepth;
        domain.localScopeEnd = std::min(
            domain.localScopeEnd.value_or(scopes.size()), scopes.size());
        macroLookup.emplace(entry->second, std::move(domain));
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
      reject("A macro invocation requires explicit fixed arguments before a "
             "forwarded pack.");
      return true;
    }
  }
  MacroParameterUses parameterUses;
  parameterUses.activeBranch = [&](const ConditionalStmt &branch) {
    return branch.activeBranch(target);
  };
  parameterUses.statement(definition.body().get());
  AstCloneOptions options;
  options.expression = [&](const Expr &expression) -> ExprPtr {
    if (!parameterUses.unbound.contains(&expression))
      return nullptr;
    if (const auto *assignment = exprAs<Assign>(&expression)) {
      const auto argument = arguments.find(assignment->name().lexeme);
      if (argument == arguments.end())
        return nullptr;
      Token operation = assignment->oper();
      operation.expansion = bodyExpansion;
      const bool defaulted =
          defaultArguments.contains(assignment->name().lexeme);
      ExprPtr target =
          cloneArgument(*argument->second,
                        defaulted ? &macroLookup.at(bodyExpansion) : nullptr);
      if (defaulted)
        semanticModel.recordMacroDefault(*target, invocation);
      return std::make_unique<PlaceSet>(
          std::move(target), std::move(operation),
          cloneExpr(*assignment->value(), bodyExpansion, options));
    }
    const auto *variable = exprAs<Variable>(&expression);
    if (!variable)
      return nullptr;
    const auto argument = arguments.find(variable->name().lexeme);
    if (argument == arguments.end())
      return nullptr;
    const bool defaulted = defaultArguments.contains(variable->name().lexeme);
    ExprPtr value =
        cloneArgument(*argument->second,
                      defaulted ? &macroLookup.at(bodyExpansion) : nullptr);
    if (defaulted)
      semanticModel.recordMacroDefault(*value, invocation);
    return value;
  };
  options.pack = [&](const PackExpansion &pack) -> std::optional<ExprList> {
    if (!parameterUses.unbound.contains(&pack))
      return std::nullopt;
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
    reject("The macro body could not be expanded.");
    return true;
  }
  ResolvedCallInfo selected = *selection;
  selected.defaultArguments.clear(); // Probe-owned syntax is not executable.
  recordSelectedCallOccurrence(*call, std::move(selected));
  semanticModel.record(*call, expressionInfo(resultType));
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
  ++macroExpansionDepth;
  ++macroExpandedBodies;
  macroActiveBodies.push_back(activeBody);
  const std::size_t diagnosticStart = diagnostics.size();
  const ScopeStack incoming = scopes;
  macroResultRegions.push_back({.function = currentFunctionDeclaration,
                                .lambdaDepth = lambdaDepth,
                                .expansion = bodyExpansion,
                                .type = resultType});
  {
    // This is an executable lexical body even inside a namespace or field
    // initializer. Declaration classification shares the ordinary local
    // rules; the real enclosing callable/effect owner stays unchanged.
    struct ExecutableBodyScope {
      std::size_t &depth;
      explicit ExecutableBodyScope(std::size_t &depth) : depth(depth) {
        ++depth;
      }
      ~ExecutableBodyScope() { --depth; }
    } executableBody(functionDepth);
    MacroLookupScope definitionScope(*this, macroLookup.at(bodyExpansion));
    analyze(expansion.statement);
  }
  auto resultRegion = std::move(macroResultRegions.back());
  macroResultRegions.pop_back();
  std::vector<const ReturnStmt *> reachableReturns;
  const FlowSummary resultFlow =
      summarizeFlow(expansion.statement.get(), &reachableReturns);
  if (resultType != SemanticType::Void && resultFlow.canFallThrough)
    reject(
        "A value macro must return a value on every normally completing path.");
  std::vector<ScopeStack> exits;
  for (auto &[returned, state] : resultRegion.exits)
    if (std::find(reachableReturns.begin(), reachableReturns.end(), returned) !=
        reachableReturns.end())
      exits.push_back(std::move(state));
  if (resultFlow.canFallThrough)
    exits.push_back(scopes);
  if (!exits.empty())
    applyMoveStates(mergedValueStates(incoming, exits));
  macroActiveBodies.pop_back();
  --macroExpansionDepth;
  for (std::size_t index = diagnosticStart; index < diagnostics.size();
       ++index) {
    auto &diagnostic = diagnostics[index];
    if (definition.extent() &&
        diagnostic.primary.source == definition.extent()->source &&
        diagnostic.primary.start >= definition.extent()->start &&
        diagnostic.primary.end <= definition.extent()->end) {
      macroRelated(diagnostic, diagnostic.primary,
                   "In the body of macro '" + definition.name().lexeme + "'.");
      diagnostic.primary = tokenSpan(callableToken(call->callee()));
    }
    const std::string name =
        qualifiedName(function->namespaceScope, definition.name().lexeme);
    macroRelated(diagnostic, tokenSpan(callableToken(call->callee())),
                 "While expanding macro '" + name + "' here.");
    macroRelated(diagnostic, tokenSpan(definition.name()),
                 "Macro '" + name + "' is declared here.");
  }
  if (statement && resultType == SemanticType::Void &&
      resultRegion.exits.empty())
    semanticModel.recordMacroExpansion(*statement, expansion.statement,
                                       definition);
  semanticModel.recordMacroExpansion(*call, std::move(expansion.statement),
                                     definition);
  currentType = resultType;
  return true;
}

auto SemanticVisitor::Impl::analyzeMacroReturn(const ReturnStmt &statement)
    -> bool {
  if (macroResultRegions.empty() ||
      macroResultRegions.back().function != currentFunctionDeclaration ||
      macroResultRegions.back().lambdaDepth != lambdaDepth)
    return false;
  const SemanticType result = macroResultRegions.back().type;
  if (result == SemanticType::Void) {
    if (statement.value() && analyze(statement.value()) != SemanticType::Void)
      report(statement.keyword(), "A void macro cannot return a value.");
  } else if (!statement.value()) {
    report(statement.keyword(), "A value macro return requires a value.");
  } else {
    const SemanticType value = analyzeInitializer(statement.value(), result);
    const auto *info = semanticModel.findExpression(*statement.value());
    if (result.kind == SemanticType::Reference ||
        (info && info->traits.containsBorrowedState)) {
      report(statement.keyword(),
             "A macro result cannot export a reference or tracked borrowed "
             "state from its local result region.",
             "GTI-S2124");
    } else if (!recordsDeclarationConversion(*statement.value()) &&
               !isOwnershipAssignable(result, value, statement.value())) {
      const auto *variable = movedVariable(statement.value());
      // A substituted argument is an expression, not an eager by-value
      // parameter. Only the macro's own named locals get implicit return
      // movement; consuming a caller owner requires a written move.
      const auto moved =
          variable && variable->name().expansion ==
                          macroResultRegions.back().expansion
              ? analyzeImplicitReturnMove(statement.value(), result, value)
              : ImplicitReturnMoveStatus::NotEligible;
      if (moved == ImplicitReturnMoveStatus::NotEligible)
        report(statement.keyword(),
               "Cannot initialize macro result of type '" +
                   typeSpelling(result) + "' from '" + typeSpelling(value) +
                   "'.",
               "GTI-S2003");
    }
  }
  macroResultRegions.back().exits.emplace_back(&statement, scopes);
  return true;
}

} // namespace lang
