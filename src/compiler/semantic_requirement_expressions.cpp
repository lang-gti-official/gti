#include "semantic_analyzer_impl.h"

#include <algorithm>

namespace lang {
namespace {
SemanticType referent(SemanticType type) {
  if (type.kind == SemanticType::Reference && type.arguments.size() == 1) {
    SemanticType value = type.arguments.front();
    return value;
  }
  return type;
}

// Reconstruct lexical type bindings from already resolved query arguments.
// No expression lookup or language policy is inferred from a public name.
void bindQueryType(const TypeRef &syntax, SemanticType type,
                   std::unordered_map<std::string, SemanticType> &bindings) {
  if (syntax.reference && type.kind == SemanticType::Reference &&
      type.arguments.size() == 1)
    type = referent(std::move(type));
  for (std::size_t index = 0; index < syntax.arrayExtents.size(); ++index) {
    if (type.kind != SemanticType::Array || type.arguments.empty())
      return;
    SemanticType element = type.arguments.front();
    type = std::move(element);
  }
  for (std::size_t index = 0; index < syntax.pointers.size(); ++index) {
    if (type.kind != SemanticType::RawPointer || type.arguments.empty())
      return;
    SemanticType pointee = type.arguments.front();
    type = std::move(pointee);
  }
  if (syntax.name.segments.size() == 1 && syntax.arguments.empty() &&
      syntax.name.last().kind == TokenKind::IDENTIFIER)
    bindings.insert_or_assign(syntax.name.last().lexeme, type);
  if (syntax.arguments.size() == type.arguments.size())
    for (std::size_t index = 0; index < syntax.arguments.size(); ++index)
      bindQueryType(syntax.arguments[index], type.arguments[index], bindings);
}

bool closedPrimitive(const SemanticType &type) {
  if (type.kind == SemanticType::Reference)
    return type.arguments.size() == 1 &&
           closedPrimitive(type.arguments.front());
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
  case SemanticType::Void:
    return true;
  default:
    return false;
  }
}

// This is cache eligibility, not a second expression checker. Only operations
// on the query's own primitive parameters qualify; ordinary analysis supplies
// the first proof, including operator legality, mutability and exact result.
bool primitiveQueryExpression(const Expr *expression,
                              const MemberRequirement &query) {
  if (const auto *variable = exprAs<Variable>(expression))
    return std::any_of(query.bindings().begin(), query.bindings().end(),
                       [&](const Token &binding) {
                         return binding.lexeme == variable->name().lexeme;
                       });
  if (const auto *group = exprAs<Grouping>(expression))
    return primitiveQueryExpression(group->expression().get(), query);
  if (const auto *unary = exprAs<Unary>(expression))
    return primitiveQueryExpression(unary->right().get(), query);
  if (const auto *binary = exprAs<Binary>(expression))
    return primitiveQueryExpression(binary->left().get(), query) &&
           primitiveQueryExpression(binary->right().get(), query);
  return false;
}
} // namespace

auto SemanticVisitor::Impl::collectIterationRequirementTypeArguments(
    const MemberRequirement &query, std::vector<const TypeRef *> &types)
    -> void {
  const auto *loop = stmtAs<RangeForStmt>(query.iteration().get());
  const auto *body = loop ? stmtAs<BlockStmt>(loop->sourceBody()) : nullptr;
  if (body)
    for (const auto &statement : body->statements())
      if (const auto *check = stmtAs<StaticAssertDecl>(statement.get()))
        collectRequirementTypeArguments(check->condition().get(), types);
}

