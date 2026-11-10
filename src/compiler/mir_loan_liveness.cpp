#include "mir_loan_liveness.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>

namespace lang {
namespace {

// A statement list: the region in which a loan created by one of its
// statements stays confined unless a use escapes it.
struct RegionKey {
  HirStatementId owner = 0; // 0: the body's root list
  std::size_t list = 0;     // which child list of the owner
  friend bool operator==(const RegionKey &, const RegionKey &) = default;
};

class StatementTree {
public:
  explicit StatementTree(const HirBody &body) : body(body) {
    std::size_t next = 0;
    for (const HirStatementId root : body.roots)
      visit(root, 0, {0, 0}, next);
    if (!body.roots.empty())
      lastInRegion.insert(body.roots.back());
  }

  [[nodiscard]] RegionKey regionOf(HirStatementId id) const {
    const auto found = region.find(id);
    return found == region.end() ? RegionKey{~HirStatementId{0}, 0}
                                 : found->second;
  }
  [[nodiscard]] bool lastInList(HirStatementId id) const {
    return lastInRegion.contains(id);
  }
  [[nodiscard]] HirStatementId parentOf(HirStatementId id) const {
    const auto found = parent.find(id);
    return found == parent.end() ? 0 : found->second;
  }
  [[nodiscard]] std::size_t orderOf(HirStatementId id) const {
    const auto found = order.find(id);
    return found == order.end() ? 0 : found->second;
  }
  [[nodiscard]] std::optional<HirStatementId>
  statementOfValue(HirValueId value) const {
    const auto found = valueStatement.find(value);
    if (found == valueStatement.end())
      return std::nullopt;
    return found->second;
  }

private:
  void visit(HirStatementId id, HirStatementId owner, RegionKey key,
             std::size_t &next) {
    const HirStatement *statement = body.findStatement(id);
    if (statement == nullptr || order.contains(id))
      return;
    parent[id] = owner;
    region[id] = key;
    order[id] = next++;
    const auto attach = [&](std::optional<HirValueId> value) {
      if (value)
        attachValue(*value, id, next);
    };
    attach(statement->value);
    attach(statement->condition);
    attach(statement->increment);
    for (const auto &element : statement->structuredBindings)
      attach(element.index);

    std::size_t list = 0;
    const auto child = [&](std::optional<HirStatementId> nested) {
      ++list;
      if (nested)
        visit(*nested, id, {id, list}, next);
    };
    child(statement->initializer);
    child(statement->body);
    child(statement->elseBranch);
    ++list;
    for (const HirStatementId nested : statement->statements)
      visit(nested, id, {id, list}, next);
    for (const HirSwitchArm &arm : statement->switchArms) {
      ++list;
      for (const auto &payload : arm.payloadBindings)
        attachValue(payload.value, id, next);
      for (const HirStatementId nested : arm.statements)
        visit(nested, id, {id, list}, next);
    }
  }

  void attachValue(HirValueId id, HirStatementId statement, std::size_t &next) {
    if (id == 0 || valueStatement.contains(id))
      return;
    const HirValue *value = body.findValue(id);
    if (value == nullptr)
      return;
    valueStatement.emplace(id, statement);
    statementValues[statement].push_back(id);
    for (const HirValueId operand : value->operands)
      attachValue(operand, statement, next);
    for (const HirValueId input : value->sequencedInputs)
      attachValue(input, statement, next);
    for (const HirValueId argument : value->defaultArguments)
      attachValue(argument, statement, next);
    if (value->receiver)
      attachValue(*value->receiver, statement, next);
    if (value->callPlan) {
      if (value->callPlan->receiver)
        attachValue(value->callPlan->receiver->value, statement, next);
      for (const auto &argument : value->callPlan->arguments)
        attachValue(argument.value, statement, next);
    }
    if (value->retainedReceiverOwner)
      attachValue(*value->retainedReceiverOwner, statement, next);
    if (value->regionBody) {
      // A result region's statements execute at this expression's position.
      std::size_t list = 1000;
      visit(*value->regionBody, statement, {statement, list}, next);
    }
  }

