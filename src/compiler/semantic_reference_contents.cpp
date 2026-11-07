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
        loan->kind != SemanticLoanKind::StoredValue || !loan->contentsOpening ||
        loan->contentsOpening->reference != expression ||
        loan->contentsOpening->referenceType != *reference ||
        loan->entry || loan->parent || loan->storedDependency || loan->lifecycleBound ||
        inventory->fields.size() != 1 + loan->additionalDependencies.size())
      return std::nullopt;
    SemanticBorrowDependencySet result{.possibleAlternatives = *alternatives};
    const auto append = [&](SemanticLoanDependency dependency, std::size_t ordinal) {
      const auto &field = inventory->fields[ordinal];
      if (dependency.place != SemanticLoanPlace{} || !dependency.referenceContents ||
          dependency.referenceContents->openingLoan != id ||
          dependency.referenceContents->fields != field.fields ||
          dependency.designatedFields != field.fields ||
          dependency.conditions != field.conditions || dependency.parent ||
          dependency.protectsStorage || dependency.access != AccessMode::ReadOnly ||
          field.reference.referenceAccess != AccessMode::ReadOnly)
        return false;
      // The group retains the copied field's external authority. Its opening
      // read loans are not parents of this shared-reference snapshot.
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
  for (const auto &[id, flow] : loanFlow.records)
    if (flow.active)
      if (const auto *loan = semanticModel.findLoan(id); loan && loan->contentsOpening)
        loanFlow.invalidatedReferenceContents.insert(id);
}

void SemanticVisitor::Impl::recordMemberReferenceContents(const Get &expression) {
  // Staged formation routine. Connect only after source tagged-origin
  // normalization/alias checking and independent MIR opening replay exist.
  // The caller must have completed ordinary place and loan-access validation.
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
  semanticModel.recordReferenceContentsLoan(*object, id);
  semanticModel.recordBorrowDependencySet(expression, *projected);
  for (const auto read : reads)
    recordSemanticLoanAuthorityUse(read);
  recordSemanticLoanUseForLoan(id);
}

} // namespace lang