auto SemanticVisitor::Impl::collectRequirementTypeArguments(
    const Expr *expression, std::vector<const TypeRef *> &types) -> void {
  // Keep the same pre-order as Parser::collectRequirementTypes: applied
  // requirement arguments carry these types after parameters and the result.
  if (const auto *call = exprAs<Call>(expression)) {
    for (const auto &argument : call->typeArguments())
      types.push_back(&argument);
    collectRequirementTypeArguments(call->callee().get(), types);
    for (const auto &argument : call->arguments())
      collectRequirementTypeArguments(argument.get(), types);
  } else if (const auto *binary = exprAs<Binary>(expression)) {
    collectRequirementTypeArguments(binary->left().get(), types);
    collectRequirementTypeArguments(binary->right().get(), types);
  } else if (const auto *assignment = exprAs<Assign>(expression)) {
    collectRequirementTypeArguments(assignment->value().get(), types);
  } else if (const auto *logical = exprAs<Logical>(expression)) {
    collectRequirementTypeArguments(logical->left().get(), types);
    collectRequirementTypeArguments(logical->right().get(), types);
  } else if (const auto *conditional = exprAs<ConditionalExpr>(expression)) {
    collectRequirementTypeArguments(conditional->condition().get(), types);
    collectRequirementTypeArguments(conditional->thenExpression().get(), types);
    collectRequirementTypeArguments(conditional->elseExpression().get(), types);
  } else if (const auto *conversion = exprAs<Conversion>(expression)) {
    types.push_back(&conversion->targetType());
    collectRequirementTypeArguments(conversion->value().get(), types);
  } else if (const auto *layout = exprAs<LayoutQuery>(expression)) {
    types.push_back(&layout->type());
  } else if (const auto *qualified = exprAs<QualifiedName>(expression)) {
    if (qualified->ownerType())
      types.push_back(&*qualified->ownerType());
  } else if (const auto *index = exprAs<Index>(expression)) {
    collectRequirementTypeArguments(index->object().get(), types);
    collectRequirementTypeArguments(index->index().get(), types);
  } else if (const auto *unary = exprAs<Unary>(expression)) {
    collectRequirementTypeArguments(unary->right().get(), types);
  } else if (const auto *group = exprAs<Grouping>(expression)) {
    collectRequirementTypeArguments(group->expression().get(), types);
  } else if (const auto *get = exprAs<Get>(expression)) {
    collectRequirementTypeArguments(get->object().get(), types);
  } else if (const auto *query = exprAs<MemberRequirement>(expression)) {
    if (!query->isExpressionRequirement())
      types.push_back(&query->receiver());
    for (const auto &parameter : query->parameters())
      types.push_back(&parameter);
    if (!query->inferredResult())
      types.push_back(&query->result());
    collectRequirementTypeArguments(query->expression().get(), types);
    collectIterationRequirementTypeArguments(*query, types);
  }
}

auto SemanticVisitor::Impl::unevaluatedExpressionWorker() const
    -> std::unique_ptr<Impl> {
  auto worker = std::make_unique<Impl>(target, sourceGraph, false, nullptr,
                                       sources, RawPointerAuditMode::Off, false,
                                       trustedBodies, sourceDebugProfile);
  // The query is logically const, but its fork temporarily suspends and
  // restores request-local expression scratch before any probe work begins.
  const_cast<Impl *>(this)->copyRequestSessionForSpeculativeModel(*worker);
  worker->macroLookupBase = this;
  worker->nextExpansion = nextExpansion;
  worker->macroExpansionDepth = macroExpansionDepth;
  worker->macroExpandedBodies = macroExpandedBodies;
  worker->macroActiveBodies = macroActiveBodies;
  worker->semanticModel.beginSpeculativeDelta(semanticModel);
  worker->semanticModel.setFunctionDemandSink(nullptr);
  worker->semanticModel.setCallJournal(nullptr);
  worker->diagnostics.clear();
  if (requestSession.openReanalyses)
    worker->requestSession.openReanalyses =
        std::make_shared<std::vector<InstanceAnalysisKey>>(
            *requestSession.openReanalyses);
  if (requestSession.programConcreteEffects)
    worker->requestSession.programConcreteEffects =
        std::make_shared<ProgramConcreteEffectState>(
            *requestSession.programConcreteEffects);
  if (const auto &queries = requestSession.completeReturnQueries) {
    auto copy = std::make_shared<CompleteReturnQueryState>();
    copy->depth = queries->depth;
    copy->discovered = queries->discovered;
    copy->completed = queries->completed;
    std::unordered_map<const CompleteReturnQuery *,
                       std::shared_ptr<CompleteReturnQuery>>
        entries;
    for (const auto &[key, entry] : queries->entries) {
      auto [found, inserted] = entries.try_emplace(entry.get());
      if (inserted)
        found->second = std::make_shared<CompleteReturnQuery>(*entry);
      copy->entries.emplace(key, found->second);
    }
    worker->requestSession.completeReturnQueries = std::move(copy);
  }
  if (const auto &queries = requestSession.rawResultQueries) {
    auto copy = std::make_shared<RawResultQueryState>();
    copy->depth = queries->depth;
    copy->nextIdentity = queries->nextIdentity;
    copy->hasRawGlobals = queries->hasRawGlobals;
    std::unordered_map<const RawResultQuery *, std::shared_ptr<RawResultQuery>>
        entries;
    const auto clone = [&](const std::shared_ptr<RawResultQuery> &entry) {
      auto [found, inserted] = entries.try_emplace(entry.get());
      if (inserted)
        found->second = std::make_shared<RawResultQuery>(*entry);
      return found->second;
    };
    for (const auto &entry : queries->entries)
      copy->entries.push_back(clone(entry));
    for (const auto &entry : queries->cachedRoots)
      copy->cachedRoots.push_back(clone(entry));
    for (const auto &entry : queries->familyEntries)
      copy->familyEntries.push_back(clone(entry));
    if (requestSession.collectingRawResult)
      worker->requestSession.collectingRawResult =
          clone(requestSession.collectingRawResult);
    worker->requestSession.rawResultQueries = std::move(copy);
  } else if (requestSession.collectingRawResult) {
    worker->requestSession.collectingRawResult =
        std::make_shared<RawResultQuery>(*requestSession.collectingRawResult);
  }
  // Deferred loop evidence can be shared by several pointer arguments and by
  // an active loop. Preserve those aliases within the probe, never with the
  // caller: completing speculative demand must not retire its pending use.
  std::unordered_map<const RawPointerLoopUse *,
                     std::shared_ptr<RawPointerLoopUse>>
      loopUses;
  const auto cloneLoopUse = [&](std::shared_ptr<RawPointerLoopUse> &entry) {
    if (!entry)
      return;
    auto [found, inserted] = loopUses.try_emplace(entry.get());
    if (inserted)
      found->second = std::make_shared<RawPointerLoopUse>(*entry);
    entry = found->second;
  };
  std::unordered_map<const LoanFlowState *, std::shared_ptr<LoanFlowState>>
      loanContexts;
  for (auto &loop : worker->valueFlow.rawPointerLoops)
    for (auto &access : loop.accesses) {
      cloneLoopUse(access.deferred);
      if (!access.loanContext)
        continue;
      auto [found, inserted] =
          loanContexts.try_emplace(access.loanContext.get());
      if (inserted)
        found->second = std::make_shared<LoanFlowState>(*access.loanContext);
      access.loanContext = found->second;
    }
  for (auto &call : worker->requestSession.rawBorrowCallUses) {
    for (auto &argument : call.arguments)
      cloneLoopUse(argument.loopUse);
    if (call.receiver)
      cloneLoopUse(call.receiver->loopUse);
  }
  worker->expressionRequirementDepth = expressionRequirementDepth;
  worker->activeRequirementQueries = activeRequirementQueries;
  worker->requestSession.trackAssertionDiagnostics = false;
  worker->requestSession.trackedAssertionFailed = false;
  worker->requestSession.assertionDiagnostics.clear();
  return worker;
}

