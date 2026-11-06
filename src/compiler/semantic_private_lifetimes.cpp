#include "semantic_analyzer_impl.h"
#include "gti/lifetime_types.h"

namespace lang {

auto SemanticVisitor::Impl::hasExplicitLifetimeContract(const TypeRef &type) const
    -> bool {
  if (type.lifetimeBinder ||
      (type.referenceLifetime && type.referenceLifetime->name.lexeme != "_") ||
      (type.genericArgumentSyntax == GenericArgumentSyntax::Lifetime &&
       type.name.last().lexeme != "_"))
    return true;
  if (type.genericArgumentSyntax == GenericArgumentSyntax::UnresolvedIdentifier)
    for (const auto &scope : lifetimeParameterScopes)
      if (scope.contains(type.name.last().lexeme))
        return true;
  return (type.qualifiedOwner && hasExplicitLifetimeContract(*type.qualifiedOwner)) ||
         std::any_of(type.arguments.begin(), type.arguments.end(),
                     [&](const auto &argument) { return hasExplicitLifetimeContract(argument); });
}

auto SemanticVisitor::Impl::privateLifetimeInferenceEligible(
    const CallableCandidate &function) const -> bool {
  if (!function.declaration || !function.declarationRecord)
    return false;
  const auto &syntax = *function.declaration;
  if (!syntax.body() || syntax.isVirtual() || syntax.isPure() || syntax.isOverride() ||
      syntax.hasCLinkage() || syntax.runtimeBinding() || syntax.isConsteval() ||
      function.record().intrinsic != IntrinsicKind::None ||
      function.record().entryPoint || function.record().macroExpansion ||
      syntax.findFunctionAttribute("borrowed_from") ||
      std::any_of(function.record().genericParameters.begin(),
                  function.record().genericParameters.end(),
                  [](const auto &parameter) { return parameter.lifetime; }) ||
      hasExplicitLifetimeContract(syntax.returnType()) ||
      std::any_of(syntax.parameters().begin(), syntax.parameters().end(),
                  [&](const auto &parameter) { return hasExplicitLifetimeContract(parameter.type); }))
    return false;
  if (function.ownerClass) {
    if (function.access != AccessModifier::Private ||
        classInfo(function.ownerClass).kind == ClassKind::Interface)
      return false;
  } else if (!function.record().internalLinkage &&
             (!sourceGraph || sourceGraph->isExported(function.record().sourceUnit,
                                                       syntax.name().position))) {
    return false;
  }
  const auto formula = buildLifetimeRequirements(function.requirements);
  return formula && std::none_of(formula->nodes.begin(), formula->nodes.end(),
                                 [](const auto &node) {
    return node.kind == LifetimeRequirementNode::Kind::Region ||
           node.kind == LifetimeRequirementNode::Kind::Type;
  });
}

auto SemanticVisitor::Impl::prepareInferredLifetimeResult(
    SemanticType &type, const std::function<Lifetime()> &fresh) const -> void {
  if (type.kind == SemanticType::FunctionSignature ||
      type.kind == SemanticType::FunctionItem)
    return; // Callable types keep their own stable quantified boundary.
  const auto fill = [&](Lifetime &region) {
    if (!region.specified() || region.kind == Lifetime::Kind::Inference)
      region = fresh();
  };
  if (type.kind == SemanticType::Reference)
    fill(type.referenceLifetime);
  if ((type.kind == SemanticType::StringView || type.kind == SemanticType::CString) &&
      type.lifetimeArguments.empty())
    type.lifetimeArguments.emplace_back();
  for (auto &region : type.lifetimeArguments)
    fill(region);
  lifetime_detail::children(type, [&](auto &child) {
    prepareInferredLifetimeResult(child, fresh);
  });
}

auto SemanticVisitor::Impl::representableInferredLifetimeBound(
    const LifetimeSignatureContract &declaration, LifetimeBound bound) const -> bool {
  const auto contains = [&](Lifetime region) {
    return region == Lifetime::staticLifetime() ||
           std::find(declaration.parameters.begin(), declaration.parameters.end(), region) !=
               declaration.parameters.end();
  };
  return contains(bound.longer) && contains(bound.shorter);
}

auto SemanticVisitor::Impl::projectInferredLifetimeContract(
    const LifetimeSignatureContract &declaration, const SemanticLifetimeBody &body) const
    -> std::optional<LifetimeSignatureContract> {
  if (!declaration.valid() || declaration.origin != LifetimeContractOrigin::BodyInferred)
    return std::nullopt;
  auto result = declaration;
  LifetimeInferenceConstraints graph(lifetimeBoundsForContract(declaration));
  for (const auto edge : body.obligations)
    if (!graph.require(edge.longer, edge.shorter))
      return std::nullopt;
  for (const auto edge : graph.fixedRequirements()) {
    if (!representableInferredLifetimeBound(declaration, edge))
      return std::nullopt;
    if (std::find(result.bounds.begin(), result.bounds.end(), edge) == result.bounds.end())
      result.bounds.push_back(edge);
  }
  for (const auto &bound : body.typeObligations) {
    for (const auto region : freeTypeLifetimes(bound.type))
      if (!representableInferredLifetimeBound(declaration, {region, region}))
        return std::nullopt;
    for (const auto target : graph.fixedTargets(bound.region)) {
      if (!representableInferredLifetimeBound(declaration, {target, target}))
        return std::nullopt;
      const LifetimeTypeBound projected{bound.type, target};
      if (std::find(result.typeBounds.begin(), result.typeBounds.end(), projected) ==
          result.typeBounds.end())
        result.typeBounds.push_back(projected);
    }
  }
  return result.valid() ? std::optional(std::move(result)) : std::nullopt;
}

auto SemanticVisitor::Impl::validateLambdaLifetimeReturn(const ExprPtr &value) -> void {
  if (!currentLambdaReturn || !currentLambdaReturn->declaration || !value ||
      expressionNeverReturns(value.get()))
    return;
  auto &context = *currentLambdaReturn;
  auto complete = completeBorrowDependencySet(value);
  if (!complete) {
    auto dependencies = storedBorrowDependencies(value);
    if (dependencies.empty()) {
      const auto place = retainedLoanPlace(value);
      if (place.valid()) {
        SemanticLoanDependency dependency{.place = place, .access = borrowAccess(currentReturnType)};
        if (const auto source = retainedLoanSource(value))
          dependency.parent = source->loan;
        dependencies.push_back(std::move(dependency));
      }
    }
    complete = SemanticBorrowDependencySet{.dependencies = std::move(dependencies)};
  }
  FunctionInfo signature{.returnType = currentReturnType,
                          .parameterTypes = context.parameterTypes,
                          .lifetimeContract = context.lifetimeContract,
                          .staticMember = true};
  std::vector<LifetimeBound> required;
  const auto normalized = normalizedCompleteBorrow(
      signature, context.declaration->parameters(), currentReceiverMutability, *complete,
      loanFlow.collectingLifetimeContract ? &required : nullptr, context.captures);
  if (!normalized) {
    report(expressionToken(value),
           "Every possible owner returned by the lambda must satisfy its lifetime contract and preserve access and cleanup obligations.",
           "GTI-S2127");
    return;
  }
  for (const auto bound : required)
    if (!requireLifetimeRelationship(bound.longer, bound.shorter, expressionToken(value)))
      return;
  // A declared signature's full supplier set remains authoritative. The
  // inferred private summary joins every productive return path.
  if (!context.completeReturnBorrow)
    context.completeReturnBorrow = FunctionBorrowDependencySummary{};
  for (const auto &dependency : normalized->dependencies)
    if (std::find(context.completeReturnBorrow->dependencies.begin(),
                  context.completeReturnBorrow->dependencies.end(), dependency) ==
        context.completeReturnBorrow->dependencies.end())
      context.completeReturnBorrow->dependencies.push_back(dependency);
  for (const auto &alternative : normalized->possibleAlternatives)
    if (std::find(context.completeReturnBorrow->possibleAlternatives.begin(),
                  context.completeReturnBorrow->possibleAlternatives.end(), alternative) ==
        context.completeReturnBorrow->possibleAlternatives.end())
      context.completeReturnBorrow->possibleAlternatives.push_back(alternative);
  if (!context.completeReturnBorrow->dependencies.empty()) {
    const auto &first = context.completeReturnBorrow->dependencies.front();
    context.origin = first.origin;
    context.parameter = first.parameter;
    context.access = first.access;
  }
}

auto SemanticVisitor::Impl::inferPrivateLifetimeContracts() -> void {
  std::vector<const FunctionDecl *> pending;
  for (const auto &entry : catalog.callableDeclarations)
    if (entry->syntax && entry->lifetimeContract.origin == LifetimeContractOrigin::BodyInferred &&
        entry->syntax->body())
      pending.push_back(entry->syntax);
  if (pending.empty())
    return;
  const auto candidateIndex = functionCandidateBorrowSummaryIndex();
  // The lattice consists of declaration-region pairs and exact type premises.
  // Selected calls compose those facts, so revisiting all recursive groups
  // computes a monotone fixed point independent of source order. The work
  // bound also catches recursively expanding generic type premises.
  std::size_t work = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (const auto *syntax : pending) {
      if (++work > 4096) {
        report(syntax->name(),
               "Private lifetime inference exceeded its finite summary budget; declare the recursive relationship explicitly.",
               "GTI-S2127");
        for (const auto *incomplete : pending)
          if (const auto *found = semanticModel.findFunction(*incomplete)) {
            auto invalid = *found;
            invalid.lifetimeContract.complete = false;
            invalid.completeReturnBorrow.reset();
            invalid.returnBorrowOrigin = BorrowOriginKind::None;
            invalid.returnBorrowParameters.clear();
            invalid.returnBorrowFields.clear();
            semanticModel.record(*incomplete, invalid);
            if (auto *declaration = catalog.mutableCallable(*incomplete))
              declaration->lifetimeContract = invalid.lifetimeContract;
            if (const auto selected = candidateIndex.find(invalid.id); selected != candidateIndex.end())
              for (auto *candidate : selected->second)
                candidate->lifetimeContract = invalid.lifetimeContract;
            publishFunctionBorrowSummary(invalid, candidateIndex);
          }
        ++requestSession.lifetimeContractRevision;
        return;
      }
      const auto *found = semanticModel.findFunction(*syntax);
      if (!found || !found->lifetimeContract.valid())
        continue;
      const FunctionInfo function = *found;
      auto worker = unevaluatedExpressionWorker();
      if (!worker->catalog.sealed)
        worker->catalog.seal();
      worker->requestSession.collectingPrivateLifetimeContracts = true;
      std::vector<SemanticType> ownerTypes, types;
      std::vector<CompileTimeValue> ownerValues, values;
      if (function.ownerClass) {
        const auto owner = openClassType(function.ownerClass);
        ownerTypes = owner.arguments;
        ownerValues = owner.valueArguments;
      }
      for (const auto &parameter : function.genericParameters) {
        if (parameter.value)
          values.push_back(CompileTimeValue::parameter(parameter.id));
        else if (!parameter.lifetime)
          types.push_back(parameter.pack ? SemanticType::typePack(parameter.id)
                                         : SemanticType::typeParameter(parameter.id));
      }
      const auto analysis = worker->analyzeFunctionInstance(
          function.id, ownerTypes, ownerValues, types, values);
      const auto *body = analysis.model.findLifetimeBody(*syntax->body());
      if (!body)
        continue;
      auto inferred = projectInferredLifetimeContract(function.lifetimeContract, *body);
      if (!inferred || *inferred == function.lifetimeContract)
        continue;
      FunctionInfo updated = function;
      updated.lifetimeContract = std::move(*inferred);
      refreshDeclaredLifetimeBorrowSummary(updated, true);
      semanticModel.record(*syntax, updated);
      if (auto *declaration = catalog.mutableCallable(*syntax))
        declaration->lifetimeContract = updated.lifetimeContract;
      if (const auto selected = candidateIndex.find(function.id); selected != candidateIndex.end())
        for (auto *candidate : selected->second)
          candidate->lifetimeContract = updated.lifetimeContract;
      publishFunctionBorrowSummary(updated, candidateIndex);
      ++requestSession.lifetimeContractRevision;
      changed = true;
    }
  }
}

} // namespace lang
