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
} // namespace

auto SemanticVisitor::Impl::unevaluatedExpressionWorker() const
    -> std::unique_ptr<Impl> {
  auto worker = std::make_unique<Impl>(
      target, sourceGraph, false, nullptr, sources, RawPointerAuditMode::Off,
      false, trustedBodies);
  static_cast<RequestSessionContext &>(worker->requestSession) = requestSession;
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
    for (const auto &[key, entry] : queries->entries)
      copy->entries.emplace(key, std::make_shared<CompleteReturnQuery>(*entry));
    worker->requestSession.completeReturnQueries = std::move(copy);
  }
  if (const auto &queries = requestSession.rawResultQueries) {
    auto copy = std::make_shared<RawResultQueryState>();
    copy->depth = queries->depth;
    copy->nextIdentity = queries->nextIdentity;
    copy->hasRawGlobals = queries->hasRawGlobals;
    std::unordered_map<const RawResultQuery *, std::shared_ptr<RawResultQuery>> entries;
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
      worker->requestSession.collectingRawResult = clone(requestSession.collectingRawResult);
    worker->requestSession.rawResultQueries = std::move(copy);
  } else if (requestSession.collectingRawResult) {
    worker->requestSession.collectingRawResult =
        std::make_shared<RawResultQuery>(*requestSession.collectingRawResult);
  }
  worker->expressionRequirementDepth = expressionRequirementDepth;
  worker->requestSession.trackAssertionDiagnostics = false;
  worker->requestSession.trackedAssertionFailed = false;
  worker->requestSession.assertionDiagnostics.clear();
  return worker;
}

auto SemanticVisitor::Impl::expressionRequirementFact(
    const MemberRequirement &query,
    std::span<const SemanticType> arguments) const
    -> std::optional<ExpandedStructuralRequirement> {
  if (!query.isExpressionRequirement() ||
      arguments.size() <= query.parameters().size())
    return std::nullopt;
  const auto parameter = [&](const Expr *expression)
      -> std::optional<std::size_t> {
    const auto *variable = exprAs<Variable>(expression);
    if (!variable)
      return std::nullopt;
    for (std::size_t index = 0; index < query.bindings().size(); ++index)
      if (query.bindings()[index].lexeme == variable->name().lexeme)
        return index;
    return std::nullopt;
  };
  const Expr *expression = query.expression().get();
  while (const auto *group = exprAs<Grouping>(expression))
    expression = group->expression().get();
  ExpandedStructuralRequirement fact{.kind = StructuralConstraintKind::ReadMember,
                                     .requiresUnsafe = query.unsafeKeyword().has_value()};
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
    operation = unaryValueOperator(unary->oper().kind);
  } else if (const auto *call = exprAs<Call>(expression)) {
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
    fact.arguments = {referent(arguments[*receiver]),
                      arguments[query.parameters().size()]};
    fact.mutableReceiver = arguments[*receiver].kind == SemanticType::Reference &&
                           arguments[*receiver].referenceAccess == AccessMode::Mutable;
  } else if (fact.memberName != "$construct") {
    return std::nullopt;
  }
  if (operation)
    fact.memberName = operatorFunctionName(*operation);
  for (const auto operand : operands)
    fact.arguments.push_back(arguments[operand]);
  return fact;
}