auto SemanticVisitor::Impl::prepareRequirementLookup(
    const std::vector<std::string> *lookupScope, SourceUnitId unit) -> void {
  if (lookupScope) {
    // A declaration's query cannot capture the caller's locals, generic
    // spelling, proofs, or contextual result expectations. Direct body queries
    // intentionally keep their own lexical state, isolated by the worker.
    scopes.clear();
    typeParameterScopes.clear();
    valueParameterScopes.clear();
    typePackScopes.clear();
    requirementScopes.clear();
    instanceTypeSubstitution.clear();
    instanceValueSubstitution.clear();
    requestSession.lambdaUncapturedLocals.clear();
    contextualInitializerType.reset();
    contextualInitializerExpression = nullptr;
    contextualReturnValue = nullptr;
    contextualOperatorIntegerType.reset();
    contextualCallableResult.reset();
    contextualResultUse.reset();
    currentFunctionDeclaration = nullptr;
    currentReturnType = SemanticType::Unknown;
    currentNamespace = *lookupScope;
    currentClass.reset();
  }
  currentSourceUnit = unit;
}

auto SemanticVisitor::Impl::expressionRequirementFact(
    const MemberRequirement &query,
    std::span<const SemanticType> arguments) const
    -> std::optional<ExpandedStructuralRequirement> {
  if (!query.isExpressionRequirement() ||
      arguments.size() <= query.parameters().size())
    return std::nullopt;
  const auto binding = [&](const Token &name) -> std::optional<std::size_t> {
    for (std::size_t index = 0; index < query.bindings().size(); ++index)
      if (query.bindings()[index].lexeme == name.lexeme)
        return index;
    return std::nullopt;
  };
  const auto parameter =
      [&](const Expr *expression) -> std::optional<std::size_t> {
    const auto *variable = exprAs<Variable>(expression);
    return variable ? binding(variable->name()) : std::nullopt;
  };
  const Expr *expression = query.expression().get();
  bool requiresUnsafe = query.unsafeKeyword().has_value();
  while (const auto *group = exprAs<Grouping>(expression)) {
    requiresUnsafe = requiresUnsafe || group->unsafeKeyword().has_value();
    expression = group->expression().get();
  }
  ExpandedStructuralRequirement fact{.kind =
                                         StructuralConstraintKind::ReadMember,
                                     .requiresUnsafe = requiresUnsafe};
  if (const auto *loop = stmtAs<RangeForStmt>(query.iteration().get())) {
    const auto range = parameter(expression);
    // The current symbolic loop carrier has no unsafe operation bit. An
    // explicitly unsafe iteration query is therefore a concrete filter only,
    // never permission to use its operations in a safe generic body.
    if (requiresUnsafe || !range ||
        arguments[*range].kind != SemanticType::Reference)
      return std::nullopt;
    fact.kind = StructuralConstraintKind::Iteration;
    fact.mutableReceiver = loop->bindingMutability() == Mutability::Mutable &&
                           loop->bindingType().reference.has_value();
    if (fact.mutableReceiver &&
        arguments[*range].referenceAccess != AccessMode::Mutable)
      return std::nullopt;
    fact.arguments = {referent(arguments[*range])};
    if (arguments[query.parameters().size()] != SemanticType::Void)
      fact.arguments.push_back(referent(arguments[query.parameters().size()]));
    return fact;
  }
  std::optional<std::size_t> receiver;
  std::optional<OverloadedOperator> operation;
  std::vector<std::size_t> operands;
  if (const auto *binary = exprAs<Binary>(expression)) {
    receiver = parameter(binary->left().get());
    const auto right = parameter(binary->right().get());
    operation = binaryOperator(binary->oper().kind);
    if (!right)
      return std::nullopt;
    operands.push_back(*right);
  } else if (const auto *unary = exprAs<Unary>(expression)) {
    receiver = parameter(unary->right().get());
    operation = unary->oper().kind == TokenKind::PLUS_PLUS
                    ? std::optional(OverloadedOperator::PreIncrement)
                : unary->oper().kind == TokenKind::MINUS_MINUS
                    ? std::optional(OverloadedOperator::PreDecrement)
                : unary->oper().kind == TokenKind::STAR
                    ? std::optional(OverloadedOperator::Dereference)
                    : unaryValueOperator(unary->oper().kind);
  } else if (const auto *index = exprAs<Index>(expression)) {
    receiver = parameter(index->object().get());
    const auto offset = parameter(index->index().get());
    if (!offset)
      return std::nullopt;
    operation = OverloadedOperator::Subscript;
    operands.push_back(*offset);
  } else if (const auto *assignment = exprAs<Assign>(expression)) {
    if (assignment->path().segments.size() != 1)
      return std::nullopt;
    receiver = binding(assignment->name());
    operation = compoundOperator(assignment->oper().kind);
    const auto value = parameter(assignment->value().get());
    if (!operation || !value)
      return std::nullopt;
    operands.push_back(*value);
  } else if (const auto *call = exprAs<Call>(expression)) {
    // A typed call has extra selection facts not represented by the existing
    // unparameterized structural call proof. Concrete probing still supports
    // it; do not silently prove a different call in an open body.
    if (!call->typeArguments().empty())
      return std::nullopt;
    if (const auto *get = exprAs<Get>(call->callee().get())) {
      receiver = parameter(get->object().get());
      fact.memberName = get->name().lexeme;
    } else if (const auto bound = parameter(call->callee().get())) {
      receiver = bound;
      operation = OverloadedOperator::Call;
    } else if (const auto *target = exprAs<Variable>(call->callee().get());
               target && query.result().name.segments.size() == 1 &&
               target->name().lexeme == query.result().name.last().lexeme) {
      fact.memberName = "$construct";
      fact.arguments = {arguments[query.parameters().size()],
                        arguments[query.parameters().size()]};
    } else {
      return std::nullopt;
    }
    for (const auto &argument : call->arguments()) {
      const auto bound = parameter(argument.get());
      if (!bound)
        return std::nullopt;
      operands.push_back(*bound);
    }
  } else {
    return std::nullopt;
  }
  if (receiver) {
    // An owned synthetic receiver can admit operations with a different
    // access/consuming contract. It is a concrete filter, not evidence that
    // the same operation is available through a read-only or mutable borrow.
    if (arguments[*receiver].kind != SemanticType::Reference)
      return std::nullopt;
    fact.arguments = {referent(arguments[*receiver]),
                      arguments[query.parameters().size()]};
    if (query.inferredResult())
      fact.arguments[1] =
          query.borrowedFrom()
              ? SemanticType::referenceTo(SemanticType::Unknown,
                                          query.result().referenceMutability
                                              ? AccessMode::Mutable
                                              : AccessMode::ReadOnly)
              : SemanticType::Unknown;
    fact.mutableReceiver =
        arguments[*receiver].kind == SemanticType::Reference &&
        arguments[*receiver].referenceAccess == AccessMode::Mutable;
  } else if (fact.memberName != "$construct") {
    return std::nullopt;
  }
  if (operation)
    fact.memberName = operatorFunctionName(*operation);
  if (fact.memberName.empty())
    return std::nullopt;
  for (const auto operand : operands)
    fact.arguments.push_back(arguments[operand]);
  if (query.borrowedFrom()) {
    const auto origin = binding(*query.borrowedFrom());
    const auto &result = fact.arguments[1];
    if (!receiver || !origin || result.kind != SemanticType::Reference ||
        arguments[*origin].kind != SemanticType::Reference ||
        (result.referenceAccess == AccessMode::Mutable &&
         arguments[*origin].referenceAccess != AccessMode::Mutable))
      return std::nullopt;
    if (*origin == *receiver) {
      fact.borrowSource = 0;
    } else {
      const auto found = std::find(operands.begin(), operands.end(), *origin);
      if (found == operands.end())
        return std::nullopt;
      fact.borrowSource =
          2 + static_cast<std::size_t>(found - operands.begin());
    }
  }
  return fact;
}

