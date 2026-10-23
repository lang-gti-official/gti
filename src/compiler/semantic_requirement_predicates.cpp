#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::recordConstraintPredicateUses(
    const std::vector<ConceptApplication> &requirements) -> void {
  // Predicate checking may run before ordinary callable registration and in a
  // detached query worker. The lexical visit records only declaration-owned
  // editor references once ordinary lookup is available; no executable call
  // or speculative effect/ownership state crosses back into the main model.
  for (const auto &requirement : requirements) {
    const auto *call = exprAs<Call>(requirement.predicate.get());
    if (!call)
      continue;
    const Expr *callee = call->callee().get();
    while (const auto *group = exprAs<Grouping>(callee))
      callee = group->expression().get();
    std::optional<NamePath> path;
    if (const auto *qualified = exprAs<QualifiedName>(callee)) {
      if (qualified->ownerType())
        continue;
      path = qualified->name();
    } else if (const auto *variable = exprAs<Variable>(callee)) {
      path = NamePath(variable->name());
    }
    if (!path || compilerPrivatePathDenied(*path))
      continue;
    recordQualifiedPathUses(*path);
    const Symbol *symbol = resolveQualified(*path);
    if (!symbol || symbol->overloads.size() != 1)
      continue;
    const auto &candidate = symbol->overloads.front();
    if (!candidate.declaration ||
        !semanticModel.findFunction(*candidate.declaration))
      continue;
    const Token &token = path->last();
    if (token.generated)
      continue;
    semanticModel.recordOccurrence(
        {.sourceUnit = sourceUnitFor(token),
         .span = tokenSpan(token),
         .kind = SemanticOccurrenceKind::Function,
         .symbol = recordFunctionSymbol(*candidate.declaration),
         .roles = OccurrenceRole::Reference | OccurrenceRole::Call,
         .name = token.lexeme,
         .type = candidate.returnType,
         .function = candidate.declaration});
  }
}

auto SemanticVisitor::Impl::registerConstraintPredicateDeclarations(
    const StmtList &statements, std::vector<std::string> scope) -> void {
  assert(!catalog.sealed);
  for (const auto &statement : statements) {
    if (const auto *conditional = stmtAs<ConditionalStmt>(statement.get())) {
      if (const StmtList *branch = conditional->activeBranch(target))
        registerConstraintPredicateDeclarations(*branch, scope);
    } else if (const auto *foreign = stmtAs<ExternCDecl>(statement.get())) {
      registerConstraintPredicateDeclarations(foreign->declarations(), scope);
    } else if (const auto *space = stmtAs<NamespaceDecl>(statement.get())) {
      auto nested = scope;
      nested.push_back(space->name().lexeme);
      registerConstraintPredicateDeclarations(space->declarations(),
                                              std::move(nested));
    } else if (const auto *function = stmtAs<FunctionDecl>(statement.get());
               function != nullptr && !function->definitionOwner()) {
      auto &entries = catalog.constraintPredicateDeclarations[qualifiedName(
          scope, function->name().lexeme)];
      if (std::none_of(entries.begin(), entries.end(), [&](const auto &entry) {
            return entry.syntax == function;
          }))
        entries.push_back({.syntax = function,
                           .sourceUnit = sourceUnitFor(function->name()),
                           .namespaceScope = scope});
    }
  }
}