  const HirBody &body;
  std::unordered_map<HirStatementId, HirStatementId> parent;
  std::unordered_map<HirStatementId, RegionKey> region;
  std::unordered_map<HirStatementId, std::size_t> order;
  std::unordered_set<HirStatementId> lastInRegion;
  std::unordered_map<HirValueId, HirStatementId> valueStatement;
  std::unordered_map<HirStatementId, std::vector<HirValueId>> statementValues;

public:
  [[nodiscard]] const std::vector<HirValueId> &
  valuesOf(HirStatementId statement) const {
    static const std::vector<HirValueId> none;
    const auto found = statementValues.find(statement);
    return found == statementValues.end() ? none : found->second;
  }
};

// The root symbol a place-valued expression designates, through member and
// index projections.
SymbolId placeRoot(const HirBody &body, HirValueId id) {
  for (std::size_t depth = 0; depth < 64 && id != 0; ++depth) {
    const HirValue *value = body.findValue(id);
    if (value == nullptr)
      return 0;
    if (value->symbol != 0 && (value->kind == HirValueKind::Variable))
      return value->symbol;
    if (value->operands.empty())
      return 0;
    id = value->operands.front();
  }
  return 0;
}

// Whether a statement accesses `root` in a way that conflicts with a loan of
// the given access: any access for an exclusive loan, a write or mutable
// borrow for a shared one.
bool conflictsWithLoan(const HirBody &body, const StatementTree &tree,
                       HirStatementId statement, SymbolId root,
                       AccessMode loanAccess) {
  if (root == 0)
    return false;
  for (const HirValueId id : tree.valuesOf(statement)) {
    const HirValue *value = body.findValue(id);
    if (value == nullptr)
      continue;
    if (loanAccess == AccessMode::Mutable) {
      if (value->kind == HirValueKind::Variable && value->symbol == root)
        return true;
      continue;
    }
    if (value->callPlan) {
      if (value->callPlan->receiver &&
          value->callPlan->receiver->kind == HirCallInputKind::MutableBorrow &&
          placeRoot(body, value->callPlan->receiver->value) == root)
        return true;
      for (const auto &argument : value->callPlan->arguments)
        if ((argument.kind == HirCallInputKind::MutableBorrow ||
             argument.kind == HirCallInputKind::MoveValue) &&
            placeRoot(body, argument.value) == root)
          return true;
    }
    switch (value->kind) {
    case HirValueKind::Assignment:
    case HirValueKind::PlaceSet:
    case HirValueKind::MemberSet:
    case HirValueKind::IndexSet:
    case HirValueKind::DereferenceSet:
    case HirValueKind::Move:
      if (!value->operands.empty() &&
          placeRoot(body, value->operands.front()) == root)
        return true;
      break;
    default:
      break;
    }
  }
  return false;
}

} // namespace

MirDerivedLoanEnds deriveLoanEnds(const HirBody &body) {
  MirDerivedLoanEnds result;
  const StatementTree tree(body);

  std::unordered_map<SymbolId, HirBindingId> bindingOfSymbol;
  std::unordered_map<HirBindingId, HirStatementId> declaringStatement;
  for (const HirBinding &binding : body.bindings)
    if (binding.info.symbol != 0)
      bindingOfSymbol.emplace(binding.info.symbol, binding.id);
  for (const HirStatement &statement : body.statements)
    if (statement.binding)
      declaringStatement.emplace(*statement.binding, statement.id);

  // Statements that read each binding.
  std::unordered_map<HirBindingId, std::vector<HirStatementId>> readers;
  for (const HirValue &value : body.values) {
    if (value.symbol == 0)
      continue;
    const auto binding = bindingOfSymbol.find(value.symbol);
    const auto statement = tree.statementOfValue(value.id);
    if (binding != bindingOfSymbol.end() && statement)
      readers[binding->second].push_back(*statement);
  }

  // Each loan's own uses, then its dependents' uses (a parent outlives its
  // children).
  std::unordered_map<SemanticLoanId, std::vector<HirStatementId>> uses;
  std::unordered_map<SemanticLoanId, std::optional<HirStatementId>> created;
  std::unordered_set<SemanticLoanId> unconfined;
  for (const HirLoan &loan : body.loans) {
    auto &own = uses[loan.semanticLoan];
    std::optional<HirStatementId> creation;
    for (const HirBindingId carrier : loan.carriers) {
      if (const auto found = readers.find(carrier); found != readers.end())
        own.insert(own.end(), found->second.begin(), found->second.end());
      if (const auto declared = declaringStatement.find(carrier);
          declared != declaringStatement.end()) {
        own.push_back(declared->second);
        if (!creation ||
            tree.orderOf(declared->second) < tree.orderOf(*creation))
          creation = declared->second;
      }
    }
    created[loan.semanticLoan] = loan.entry ? std::nullopt : creation;
    // A mutable borrow of global storage and a switch payload binding's
    // retained selection keep a lexical extent: they end at the region exit.
    for (const HirBindingId carrier : loan.carriers) {
      const HirBinding *binding = nullptr;
      for (const HirBinding &candidate : body.bindings)
        if (candidate.id == carrier)
          binding = &candidate;
      if (binding != nullptr && binding->payloadPattern != nullptr &&
          loan.carriers.size() == 1)
        unconfined.insert(loan.semanticLoan);
      const auto declared = declaringStatement.find(carrier);
      if (loan.access == AccessMode::Mutable &&
          declared != declaringStatement.end())
        if (const HirStatement *statement =
                body.findStatement(declared->second);
            statement != nullptr && statement->value)
          if (const HirValue *initializer = body.findValue(*statement->value);
              initializer != nullptr &&
              initializer->borrowOrigin == BorrowOriginKind::Global)
            unconfined.insert(loan.semanticLoan);
    }
    // A carrier attached from another region (or with no declaration of its
    // own) makes the loan's extent region-wide: it ends at the region exit.
    if (!loan.entry && creation && loan.carriers.size() > 1)
      for (const HirBindingId carrier : loan.carriers) {
        const auto declared = declaringStatement.find(carrier);
        const bool payload =
            std::any_of(body.bindings.begin(), body.bindings.end(),
                        [&](const HirBinding &candidate) {
                          return candidate.id == carrier &&
                                 candidate.payloadPattern != nullptr;
                        });
        if (payload)
          continue;
        if (declared == declaringStatement.end() ||
            !(tree.regionOf(declared->second) == tree.regionOf(*creation)))
          unconfined.insert(loan.semanticLoan);
      }
  }
  // Cleanup is a use of every dependency a cleanup-bound carrier retains:
  // its ancestors cannot end at an ordinary last read beneath it.
  for (const HirLoan &loan : body.loans) {
    if (!loan.lifecycleBound)
      continue;
    std::vector<SemanticLoanId> pending{loan.parent, loan.storedDependency};
    for (const auto &dependency : loan.additionalDependencies)
      pending.push_back(dependency.parent);
    std::unordered_set<SemanticLoanId> visited;
    while (!pending.empty()) {
      const SemanticLoanId ancestor = pending.back();
      pending.pop_back();
      if (ancestor == 0 || !visited.insert(ancestor).second)
        continue;
      unconfined.insert(ancestor);
      if (const HirLoan *record = body.findLoan(ancestor)) {
        pending.push_back(record->parent);
        pending.push_back(record->storedDependency);
        for (const auto &dependency : record->additionalDependencies)
          pending.push_back(dependency.parent);
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const HirLoan &loan : body.loans) {
      std::vector<SemanticLoanId> parents{loan.parent, loan.storedDependency};
      for (const auto &dependency : loan.additionalDependencies)
        parents.push_back(dependency.parent);
      for (const SemanticLoanId parent : parents) {
        if (parent == 0 || parent == loan.semanticLoan ||
            !uses.contains(parent))
          continue;
        auto &target = uses[parent];
        for (const HirStatementId use : uses[loan.semanticLoan])
          if (std::find(target.begin(), target.end(), use) == target.end()) {
            target.push_back(use);
            changed = true;
          }
      }
    }
  }

  for (const HirLoan &loan : body.loans) {
    // An entry loan is the caller's: it outlives the whole body.
    if (loan.entry)
      continue;
    // A cleanup-bound carrier keeps its loan until the carrier's cleanup;
    // several exclusive aliases share state transitions a single last-use
    // end point cannot express. Both end at their region's exit.
    if (loan.lifecycleBound || unconfined.contains(loan.semanticLoan) ||
        (loan.carriers.size() > 1 && loan.access != AccessMode::ReadOnly))
      continue;
    const auto &own = uses[loan.semanticLoan];
    if (own.empty())
      continue;
    const HirStatementId last = *std::max_element(
        own.begin(), own.end(), [&](HirStatementId left, HirStatementId right) {
          return tree.orderOf(left) < tree.orderOf(right);
        });
    const RegionKey region = created[loan.semanticLoan]
                                 ? tree.regionOf(*created[loan.semanticLoan])
                                 : RegionKey{0, 0};
    HirStatementId end = last;
    while (end != 0 && !(tree.regionOf(end) == region))
      end = tree.parentOf(end);
    if (end == 0)
      continue;
    // A loan still live when the body completes is retired by the body's
    // exit (or escapes through a return), not after a statement. Nested
    // blocks retire it after their last statement like any other.
    const HirStatement *ending = body.findStatement(end);
    if (ending == nullptr || tree.lastInList(end) ||
        ending->kind == HirStatementKind::Return ||
        ending->kind == HirStatementKind::Break ||
        ending->kind == HirStatementKind::Continue) {
      result.regionExit[loan.semanticLoan] = end;
      continue;
    }
    result.after[end].push_back(loan.semanticLoan);

    // A loop body that ends `...; conflict; break;` never reaches its
    // backedge after the conflict, so the loan also ends after its last use
    // before that conflict on this path.
    const bool loop = ending->kind == HirStatementKind::While ||
                      ending->kind == HirStatementKind::For ||
                      ending->kind == HirStatementKind::DoWhile;
    const HirStatement *loopBody =
        loop && ending->body ? body.findStatement(*ending->body) : nullptr;
    if (loopBody == nullptr || loopBody->kind != HirStatementKind::Block)
      continue;
    const auto &list = loopBody->statements;
    for (std::size_t index = 0; index + 1 < list.size(); ++index) {
      const HirStatement *next = body.findStatement(list[index + 1]);
      if (next == nullptr || next->kind != HirStatementKind::Break ||
          !conflictsWithLoan(body, tree, list[index], loan.place.root,
                             loan.access))
        continue;
      std::optional<std::size_t> lastBefore;
      bool usedAtOrAfterConflict = false;
      for (const HirStatementId use : own) {
        HirStatementId cursor = use;
        while (cursor != 0 && tree.parentOf(cursor) != loopBody->id)
          cursor = tree.parentOf(cursor);
        const auto position = std::find(list.begin(), list.end(), cursor);
        if (position == list.end())
          continue;
        const auto at = static_cast<std::size_t>(position - list.begin());
        if (at >= index)
          usedAtOrAfterConflict = true;
        else if (!lastBefore || at > *lastBefore)
          lastBefore = at;
      }
      if (usedAtOrAfterConflict || !lastBefore)
        break;
      const HirStatement *last = body.findStatement(list[*lastBefore]);
      if (last != nullptr && (last->kind == HirStatementKind::Variable ||
                              last->kind == HirStatementKind::Expression))
        result.after[list[*lastBefore]].push_back(loan.semanticLoan);
      break;
    }
  }
  // A consumed switch subject hands its borrowed dependency to the arm's
  // transferred payload references: that dependency ends at the arm's entry.
  for (const HirStatement &statement : body.statements) {
    if (statement.kind != HirStatementKind::Switch || !statement.value)
      continue;
    const HirValue *subject = body.findValue(*statement.value);
    if (subject == nullptr || subject->kind != HirValueKind::Move)
      continue;
    const SymbolId root = placeRoot(body, *statement.value);
    const HirBinding *owner = nullptr;
    for (const HirBinding &binding : body.bindings)
      if (root != 0 && binding.info.symbol == root)
        owner = &binding;
    if (owner == nullptr || owner->info.retainedLoan == 0)
      continue;
    SemanticLoanId dependency = owner->info.retainedLoan;
    if (const HirLoan *loan = body.findLoan(dependency);
        loan != nullptr && loan->kind == SemanticLoanKind::Reference)
      dependency = loan->storedDependency;
    if (dependency == 0)
      continue;
    for (std::size_t arm = 0; arm < statement.switchArms.size(); ++arm) {
      bool transfers = false;
      bool valid = true;
      for (const auto &payload : statement.switchArms[arm].payloadBindings) {
        const HirValue *extraction = body.findValue(payload.value);
        const HirBinding *binding = nullptr;
        for (const HirBinding &candidate : body.bindings)
          if (candidate.id == payload.binding)
            binding = &candidate;
        const bool transfer = extraction != nullptr &&
                              extraction->payloadExtractionKind ==
                                  PayloadExtractionKind::TransferReference;
        if (transfer)
          transfers = true;
        else if (binding != nullptr &&
                 binding->info.traits.containsBorrowedState)
          valid = false;
      }
      (void)transfers;
      if (valid)
        result.switchArmEntry[{statement.id, arm}].push_back(dependency);
    }
  }
  return result;
}

MirLoanEndAgreement compareLoanEnds(const HirBody &body,
                                    const MirDerivedLoanEnds &derived,
                                    bool report) {
  MirLoanEndAgreement agreement;
  const auto contains = [](const std::vector<SemanticLoanId> &loans,
                           SemanticLoanId loan) {
    return std::find(loans.begin(), loans.end(), loan) != loans.end();
  };
  // Every end point as "kind:statement[:arm]".
  using Points = std::vector<std::string>;
  for (const HirLoan &loan : body.loans) {
    ++agreement.loans;
    const SemanticLoanId id = loan.semanticLoan;
    Points semantic;
    Points mir;
    for (const HirStatement &statement : body.statements) {
      const std::string at = std::to_string(statement.id);
      if (contains(statement.endedLoans, id))
        semantic.push_back("after:" + at);
      if (contains(statement.thenEntryEndedLoans, id))
        semantic.push_back("then:" + at);
      if (contains(statement.elseEntryEndedLoans, id))
        semantic.push_back("else:" + at);
      for (std::size_t arm = 0; arm < statement.switchArms.size(); ++arm)
        if (contains(statement.switchArms[arm].entryEndedLoans, id))
          semantic.push_back("arm:" + at + ":" + std::to_string(arm));
    }
    for (const auto &[statement, loans] : derived.after)
      if (contains(loans, id))
        mir.push_back("after:" + std::to_string(statement));
    for (const auto &[statement, loans] : derived.thenEntry)
      if (contains(loans, id))
        mir.push_back("then:" + std::to_string(statement));
    for (const auto &[statement, loans] : derived.elseEntry)
      if (contains(loans, id))
        mir.push_back("else:" + std::to_string(statement));
    for (const auto &[key, loans] : derived.switchArmEntry)
      if (contains(loans, id))
        mir.push_back("arm:" + std::to_string(key.first) + ":" +
                      std::to_string(key.second));
    std::sort(semantic.begin(), semantic.end());
    std::sort(mir.begin(), mir.end());
    if (semantic.size() > 1)
      ++agreement.semanticEarlyPlan;
    if (semantic == mir) {
      ++agreement.agree;
      continue;
    }
    ++agreement.differ;
    if (report) {
      std::cerr << "loan-end-differ semantic=" << id
                << " kind=" << static_cast<int>(loan.kind)
                << " entry=" << loan.entry
                << " carriers=" << loan.carriers.size()
                << " parent=" << loan.parent << " semantic=[";
      for (const auto &point : semantic)
        std::cerr << point << ' ';
      std::cerr << "] derived=[";
      for (const auto &point : mir)
        std::cerr << point << ' ';
      std::cerr << "] names=";
      for (const auto carrier : loan.carriers)
        for (const HirBinding &binding : body.bindings)
          if (binding.id == carrier)
            std::cerr << (binding.variable ? binding.variable->name().lexeme
                                           : std::string("<anon>"))
                      << (binding.info.traits.containsBorrowedState
                              ? "(borrowed)"
                              : "")
                      << ',';
      std::cerr << " lifecycle=" << loan.lifecycleBound
                << " stored=" << loan.storedDependency
                << " fields=" << loan.designatedFields.size() << "\n";
    }
  }
  return agreement;
}

} // namespace lang