auto SemanticVisitor::Impl::requirementQueryIdentity(
    const void *syntax, std::span<const SemanticType> arguments) const
    -> ActiveRequirementQuery {
  ActiveRequirementQuery identity{
      .syntax = syntax,
      .arguments = {arguments.begin(), arguments.end()},
      .types = instanceTypeSubstitution,
      .values = instanceValueSubstitution};
  return identity;
}

auto SemanticVisitor::Impl::expressionRequirementSatisfied(
    const MemberRequirement &query,
    std::span<const SemanticType> inputArguments, std::string *reason,
    const std::vector<std::string> *lookupScope) const -> bool {
  // A constraint on a forwarded open pack is universally quantified over
  // its elements. Probe one symbolic element with that pack's identity and
  // established facts, not a runtime pack value. Ordinary expressions outside
  // this unevaluated query retain all pack-expansion restrictions.
  const auto elementType = [](const auto &self,
                              SemanticType type) -> SemanticType {
    if (type.kind == SemanticType::TypePack && !type.concretePack)
      return SemanticType::typeParameter(type.genericParameterId);
    for (auto &argument : type.arguments)
      argument = self(self, std::move(argument));
    return type;
  };
  std::vector<SemanticType> elementArguments;
  elementArguments.reserve(inputArguments.size());
  for (const auto &argument : inputArguments)
    elementArguments.push_back(elementType(elementType, argument));
  const std::span<const SemanticType> arguments(elementArguments);
  const auto reject = [&](std::string message) {
    if (reason)
      *reason = std::move(message);
    return false;
  };
  if (arguments.size() <= query.parameters().size())
    return reject("The required expression has unresolved parameter types.");
  std::vector<const TypeRef *> extraTypes;
  collectRequirementTypeArguments(query.expression().get(), extraTypes);
  collectIterationRequirementTypeArguments(query, extraTypes);
  if (arguments.size() != query.parameters().size() + 1 + extraTypes.size())
    return reject(
        "The required expression has incomplete explicit type arguments.");
  const bool cacheable =
      !query.inferredResult() && !query.borrowedFrom() && !query.iteration() &&
      !macroContext(query.keyword()) &&
      std::all_of(arguments.begin(), arguments.end(), closedPrimitive) &&
      primitiveQueryExpression(query.expression().get(), query);
  if (cacheable) {
    const auto cached = primitiveRequirementProofs.find(&query);
    if (cached != primitiveRequirementProofs.end())
      for (const auto &proof : cached->second)
        if (std::equal(arguments.begin(), arguments.end(), proof.begin(),
                       proof.end()))
          return true;
  }
  if (std::any_of(arguments.begin(), arguments.end(),
                  containsOpenTypeParameter)) {
    const auto wanted = expressionRequirementFact(query, arguments);
    const auto *loop = stmtAs<RangeForStmt>(query.iteration().get());
    const auto *body = loop ? stmtAs<BlockStmt>(loop->sourceBody()) : nullptr;
    // An iteration fact proves that the loop can bind its element, not that
    // additional checks inside this particular query succeed.
    if (wanted && (!body || body->statements().empty()))
      for (const auto &active : activeStructuralRequirements())
        if (active.kind == wanted->kind &&
            active.arguments == wanted->arguments &&
            active.memberName == wanted->memberName &&
            active.mutableReceiver == wanted->mutableReceiver &&
            active.borrowSource == wanted->borrowSource &&
            (wanted->requiresUnsafe || !active.requiresUnsafe))
          return true;
    // Ordinary symbolic analysis can also prove built-in operations from
    // classification facts (for example integer comparisons). Do not invent
    // an operation from the query being asked: only the enclosing worker's
    // existing constraints participate, and concrete instances recheck it.
  }
  ActiveRequirementQuery identity = requirementQueryIdentity(&query, arguments);
  if (std::find(activeRequirementQueries.begin(),
                activeRequirementQueries.end(),
                identity) != activeRequirementQueries.end())
    return reject("Expression requirements contain a recursive query cycle.");
  if (expressionRequirementDepth >= 64)
    return reject(
        "Expression requirements exceed the 64-query recursion limit.");
  ++expressionRequirementDepth;
  activeRequirementQueries.push_back(std::move(identity));
  struct PopDepth {
    std::size_t &depth;
    std::vector<ActiveRequirementQuery> &active;
    ~PopDepth() {
      active.pop_back();
      --depth;
    }
  } pop{expressionRequirementDepth, activeRequirementQueries};
  auto worker = unevaluatedExpressionWorker();
  std::unordered_map<std::string, SemanticType> bindings;
  for (std::size_t index = 0; index < query.parameters().size(); ++index)
    bindQueryType(query.parameters()[index], arguments[index], bindings);
  if (!query.inferredResult())
    bindQueryType(query.result(), arguments[query.parameters().size()],
                  bindings);
  for (std::size_t index = 0; index < extraTypes.size(); ++index)
    bindQueryType(*extraTypes[index],
                  arguments[query.parameters().size() + 1 + index], bindings);
  worker->prepareRequirementLookup(lookupScope, sourceUnitFor(query.keyword()));
  // Declaration lookup must not capture caller scopes, but substituted type
  // arguments still carry their proven primitive facts. Preserve those facts
  // explicitly rather than lending the query the caller's entire requires
  // clause or allowing it to assume its own desired expression.
  const auto retainArgumentFacts = [&](const auto &self,
                                       const SemanticType &type) -> void {
    if (type.kind == SemanticType::TypeParameter)
      worker->genericConstraints[type.genericParameterId] |= constraintOf(type);
    for (const auto &argument : type.arguments)
      self(self, argument);
  };
  for (const auto &argument : arguments)
    retainArgumentFacts(retainArgumentFacts, argument);
  worker->typeParameterScopes.push_back(std::move(bindings));
  worker->typePackScopes.emplace_back();
  worker->beginScope();
  if (!query.parameters().empty() && macroContext(query.keyword())) {
    // Caller syntax retains its independent macro lookup domain. Admit only
    // the query's newly introduced frame in addition to that domain's original
    // caller range, never the macro-local frames between them. Frame identity
    // prevents this permission from surviving a fresh callable scope stack.
    auto &scope = worker->scopes.back();
    scope.identity = std::make_shared<ScopeIdentity>();
    const MacroLookupContext::ScopeOverlay overlay{worker->scopes.size() - 1,
                                                   scope.identity};
    AstCloneOptions inspection;
    inspection.token = [&](const Token &token) {
      if (const auto *context = worker->macroContext(token)) {
        MacroLookupContext local = *context;
        if (std::none_of(local.queryScopes.begin(), local.queryScopes.end(),
                         [&](const auto &entry) {
                           return entry.identity == overlay.identity;
                         }))
          local.queryScopes.push_back(overlay);
        worker->macroLookup.insert_or_assign(token.expansion, std::move(local));
      }
      return token;
    };
    (void)cloneExpr(*query.expression(), 0, inspection);
    if (query.iteration())
      (void)cloneStmt(*query.iteration(), 0, inspection);
  }
  worker->unsafeDepth = query.unsafeKeyword() ? 1 : 0;
  worker->valueFlow.extendBody(worker->catalog);
  std::vector<SymbolId> parameterIdentities;
  for (std::size_t index = 0; index < query.parameters().size(); ++index) {
    const auto &binding = query.bindings()[index];
    Symbol symbol;
    symbol.type = arguments[index];
    symbol.declaration = binding;
    symbol.sourceUnit = sourceUnitFor(binding);
    symbol.assignable = symbol.type.kind == SemanticType::Reference &&
                        symbol.type.referenceAccess == AccessMode::Mutable;
    symbol.bindingKind = SemanticBindingKind::Parameter;
    symbol.toolingSymbol = worker->recordToolingSymbol(
        binding, SymbolKind::RequirementParameter, binding.lexeme, symbol.type);
    parameterIdentities.push_back(symbol.toolingSymbol);
    worker->scopes.back().emplace(binding.lexeme, std::move(symbol));
  }
  if (const auto *loop = stmtAs<RangeForStmt>(query.iteration().get())) {
    // Query bindings and the loop's synthetic owners are local even when the
    // requirement is declared at namespace scope. No executable function or
    // caller-owned state is published by this isolated worker.
    worker->functionDepth = std::max<std::size_t>(worker->functionDepth, 1);
    worker->analyze(query.iteration());
    for (const auto &diagnostic : worker->diagnostics)
      if (diagnostic.severity == DiagnosticSeverity::Error)
        return reject(diagnostic.message);
    const auto *iteration = worker->semanticModel.findRangeIteration(*loop);
    const auto &required = arguments[query.parameters().size()];
    if (!iteration || (required != SemanticType::Void &&
                       iteration->elementType != referent(required)))
      return reject("The range does not have the exact required element type.");
    return true;
  }
  const SemanticType actual = worker->analyze(query.expression());
  for (const auto &diagnostic : worker->diagnostics)
    if (diagnostic.severity == DiagnosticSeverity::Error)
      return reject(diagnostic.message);
  if (query.inferredResult() && actual == SemanticType::Unknown)
    return reject("The expression's operation remains unresolved. An inferred "
                  "result does not prove an unresolved compound operation.");
  const auto required =
      query.inferredResult() && query.borrowedFrom()
          ? SemanticType::referenceTo(actual, query.result().referenceMutability
                                                  ? AccessMode::Mutable
                                                  : AccessMode::ReadOnly)
          : arguments[query.parameters().size()];
  if (query.borrowedFrom()) {
    const auto origin =
        std::find_if(query.bindings().begin(), query.bindings().end(),
                     [&](const Token &binding) {
                       return binding.lexeme == query.borrowedFrom()->lexeme;
                     });
    if (origin == query.bindings().end())
      return reject("The borrow origin is not a requirement parameter.");
    const auto index =
        static_cast<std::size_t>(origin - query.bindings().begin());
    if (required.kind != SemanticType::Reference ||
        required.arguments.size() != 1 ||
        arguments[index].kind != SemanticType::Reference)
      return reject("A borrowed requirement result needs a checked reference "
                    "result and a borrowed parameter origin.");
    const auto *info =
        worker->semanticModel.findExpression(*query.expression());
    if (actual != required.arguments.front() || !info ||
        info->category != ValueCategory::Place ||
        info->access != required.referenceAccess)
      return reject("The expression does not produce the exact required "
                    "checked reference access and referent.");
    if (required.referenceAccess == AccessMode::Mutable &&
        arguments[index].referenceAccess != AccessMode::Mutable)
      return reject("A mutable result requires a mutable borrowed origin.");
    const auto places = worker->loanPlaces(query.expression());
    if (places.empty() || parameterIdentities[index] == 0 ||
        std::any_of(places.begin(), places.end(),
                    [&](const SemanticLoanPlace &place) {
                      return !place.valid() || place.receiver ||
                             place.root != parameterIdentities[index];
                    }))
      return reject("The checked result is not borrowed from the named "
                    "requirement parameter.");
    return true;
  }
  if (query.inferredResult())
    return true;
  // Call expressions expose their referent as the ordinary expression type.
  // That value-facing view must not erase a selected declaration's borrowed
  // result when proving an exact owned-result requirement.
  const Expr *resultExpression = query.expression().get();
  while (const auto *group = exprAs<Grouping>(resultExpression))
    resultExpression = group->expression().get();
  const auto borrowedResult = [](const auto *resolved) {
    return resolved && (resolved->returnType.kind == SemanticType::Reference ||
                        resolved->borrowOrigin != BorrowOriginKind::None);
  };
  if (borrowedResult(worker->semanticModel.findOperator(*resultExpression)))
    return reject(
        "The selected operation returns a borrow, not an owned result.");
  if (const auto *call = exprAs<Call>(resultExpression);
      call && (borrowedResult(worker->semanticModel.findCall(*call)) ||
               borrowedResult(worker->semanticModel.findLambdaCall(*call))))
    return reject("The selected call returns a borrow, not an owned result.");
  if (actual != required)
    return reject(
        "The expression does not have the exact required owned result type.");
  if (actual.kind == SemanticType::Reference ||
      worker->typeTraits(actual).containsBorrowedState)
    return reject("Expression requirements currently require an owned, "
                  "non-borrowed result.");
  if (cacheable && primitiveRequirementProofCount < 1024) {
    primitiveRequirementProofs[&query].emplace_back(arguments.begin(),
                                                    arguments.end());
    ++primitiveRequirementProofCount;
  }
  return true;
}

