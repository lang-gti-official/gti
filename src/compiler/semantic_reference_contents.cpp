#include "semantic_analyzer_impl.h"

namespace lang {

auto SemanticVisitor::Impl::referenceContentsInventory(
    const SemanticType &type) const -> std::optional<ReferenceContentsInventory> {
  const auto inventory = storedReferenceInventory(type);
  if (!inventory)
    return std::nullopt;
  ReferenceContentsInventory result;
  for (const auto &path : *inventory) {
    auto fields = storedBorrowPath(type, path.fields);
    auto conditions = storedBorrowConditions(type, path.conditions);
    if (fields.empty() || fields.size() != path.fields.size() || !conditions ||
        path.reference.kind != SemanticType::Reference ||
        path.reference.arguments.size() != 1 ||
        !path.reference.referenceLifetime.specified())
      return std::nullopt;
    result.fields.push_back({.fields = std::move(fields),
                             .reference = path.reference,
                             .conditions = std::move(*conditions)});
  }
  return result;
}

auto SemanticVisitor::Impl::referenceSupplierPaths(const SemanticType &type) const
    -> ReferenceSupplierQuery {
  return collectReferenceSupplierPaths(type, [&](const auto &selected) {
    return referenceContentsInventory(selected);
  });
}

auto SemanticVisitor::Impl::checkedReferenceExpressionType(
    const Expr *expression) const -> std::optional<SemanticType> {
  std::unordered_set<const Expr *> visited;
  while (expression && visited.size() < borrowInventoryDepthLimit &&
         visited.insert(expression).second) {
    if (const auto *alias = semanticModel.findExpressionAlias(*expression)) {
      expression = alias;
      continue;
    }
    if (const auto *group = exprAs<Grouping>(expression)) {
      expression = group->expression().get();
      continue;
    }
    if (const auto *binary = exprAs<Binary>(expression);
        binary && binary->oper().kind == TokenKind::COMMA) {
      expression = binary->right().get();
      continue;
    }
    std::optional<SemanticType> selected;
    if (const auto *call = exprAs<Call>(expression)) {
      if (const auto *resolved = semanticModel.findCall(*call))
        selected = resolved->returnType;
      else if (const auto *resolved = semanticModel.findLambdaCall(*call))
        selected = resolved->returnType;
    }
    if (!selected)
      if (const auto *resolved = semanticModel.findOperator(*expression))
        selected = resolved->returnType;
    if (!selected)
      if (const auto *field = exprAs<Get>(expression)) {
        const auto owner = memberAccessObjectType(*field);
        if (const auto *member = findMember(owner, field->name()))
          selected = substituteSymbol(member->symbol, owner).type;
      }
    if (!selected)
      if (const auto *variable = exprAs<Variable>(expression))
        if (const auto *symbol = resolve(variable->name()))
          selected = symbol->type;
    if (!selected)
      if (const auto *qualified = exprAs<QualifiedName>(expression))
        if (const auto *symbol = resolveQualified(qualified->name()))
          selected = symbol->type;
    if (!selected || selected->kind != SemanticType::Reference ||
        selected->arguments.size() != 1 ||
        !selected->referenceLifetime.specified())
      return std::nullopt;
    const auto *info = semanticModel.findExpression(*expression);
    if (!info || info->category != ValueCategory::Place ||
        !equivalentLifetimeTypes(selected->arguments.front(), info->type))
      return std::nullopt;
    return selected;
  }
  return std::nullopt;
}

auto SemanticVisitor::Impl::referenceContentsSelector(const Expr *expression) const
    -> std::optional<ReferenceContentsSelector> {
  BorrowInventoryBudget budget;
  std::unordered_set<const Expr *> visited;
  const auto select = [&](const auto &self, const Expr *node)
      -> std::optional<ReferenceContentsSelector> {
    if (!node || !budget.consume() || visited.size() >= borrowInventoryDepthLimit ||
        !visited.insert(node).second)
      return std::nullopt;
    if (const auto *alias = semanticModel.findExpressionAlias(*node))
      return self(self, alias);
    if (const auto *group = exprAs<Grouping>(node))
      return self(self, group->expression().get());
    if (const auto *comma = exprAs<Binary>(node);
        comma && comma->oper().kind == TokenKind::COMMA)
      return self(self, comma->right().get());
    const auto *info = semanticModel.findExpression(*node);
    if (!info || info->type == SemanticType::Unknown ||
        info->category != ValueCategory::Place)
      return std::nullopt;
    if (exprAs<This>(node)) {
      // A lambda's captured outer receiver needs its exact capture selector;
      // the lexical `this` must not become the closure's own receiver shell.
      if (currentLambdaReturn)
        return std::nullopt;
      return ReferenceContentsSelector{.receiver = true, .type = info->type};
    }
    if (const auto *field = exprAs<Get>(node)) {
      if (field->access().kind == TokenKind::ARROW)
        return std::nullopt;
      auto source = self(self, field->object().get());
      const auto owner = memberAccessObjectType(*field);
      const auto *member = findMember(owner, field->name());
      const auto path = member ? memberBorrowPath(owner, member->symbol) : std::nullopt;
      if (!source || !path)
        return std::nullopt;
      const auto &selectedOwner = source->type.kind == SemanticType::Reference &&
                                          source->type.arguments.size() == 1
                                      ? source->type.arguments.front() : source->type;
      if (!equivalentLifetimeTypes(selectedOwner, owner) ||
          source->fields.size() + path->size() >= borrowInventoryDepthLimit)
        return std::nullopt;
      source->fields.insert(source->fields.end(), path->begin(), path->end());
      source->type = substituteSymbol(member->symbol, owner).type;
      const auto &visible = source->type.kind == SemanticType::Reference &&
                                    source->type.arguments.size() == 1
                                ? source->type.arguments.front() : source->type;
      if (!equivalentLifetimeTypes(visible, info->type))
        return std::nullopt;
      return source;
    }
    if (const auto *index = exprAs<Index>(node)) {
      auto source = self(self, index->object().get());
      const auto constant = semanticModel.findConstant(*index->index());
      const auto *integer = constant ? std::get_if<ConstantInteger>(&*constant) : nullptr;
      if (!source || !integer || integer->negative ||
          source->fields.size() + 1 >= borrowInventoryDepthLimit)
        return std::nullopt;
      const auto step = constantIndexProjection(integer->magnitude);
      const auto selected = borrowedPathElement(source->type, step);
      if (!selected)
        return std::nullopt;
      const auto &visible = selected->kind == SemanticType::Reference && selected->arguments.size() == 1
                                ? selected->arguments.front() : *selected;
      if (!equivalentLifetimeTypes(visible, info->type))
        return std::nullopt;
      source->fields.push_back(step);
      source->type = *selected;
      return source;
    }
    if (!exprAs<Variable>(node) && !exprAs<QualifiedName>(node))
      return std::nullopt; // Calls/unsafe owner promises do not identify an address selector.
    const auto id = semanticModel.findResolvedSymbol(*node);
    const auto *record = semanticModel.database().findSymbol(id);
    if (!id || !record)
      return std::nullopt;
    const Symbol *symbol = nullptr;
    if (const auto *variable = exprAs<Variable>(node))
      symbol = resolve(variable->name());
    else if (const auto *qualified = exprAs<QualifiedName>(node))
      symbol = resolveQualified(qualified->name());
    if (symbol && symbol->toolingSymbol != id)
      symbol = nullptr;
    const auto type = symbol ? symbol->type : record->type;
    const auto &visible = type.kind == SemanticType::Reference && type.arguments.size() == 1
                              ? type.arguments.front() : type;
    if (!equivalentLifetimeTypes(visible, info->type))
      return std::nullopt;
    if (record->kind == SymbolKind::Field && !record->staticMember) {
      const auto path = symbol && currentClass && !currentLambdaReturn
                            ? memberBorrowPath(openClassType(*currentClass), *symbol)
                            : std::nullopt;
      if (!path)
        return std::nullopt;
      return ReferenceContentsSelector{.receiver = true, .fields = *path, .type = type};
    }
    return ReferenceContentsSelector{.root = id, .type = type};
  };
  return select(select, expression);
}

auto SemanticVisitor::Impl::checkedReferenceContentsOpening(SemanticLoanId id) const
    -> std::optional<SemanticBorrowDependencySet> {
  const auto *loan = semanticModel.findLoan(id);
  if (!loan || loan->kind != SemanticLoanKind::StoredValue || !loan->contentsOpening ||
      loan->entry || loan->parent || loan->storedDependency || loan->lifecycleBound ||
      loan->contentsOpening->readLoans.empty())
    return std::nullopt;
  const auto &opening = *loan->contentsOpening;
  const auto reference = checkedReferenceExpressionType(opening.reference);
  if (!reference || *reference != opening.referenceType)
    return std::nullopt;
  const auto inventory = referenceContentsInventory(reference->arguments.front());
  const auto alternatives = storedBorrowAlternativeInventory(reference->arguments.front());
  if (!inventory || !alternatives ||
      inventory->fields.size() != 1 + loan->additionalDependencies.size())
    return std::nullopt;
  // Formation captures the read capabilities while the evaluated reference is
  // active. Do not re-query its current dependency set here: later writes or
  // branch analysis may change that set without revoking a shared snapshot.
  // HIR/MIR replay must independently match these capabilities at formation.
  BorrowInventoryBudget budget;
  std::unordered_set<SemanticLoanId> checked;
  std::unordered_set<SemanticLoanId> visiting;
  std::vector<std::pair<SemanticLoanId, bool>> pending;
  for (const auto read : opening.readLoans) {
    if (!read || !visiting.insert(read).second)
      return std::nullopt;
    pending.emplace_back(read, false);
  }
  visiting.clear();
  while (!pending.empty()) {
    if (!budget.consume())
      return std::nullopt;
    const auto [read, leaving] = pending.back();
    pending.pop_back();
    if (!read || checked.contains(read))
      continue;
    if (leaving) {
      visiting.erase(read);
      checked.insert(read);
      continue;
    }
    const auto *authority = semanticModel.findLoan(read);
    if (read == id || !visiting.insert(read).second || !authority ||
        (authority->lifecycleBound && !authority->entry))
      return std::nullopt;
    pending.emplace_back(read, true);
    pending.emplace_back(authority->parent, false);
    pending.emplace_back(authority->storedDependency, false);
    for (const auto &dependency : authority->additionalDependencies)
      pending.emplace_back(dependency.parent, false);
  }
  SemanticBorrowDependencySet result{.possibleAlternatives = *alternatives};
  const auto append = [&](SemanticLoanDependency dependency, std::size_t ordinal) {
    const auto &field = inventory->fields[ordinal];
    if (dependency.place != SemanticLoanPlace{} || !dependency.referenceContents ||
        dependency.referenceContents->openingLoan != id ||
        dependency.referenceContents->fields != field.fields ||
        dependency.designatedFields != field.fields || dependency.conditions != field.conditions ||
        dependency.parent || dependency.protectsStorage || dependency.access != AccessMode::ReadOnly ||
        field.reference.referenceAccess != AccessMode::ReadOnly)
      return false;
    dependency.parent = id;
    result.dependencies.push_back(std::move(dependency));
    return true;
  };
  if (!append({.place = loan->place, .referenceContents = loan->referenceContents,
               .designatedFields = loan->designatedFields, .access = loan->access,
               .protectsStorage = loan->protectsStorage, .parent = loan->parent,
               .conditions = loan->conditions}, 0))
    return std::nullopt;
  for (std::size_t index = 0; index < loan->additionalDependencies.size(); ++index)
    if (!append(loan->additionalDependencies[index], index + 1))
      return std::nullopt;
  return result;
}

auto SemanticVisitor::Impl::referenceContentsSourceSelector(
    const SemanticLoanDependency &dependency) const -> std::optional<ReferenceContentsSelector> {
  if (dependency.place != SemanticLoanPlace{} || !dependency.referenceContents ||
      dependency.protectsStorage || dependency.access != AccessMode::ReadOnly)
    return std::nullopt;
  const auto &source = *dependency.referenceContents;
  const auto contents = checkedReferenceContentsOpening(source.openingLoan);
  if (!contents || !loanIsAncestor(source.openingLoan, dependency.parent) ||
      std::none_of(contents->dependencies.begin(), contents->dependencies.end(), [&](const auto &field) {
        return field.referenceContents == dependency.referenceContents && field.access == dependency.access;
      }))
    return std::nullopt;
  const auto *loan = semanticModel.findLoan(source.openingLoan);
  auto selector = referenceContentsSelector(loan->contentsOpening->reference);
  if (!selector || selector->type != loan->contentsOpening->referenceType ||
      selector->fields.size() + source.fields.size() >= borrowInventoryDepthLimit)
    return std::nullopt;
  for (const auto &field : source.fields) {
    const auto selected = borrowedPathElement(selector->type, field);
    if (!selected)
      return std::nullopt;
    selector->fields.push_back(field);
    selector->type = *selected;
  }
  if (selector->type.kind != SemanticType::Reference || selector->type.arguments.size() != 1 ||
      selector->type.referenceAccess != dependency.access)
    return std::nullopt;
  return selector;
}

auto SemanticVisitor::Impl::referenceContentsDependencySet(
    const Expr *expression) const -> std::optional<SemanticBorrowDependencySet> {
  const auto reference = checkedReferenceExpressionType(expression);
  if (!reference)
    return std::nullopt;
  const auto inventory = referenceContentsInventory(reference->arguments.front());
  const auto alternatives = storedBorrowAlternativeInventory(reference->arguments.front());
  if (!inventory || !alternatives)
    return std::nullopt;
  const auto active = [&](SemanticLoanId id) {
    const auto flow = loanFlow.records.find(id);
    return id && flow != loanFlow.records.end() && flow->second.active;
  };
  const auto id = semanticModel.findReferenceContentsLoan(*expression);
  if (id) {
    const auto *loan = semanticModel.findLoan(id);
    if (!loan || !active(id) || loanFlow.invalidatedReferenceContents.contains(id) ||
        !loan->contentsOpening ||
        loan->contentsOpening->reference != expression ||
        loan->contentsOpening->referenceType != *reference)
      return std::nullopt;
    return checkedReferenceContentsOpening(id);
  }
  // A formal Reference<T> already has a checked separate contents authority.
  // Reuse the published value fact only through that explicit edge, never by
  // treating the scalar address dependency as a T value.
  const auto *variable = exprAs<Variable>(expression);
  const auto *carrier = variable ? resolve(variable->name()) : nullptr;
  const auto *physical = carrier ? semanticModel.findLoan(carrier->retainedLoan) : nullptr;
  const auto *stored = physical ? semanticModel.findLoan(physical->storedDependency) : nullptr;
  if (!carrier || carrier->valueState != ValueState::Available ||
      carrier->type != *reference || !carrier->completeBorrowSet ||
      !physical || physical->kind != SemanticLoanKind::Reference || !active(physical->id) ||
      !stored || stored->kind != SemanticLoanKind::StoredValue || !active(stored->id) ||
      !hasCompleteBorrowAlternativeCoverage(*alternatives,
                                           carrier->completeBorrowSet->possibleAlternatives))
    return std::nullopt;
  for (const auto &dependency : carrier->completeBorrowSet->dependencies)
    if (!active(dependency.parent) ||
        std::none_of(inventory->fields.begin(), inventory->fields.end(), [&](const auto &field) {
          return dependency.designatedFields == field.fields &&
                 dependency.conditions == field.conditions &&
                 dependency.access == field.reference.referenceAccess;
        }))
      return std::nullopt;
  for (const auto &field : inventory->fields) {
    const bool required = field.conditions.empty() || std::any_of(
        carrier->completeBorrowSet->possibleAlternatives.begin(),
        carrier->completeBorrowSet->possibleAlternatives.end(), [&](const auto &alternative) {
          return std::all_of(field.conditions.begin(), field.conditions.end(), [&](const auto &condition) {
            return std::find(alternative.conditions.begin(), alternative.conditions.end(), condition) !=
                   alternative.conditions.end();
          });
        });
    if (required && std::none_of(carrier->completeBorrowSet->dependencies.begin(),
                                carrier->completeBorrowSet->dependencies.end(), [&](const auto &dependency) {
          return dependency.designatedFields == field.fields &&
                 dependency.conditions == field.conditions &&
                 dependency.access == field.reference.referenceAccess;
        }))
      return std::nullopt;
  }
  return *carrier->completeBorrowSet;
}

void SemanticVisitor::Impl::invalidateReferenceContents() {
  for (const auto id : loanFlow.referenceContentsOpenings)
    if (const auto flow = loanFlow.records.find(id);
        flow != loanFlow.records.end() && flow->second.active)
      if (const auto *loan = semanticModel.findLoan(id); loan && loan->contentsOpening)
        loanFlow.invalidatedReferenceContents.insert(id);
}

void SemanticVisitor::Impl::recordMemberReferenceContents(const Get &expression) {
  // Staged formation routine. Connect only after source tagged-origin
  // normalization/alias checking and independent MIR opening replay exist.
  // The caller must have completed ordinary place and loan-access validation.
  if (loanFlow.finalized)
    return;
  const auto *object = expression.object().get();
  const auto reference = checkedReferenceExpressionType(object);
  if (!reference || referenceContentsDependencySet(object))
    return;
  const auto owner = memberAccessObjectType(expression);
  const auto *member = findMember(owner, expression.name());
  const auto *resultInfo = semanticModel.findExpression(expression);
  if (!member || !resultInfo || resultInfo->type == SemanticType::Unknown)
    return;
  const auto selected = substituteSymbol(member->symbol, owner).type;
  if (selected.kind != SemanticType::Reference &&
      !typeTraits(selected).containsBorrowedState)
    return;
  const auto prefix = memberBorrowPath(owner, member->symbol);
  const auto inventory = referenceContentsInventory(reference->arguments.front());
  const auto alternatives = storedBorrowAlternativeInventory(reference->arguments.front());
  // Mutable fields need a retained outer permission parent or an independently
  // checked transfer; an outlives relation alone must never detach them.
  if (!prefix || !inventory || inventory->fields.empty() || !alternatives ||
      std::any_of(inventory->fields.begin(), inventory->fields.end(), [](const auto &field) {
        return field.reference.referenceAccess != AccessMode::ReadOnly;
      }))
    return;
  std::vector<SemanticLoanId> reads;
  if (const auto *variable = exprAs<Variable>(object))
    if (const auto *carrier = resolve(variable->name());
        carrier && carrier->valueState == ValueState::Available)
      if (const auto *physical = semanticModel.findLoan(carrier->retainedLoan);
          physical && physical->kind == SemanticLoanKind::Reference)
        appendUniqueLoan(reads, physical->id);
  if (reads.empty()) {
    const auto address = completeBorrowDependencySet(object);
    if (!address || address->dependencies.empty() || !address->possibleAlternatives.empty())
      return;
    for (const auto &dependency : address->dependencies) {
      if (!dependency.parent || !dependency.designatedFields.empty() ||
          !dependency.conditions.empty())
        return;
      appendUniqueLoan(reads, dependency.parent);
    }
  }
  BorrowInventoryBudget budget;
  std::unordered_set<SemanticLoanId> visiting;
  std::unordered_set<SemanticLoanId> checked;
  std::vector<std::pair<SemanticLoanId, bool>> pending;
  for (const auto id : reads)
    pending.emplace_back(id, false);
  while (!pending.empty()) {
    if (!budget.consume())
      return;
    const auto [id, leaving] = pending.back();
    pending.pop_back();
    if (!id || checked.contains(id))
      continue;
    if (leaving) {
      visiting.erase(id);
      checked.insert(id);
      continue;
    }
    const auto *loan = semanticModel.findLoan(id);
    const auto active = loanFlow.records.find(id);
    if (!visiting.insert(id).second || !loan || active == loanFlow.records.end() ||
        !active->second.active || (loan->lifecycleBound && !loan->entry))
      return;
    pending.emplace_back(id, true);
    pending.emplace_back(loan->parent, false);
    pending.emplace_back(loan->storedDependency, false);
    for (const auto &dependency : loan->additionalDependencies)
      pending.emplace_back(dependency.parent, false);
  }
  const auto id = nextSemanticLoanId;
  if (!id || id == std::numeric_limits<SemanticLoanId>::max())
    return;
  SemanticBorrowDependencySet contents{.possibleAlternatives = *alternatives};
  for (const auto &field : inventory->fields)
    contents.dependencies.push_back({
        .referenceContents = SemanticReferenceContentsSource{id, field.fields},
        .designatedFields = field.fields, .access = AccessMode::ReadOnly,
        .parent = id, .conditions = field.conditions});
  const auto projected = projectBorrowDependencySet(
      contents, reference->arguments.front(), *prefix, resultInfo->type);
  if (!projected)
    return;
  // Publish only after the complete direct inventory, access/read ancestry,
  // guards, and exact result projection have all succeeded.
  const auto &first = contents.dependencies.front();
  SemanticLoanInfo opening{.id = id, .kind = SemanticLoanKind::StoredValue,
                           .origin = object, .referenceContents = first.referenceContents,
                           .access = AccessMode::ReadOnly,
                           .designatedFields = first.designatedFields,
                           .conditions = first.conditions,
                           .contentsOpening = SemanticReferenceContentsOpening{
                               object, *reference, reads}};
  for (std::size_t index = 1; index < contents.dependencies.size(); ++index) {
    auto dependency = contents.dependencies[index];
    dependency.parent = 0;
    opening.additionalDependencies.push_back(std::move(dependency));
  }
  ++nextSemanticLoanId;
  semanticModel.recordLoan(std::move(opening));
  loanFlow.records.emplace(id, LoanFlowRecord{
      .id = id, .region = loanFlow.currentRegion,
      .lastUseOrder = loanFlow.currentOrder, .lastUse = loanFlow.currentStatement});
  loanFlow.referenceContentsOpenings.push_back(id);
  semanticModel.recordReferenceContentsLoan(*object, id);
  semanticModel.recordBorrowDependencySet(expression, *projected);
  for (const auto read : reads)
    recordSemanticLoanAuthorityUse(read);
  recordSemanticLoanUseForLoan(id);
}

} // namespace lang