auto SemanticVisitor::Impl::constraintPredicateCapability(
    const Call &call, const std::vector<SemanticType> &typeArguments)
    -> std::optional<PredicateCapability> {
  // This is deliberately an implication recognizer, not a second constexpr
  // interpreter. Only a transparent source call chain ending at a trusted
  // type-fact query carries proof; arbitrary predicates still use ordinary
  // concrete semantic checking and constant evaluation.
  std::vector<const FunctionDecl *> active;
  std::function<std::optional<PredicateCapability>(
      const Call &, const std::vector<SemanticType> *)>
      inspect = [&](const Call &expression,
                    const std::vector<SemanticType> *providedArguments)
      -> std::optional<PredicateCapability> {
    if (!expression.arguments().empty() ||
        active.size() >= constexprCallDepthLimit)
      return std::nullopt;
    const Expr *callee = expression.callee().get();
    while (const auto *group = exprAs<Grouping>(callee))
      callee = group->expression().get();
    std::optional<NamePath> path;
    if (const auto *qualified = exprAs<QualifiedName>(callee)) {
      if (qualified->ownerType())
        return std::nullopt;
      path = qualified->name();
    } else if (const auto *variable = exprAs<Variable>(callee)) {
      path = NamePath(variable->name());
    } else {
      return std::nullopt;
    }

    // Use the same namespace alias and source visibility policy as ordinary
    // lookup. Ambiguous declarations yield no proof, never a guessed winner.
    const ConstraintPredicateDeclaration *selected = nullptr;
    auto select = [&](const std::string &name) {
      const auto found = catalog.constraintPredicateDeclarations.find(name);
      if (found == catalog.constraintPredicateDeclarations.end())
        return false;
      bool visible = false;
      for (const auto &entry : found->second) {
        if (!declarationVisible(entry.sourceUnit, entry.syntax->name()) ||
            (entry.syntax->staticKeyword() &&
             entry.sourceUnit != currentSourceUnit) ||
            (compilerPrivateDeclaration(entry.sourceUnit,
                                        entry.namespaceScope) &&
             !isCompilerTrustedUnit(currentSourceUnit)))
          continue;
        visible = true;
        if (selected != nullptr)
          return true;
        selected = &entry;
      }
      return visible;
    };
    if (path->segments.size() == 1) {
      for (std::size_t depth = currentNamespace.size() + 1; depth > 0;
           --depth) {
        if (select(qualifiedName(currentNamespace, depth - 1,
                                 path->last().lexeme)))
          break;
      }
    } else {
      const NamePath prefix(
          std::vector<Token>(path->segments.begin(), path->segments.end() - 1));
      const auto space = resolveNamespacePath(prefix, currentNamespace);
      if (!space || !select(*space + "::" + path->last().lexeme))
        return std::nullopt;
    }
    if (selected == nullptr)
      return std::nullopt;

    // A second visible declaration, even a default-argument overload, makes
    // this early index insufficient to establish exact call identity.
    const auto &entries =
        catalog.constraintPredicateDeclarations.at(qualifiedName(
            selected->namespaceScope, selected->syntax->name().lexeme));
    std::size_t visibleCount = 0;
    for (const auto &entry : entries) {
      if (declarationVisible(entry.sourceUnit, entry.syntax->name()) &&
          (!entry.syntax->staticKeyword() ||
           entry.sourceUnit == currentSourceUnit) &&
          (!compilerPrivateDeclaration(entry.sourceUnit,
                                       entry.namespaceScope) ||
           isCompilerTrustedUnit(currentSourceUnit)))
        ++visibleCount;
    }
    if (visibleCount != 1)
      return std::nullopt;

    const FunctionDecl &function = *selected->syntax;
    if (!function.parameters().empty())
      return std::nullopt;
    const auto intrinsic = trustedIntrinsicKind(
        selected->sourceUnit, selected->namespaceScope, function.name().lexeme);
    if (intrinsic == IntrinsicKind::MetaTypeTrait ||
        intrinsic == IntrinsicKind::MetaTypeRelation) {
      if (typeOf(function.returnType(), selected->namespaceScope) !=
          SemanticType::Bool)
        return std::nullopt;
      const std::size_t subjects =
          intrinsic == IntrinsicKind::MetaTypeTrait ? 1 : 2;
      if (expression.typeArguments().size() != subjects + 1)
        return std::nullopt;
      const auto selector =
          genericValueArgument(expression.typeArguments().back());
      if (!selector || selector->kind != CompileTimeValue::UInt64)
        return std::nullopt;
      PredicateCapability result;
      if (intrinsic == IntrinsicKind::MetaTypeTrait) {
        const auto constraint = metaTypeTraitConstraint(selector->value);
        if (!constraint)
          return std::nullopt;
        result.atomic = *constraint;
      } else {
        if (selector->value != 1)
          return std::nullopt;
        result.structural = StructuralConstraintKind::EnumUnderlying;
      }
      for (std::size_t index = 0; index < subjects; ++index) {
        const auto type = typeOf(expression.typeArguments()[index]);
        if (type == SemanticType::Unknown)
          return std::nullopt;
        result.arguments.push_back(type);
      }
      return result;
    }

    if (!function.constexprKeyword() || function.unsafeKeyword() ||
        function.requiresClause() || function.body() == nullptr ||
        function.body()->statements().size() != 1 ||
        std::find(active.begin(), active.end(), &function) != active.end())
      return std::nullopt;
    const auto *returned =
        stmtAs<ReturnStmt>(function.body()->statements().front().get());
    if (returned == nullptr || returned->value() == nullptr)
      return std::nullopt;
    const Expr *value = returned->value().get();
    while (const auto *group = exprAs<Grouping>(value))
      value = group->expression().get();
    const auto *forwarded = exprAs<Call>(value);
    if (forwarded == nullptr || expression.typeArguments().size() !=
                                    function.genericParameters().size())
      return std::nullopt;
    std::vector<SemanticType> arguments;
    if (providedArguments != nullptr) {
      arguments = *providedArguments;
    } else {
      for (const auto &argument : expression.typeArguments())
        arguments.push_back(typeOf(argument));
    }
    if (arguments.size() != function.genericParameters().size())
      return std::nullopt;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto &parameter = function.genericParameters()[index];
      if (parameter.pack || parameter.valueType || parameter.constraint ||
          arguments[index] == SemanticType::Unknown)
        return std::nullopt;
    }

    const auto enclosingUnit = currentSourceUnit;
    auto enclosingNamespace = currentNamespace;
    const auto enclosingClass = currentClass;
    auto enclosingTypes = std::move(typeParameterScopes);
    auto enclosingValues = std::move(valueParameterScopes);
    auto enclosingPacks = std::move(typePackScopes);
    typeParameterScopes.clear();
    valueParameterScopes.clear();
    typePackScopes.clear();
    currentSourceUnit = selected->sourceUnit;
    currentNamespace = selected->namespaceScope;
    currentClass.reset();
    typeParameterScopes.emplace_back();
    typePackScopes.emplace_back();
    for (std::size_t index = 0; index < arguments.size(); ++index)
      typeParameterScopes.back().emplace(
          function.genericParameters()[index].name.lexeme, arguments[index]);
    active.push_back(&function);
    auto result = typeOf(function.returnType()) == SemanticType::Bool
                      ? inspect(*forwarded, nullptr)
                      : std::nullopt;
    active.pop_back();
    typePackScopes = std::move(enclosingPacks);
    valueParameterScopes = std::move(enclosingValues);
    typeParameterScopes = std::move(enclosingTypes);
    currentClass = enclosingClass;
    currentNamespace = std::move(enclosingNamespace);
    currentSourceUnit = enclosingUnit;
    return result;
  };
  return inspect(call, &typeArguments);
}

} // namespace lang