auto SemanticVisitor::Impl::constraintPredicateSatisfied(
    const ConceptApplication &application,
    const std::vector<SemanticType> &arguments, std::string *reason,
    const std::vector<std::string> *lookupScope) const -> bool {
  const auto reject = [&](std::string message) {
    if (reason)
      *reason = std::move(message);
    return false;
  };
  const auto *call = exprAs<Call>(application.predicate.get());
  if (!call)
    return reject("A constraint predicate must be a constexpr boolean call.");
  if (const auto capability =
          predicateCapabilityFor(application, arguments, lookupScope)) {
    if (capability->atomic != GenericConstraintKind::None)
      return satisfiesConstraint(capability->arguments.front(),
                                 constraintBit(capability->atomic)) ||
             reject("The compiler-verified type fact is not satisfied.");
    if (capability->structural != StructuralConstraintKind::None)
      return structuralConstraintSatisfied(capability->structural,
                                           capability->arguments) ||
             reject(
                 "The compiler-verified type relationship is not satisfied.");
  }
  ActiveRequirementQuery identity =
      requirementQueryIdentity(&application, arguments);
  if (std::find(activeRequirementQueries.begin(),
                activeRequirementQueries.end(),
                identity) != activeRequirementQueries.end())
    return reject("Constraint predicates contain a recursive query cycle.");
  if (expressionRequirementDepth >= constexprCallDepthLimit)
    return reject("Constraint predicates exceed the 64-query recursion limit.");
  ++expressionRequirementDepth;
  activeRequirementQueries.push_back(std::move(identity));
  struct PopDepth {
    std::size_t &depth;
    std::vector<ActiveRequirementQuery> &active;
    ~PopDepth() {
      active.pop_back();
      --depth;
    }
  } pop{expressionRequirementDepth, activeRequirementQueries};
  auto worker = unevaluatedExpressionWorker();
  worker->prepareRequirementLookup(lookupScope,
                                   sourceUnitFor(application.name.last()));
  const auto retainArgumentFacts = [&](const auto &self,
                                       const SemanticType &type) -> void {
    if (type.kind == SemanticType::TypeParameter)
      worker->genericConstraints[type.genericParameterId] |= constraintOf(type);
    for (const auto &argument : type.arguments)
      self(self, argument);
  };
  for (const auto &argument : arguments)
    retainArgumentFacts(retainArgumentFacts, argument);
  worker->unsafeDepth = 0;
  std::unordered_map<std::string, SemanticType> bindings;
  for (std::size_t index = 0;
       index < application.arguments.size() && index < arguments.size();
       ++index)
    bindQueryType(application.arguments[index], arguments[index], bindings);
  worker->typeParameterScopes.push_back(std::move(bindings));
  worker->typePackScopes.emplace_back();
  worker->analyzingDependentConstantCondition = true;
  worker->constantConditionGenericDependency = false;
  const auto type = worker->analyze(*application.predicate);
  for (const auto &diagnostic : worker->diagnostics)
    if (diagnostic.severity == DiagnosticSeverity::Error)
      return reject(diagnostic.message);
  if (type != SemanticType::Bool)
    return reject("A constraint predicate must have exact type 'bool'.");
  auto context = worker->makeConstexprInterpreter();
  const auto evaluated =
      worker->evaluateConstexprExpression(*application.predicate, context);
  for (const auto &diagnostic : worker->diagnostics)
    if (diagnostic.severity == DiagnosticSeverity::Error)
      return reject(diagnostic.message);
  const bool *value =
      evaluated ? std::get_if<bool>(&*evaluated.value) : nullptr;
  if (!value)
    return reject(evaluated.detail.empty()
                      ? "The predicate is not a valid constant expression."
                      : evaluated.detail);
  return *value || reject("The compile-time predicate evaluated to false.");
}