auto SemanticVisitor::Impl::expressionRequirementSatisfied(
    const MemberRequirement &query, std::span<const SemanticType> arguments,
    std::string *reason, const std::vector<std::string> *lookupScope) const -> bool {
  const auto reject = [&](std::string message) {
    if (reason)
      *reason = std::move(message);
    return false;
  };
  if (arguments.size() <= query.parameters().size())
    return reject("The required expression has unresolved parameter types.");
  if (std::any_of(arguments.begin(), arguments.end(), containsOpenTypeParameter)) {
    const auto wanted = expressionRequirementFact(query, arguments);
    if (wanted)
      for (const auto &active : activeStructuralRequirements())
        if (active.kind == wanted->kind && active.arguments == wanted->arguments &&
            active.memberName == wanted->memberName &&
            active.mutableReceiver == wanted->mutableReceiver &&
            (wanted->requiresUnsafe || !active.requiresUnsafe))
          return true;
    return reject("The required expression is not guaranteed for the open type parameters.");
  }
  if (expressionRequirementDepth >= 64)
    return reject("Expression requirements exceed the 64-query recursion limit.");
  ++expressionRequirementDepth;
  struct PopDepth {
    std::size_t &depth;
    ~PopDepth() { --depth; }
  } pop{expressionRequirementDepth};
  auto worker = unevaluatedExpressionWorker();
  std::unordered_map<std::string, SemanticType> bindings;
  for (std::size_t index = 0; index < query.parameters().size(); ++index)
    bindQueryType(query.parameters()[index], arguments[index], bindings);
  bindQueryType(query.result(), arguments[query.parameters().size()], bindings);
  worker->typeParameterScopes.push_back(std::move(bindings));
  worker->beginScope();
  worker->currentClass.reset();
  if (lookupScope)
    worker->currentNamespace = *lookupScope;
  worker->currentSourceUnit = sourceUnitFor(query.keyword());
  worker->unsafeDepth = query.unsafeKeyword() ? 1 : 0;
  worker->valueFlow.extendBody(worker->catalog);
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
    worker->scopes.back().emplace(binding.lexeme, std::move(symbol));
  }
  const SemanticType actual = worker->analyze(query.expression());
  for (const auto &diagnostic : worker->diagnostics)
    if (diagnostic.severity == DiagnosticSeverity::Error)
      return reject(diagnostic.message);
  const auto &required = arguments[query.parameters().size()];
  if (actual != required)
    return reject("The expression does not have the exact required owned result type.");
  if (actual.kind == SemanticType::Reference || worker->typeTraits(actual).containsBorrowedState)
    return reject("Expression requirements currently require an owned, non-borrowed result.");
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
  if (const auto capability = predicateCapabilityFor(application, arguments, lookupScope)) {
    if (capability->atomic != GenericConstraintKind::None)
      return satisfiesAtomicConstraint(capability->arguments.front(),
                                       capability->atomic) ||
             reject("The compiler-verified type fact is not satisfied.");
    if (capability->structural != StructuralConstraintKind::None)
      return structuralConstraintSatisfied(capability->structural,
                                           capability->arguments) ||
             reject("The compiler-verified type relationship is not satisfied.");
  }
  if (std::any_of(arguments.begin(), arguments.end(), containsOpenTypeParameter))
    return reject("This predicate is not established for the open type parameters.");
  auto worker = unevaluatedExpressionWorker();
  if (lookupScope)
    worker->currentNamespace = *lookupScope;
  worker->currentSourceUnit = sourceUnitFor(application.name.last());
  std::unordered_map<std::string, SemanticType> bindings;
  for (std::size_t index = 0;
       index < application.arguments.size() && index < arguments.size(); ++index)
    bindQueryType(application.arguments[index], arguments[index], bindings);
  worker->typeParameterScopes.push_back(std::move(bindings));
  worker->analyzingDependentConstantCondition = true;
  worker->constantConditionGenericDependency = false;
  const auto type = worker->analyze(*application.predicate);
  for (const auto &diagnostic : worker->diagnostics)
    if (diagnostic.severity == DiagnosticSeverity::Error)
      return reject(diagnostic.message);
  if (type != SemanticType::Bool)
    return reject("A constraint predicate must have exact type 'bool'.");
  auto context = worker->makeConstexprInterpreter();
  const auto evaluated = worker->evaluateConstexprExpression(
      *application.predicate, context);
  const bool *value = evaluated ? std::get_if<bool>(&*evaluated.value) : nullptr;
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
       index < application.arguments.size() && index < arguments.size(); ++index)
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