auto SemanticVisitor::Impl::predicateCapabilityFor(
    const ConceptApplication &application,
    const std::vector<SemanticType> &arguments,
    const std::vector<std::string> *lookupScope) const
    -> std::optional<PredicateCapability> {
  const auto *call = exprAs<Call>(application.predicate.get());
  if (!call)
    return std::nullopt;
  // Implication discovery temporarily enters declaration-owned lexical scopes;
  // it neither evaluates code nor mutates the semantic catalog or caller flow.
  auto &context = *const_cast<Impl *>(this);
  const auto enclosingUnit = context.currentSourceUnit;
  auto enclosingNamespace = context.currentNamespace;
  if (lookupScope)
    context.currentNamespace = *lookupScope;
  context.currentSourceUnit = sourceUnitFor(application.name.last());
  std::unordered_map<std::string, SemanticType> bindings;
  for (std::size_t index = 0;
       index < application.arguments.size() && index < arguments.size();
       ++index)
    bindQueryType(application.arguments[index], arguments[index], bindings);
  context.typeParameterScopes.push_back(std::move(bindings));
  context.typePackScopes.emplace_back();
  auto capability = context.constraintPredicateCapability(*call, arguments);
  context.typePackScopes.pop_back();
  context.typeParameterScopes.pop_back();
  context.currentSourceUnit = enclosingUnit;
  context.currentNamespace = std::move(enclosingNamespace);
  return capability;
}
} // namespace lang
