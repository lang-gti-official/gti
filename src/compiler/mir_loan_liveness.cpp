#include "mir_loan_liveness.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

namespace lang {
namespace {

// HirBody's lookups scan linearly; liveness looks up every value and
// statement of a body, so index the body once instead (keyed by its storage,
// which is fixed while one body is analysed).
struct BodyIndex {
  const HirValue *valuesData = nullptr;
  std::size_t valueCount = 0;
  const HirStatement *statementsData = nullptr;
  std::size_t statementCount = 0;
  std::unordered_map<HirValueId, const HirValue *> values;
  std::unordered_map<HirStatementId, const HirStatement *> statements;
};

const BodyIndex &indexOf(const HirBody &body) {
  thread_local BodyIndex index;
  if (index.valuesData != body.values.data() ||
      index.valueCount != body.values.size() ||
      index.statementsData != body.statements.data() ||
      index.statementCount != body.statements.size()) {
    index = BodyIndex{.valuesData = body.values.data(),
                      .valueCount = body.values.size(),
                      .statementsData = body.statements.data(),
                      .statementCount = body.statements.size()};
    index.values.reserve(body.values.size());
    for (const HirValue &value : body.values)
      index.values.emplace(value.id, &value);
    index.statements.reserve(body.statements.size());
    for (const HirStatement &statement : body.statements)
      index.statements.emplace(statement.id, &statement);
  }
  return index;
}

const HirValue *indexedValue(const HirBody &body, HirValueId id) {
  const auto &values = indexOf(body).values;
  const auto found = values.find(id);
  return found == values.end() ? nullptr : found->second;
}

const HirStatement *indexedStatement(const HirBody &body, HirStatementId id) {
  const auto &statements = indexOf(body).statements;
  const auto found = statements.find(id);
  return found == statements.end() ? nullptr : found->second;
}

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
    // A result region no statement reaches, such as one in a constructor
    // initializer, is its own root list: its loans are confined to it. A
    // value follows the values it contains, so visiting the latest value
    // first reaches each enclosing region before the regions nested in it.
    std::size_t list = 2000;
    for (auto value = body.values.rbegin(); value != body.values.rend();
         ++value)
      if (value->regionBody && !order.contains(*value->regionBody)) {
        visit(*value->regionBody, 0, {0, ++list}, next);
        lastInRegion.insert(*value->regionBody);
      }
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
    const HirStatement *statement = indexedStatement(body, id);
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
    const HirValue *value = indexedValue(body, id);
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
    const HirValue *value = indexedValue(body, id);
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
    const HirValue *value = indexedValue(body, id);
    if (value == nullptr)
      continue;
    // An assignment names its target variable by symbol.
    if (value->kind == HirValueKind::Assignment && value->symbol == root)
      return true;
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

MirDerivedLoanEnds
deriveLoanEnds(const HirBody &body, const std::vector<LoanFacts> &loans,
               const std::function<std::size_t(HirBindingId)> &retainedLoanOf) {
  MirDerivedLoanEnds result;
  std::unordered_map<std::size_t, const LoanFacts *> factsById;
  for (const LoanFacts &loan : loans)
    factsById.emplace(loan.id, &loan);
  const auto findFacts = [&](std::size_t id) -> const LoanFacts * {
    const auto found = factsById.find(id);
    return found == factsById.end() ? nullptr : found->second;
  };
  std::function<bool(const HirStatement &, const LoanFacts &,
                     const std::vector<HirStatementId> &, MirDerivedLoanEnds &)>
      conditionalPlan;
  std::function<bool(const HirStatement &, const LoanFacts &,
                     const std::vector<HirStatementId> &, MirDerivedLoanEnds &)>
      switchPlan;
  std::function<void(const HirStatement &, const LoanFacts &,
                     const std::vector<HirStatementId> &, MirDerivedLoanEnds &)>
      breakPlans;
  const StatementTree tree(body);

  std::unordered_map<SymbolId, HirBindingId> bindingOfSymbol;
  std::unordered_map<HirBindingId, HirStatementId> declaringStatement;
  for (const HirBinding &binding : body.bindings)
    if (binding.info.symbol != 0)
      bindingOfSymbol.emplace(binding.info.symbol, binding.id);
  for (const HirStatement &statement : body.statements)
    if (statement.binding)
      declaringStatement.emplace(*statement.binding, statement.id);
  // A payload binding is declared by its switch: its loan is created in the
  // switch's region, not in the arm.
  for (const HirStatement &statement : body.statements)
    for (const HirSwitchArm &arm : statement.switchArms)
      for (const auto &payload : arm.payloadBindings)
        declaringStatement.emplace(payload.binding, statement.id);

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

  // Type-based region flow: a binding that carries no loan of its own, whose
  // type can hold a borrow and whose initializer reads another binding, may
  // hold what that binding borrows, so its reads are uses of that binding's
  // loans too (a closure capturing a view, a copy of that closure, the view
  // its call returns).
  const auto canHoldBorrow = [](const HirBinding &binding) {
    const SemanticType &type = binding.info.type;
    return type.kind == SemanticType::Reference ||
           type.kind == SemanticType::StringView ||
           type.kind == SemanticType::CString ||
           type.kind == SemanticType::Lambda ||
           binding.info.traits.containsBorrowedState;
  };
  // A binding that carries a loan of its own already has MIR's exact
  // dependencies; type-based flow only covers bindings MIR left unloaned.
  std::unordered_set<HirBindingId> loaned;
  for (const LoanFacts &loan : loans)
    loaned.insert(loan.carriers.begin(), loan.carriers.end());
  std::unordered_map<HirBindingId, std::vector<HirBindingId>> derivedFrom;
  for (const HirStatement &statement : body.statements) {
    if (!statement.binding || !statement.value)
      continue;
    const HirBinding *derived = nullptr;
    for (const HirBinding &candidate : body.bindings)
      if (candidate.id == *statement.binding)
        derived = &candidate;
    if (derived == nullptr || !canHoldBorrow(*derived) ||
        loaned.contains(derived->id))
      continue;
    std::vector<HirValueId> pending{*statement.value};
    std::unordered_set<HirValueId> visited;
    while (!pending.empty()) {
      const HirValueId id = pending.back();
      pending.pop_back();
      if (!visited.insert(id).second)
        continue;
      const HirValue *value = indexedValue(body, id);
      if (value == nullptr)
        continue;
      if (value->symbol != 0)
        if (const auto source = bindingOfSymbol.find(value->symbol);
            source != bindingOfSymbol.end() && source->second != derived->id)
          derivedFrom[source->second].push_back(derived->id);
      pending.insert(pending.end(), value->operands.begin(),
                     value->operands.end());
      if (value->receiver)
        pending.push_back(*value->receiver);
    }
  }
  // A loan on a binding's own storage (a reference to it, a callable
  // wrapper borrowing it) makes its carriers read that binding when used.
  for (const LoanFacts &loan : loans) {
    if (loan.placeRoot == 0)
      continue;
    const auto borrowed = bindingOfSymbol.find(loan.placeRoot);
    if (borrowed == bindingOfSymbol.end())
      continue;
    for (const HirBindingId carrier : loan.carriers)
      if (carrier != borrowed->second)
        derivedFrom[borrowed->second].push_back(carrier);
  }
  const auto addDerivedReads = [&](HirBindingId carrier,
                                   std::vector<HirStatementId> &into) {
    std::vector<HirBindingId> pending{carrier};
    std::unordered_set<HirBindingId> seen{carrier};
    while (!pending.empty()) {
      const HirBindingId current = pending.back();
      pending.pop_back();
      const auto derived = derivedFrom.find(current);
      if (derived == derivedFrom.end())
        continue;
      for (const HirBindingId binding : derived->second) {
        if (!seen.insert(binding).second)
          continue;
        if (const auto found = readers.find(binding); found != readers.end())
          into.insert(into.end(), found->second.begin(), found->second.end());
        pending.push_back(binding);
      }
    }
  };

  // Each loan's own uses, then its dependents' uses (a parent outlives its
  // children).
  std::unordered_map<MirLoanId, std::vector<HirStatementId>> uses;
  std::unordered_map<MirLoanId, std::optional<HirStatementId>> created;
  std::unordered_set<MirLoanId> unconfined;
  for (const LoanFacts &loan : loans) {
    auto &own = uses[loan.id];
    std::optional<HirStatementId> creation;
    for (const HirBindingId carrier : loan.carriers) {
      if (const auto found = readers.find(carrier); found != readers.end())
        own.insert(own.end(), found->second.begin(), found->second.end());
      addDerivedReads(carrier, own);
      if (const auto declared = declaringStatement.find(carrier);
          declared != declaringStatement.end()) {
        own.push_back(declared->second);
        if (!creation ||
            tree.orderOf(declared->second) < tree.orderOf(*creation))
          creation = declared->second;
      }
    }
    created[loan.id] = loan.entry ? std::nullopt : creation;
    // A mutable borrow of global storage and a switch payload binding's
    // retained selection keep a lexical extent: they end at the region exit.
    for (const HirBindingId carrier : loan.carriers) {
      const HirBinding *binding = nullptr;
      for (const HirBinding &candidate : body.bindings)
        if (candidate.id == carrier)
          binding = &candidate;
      if (binding != nullptr && binding->payloadPattern != nullptr &&
          loan.carriers.size() == 1)
        unconfined.insert(loan.id);
      // A carrier with no source symbol is invisible to last-use tracking.
      if (binding != nullptr && binding->info.symbol == 0)
        unconfined.insert(loan.id);
      const auto declared = declaringStatement.find(carrier);
      if (loan.access == AccessMode::Mutable &&
          declared != declaringStatement.end())
        if (const HirStatement *statement =
                indexedStatement(body, declared->second);
            statement != nullptr && statement->value)
          if (const HirValue *initializer =
                  indexedValue(body, *statement->value);
              initializer != nullptr &&
              initializer->borrowOrigin == BorrowOriginKind::Global)
            unconfined.insert(loan.id);
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
        // A carrier initialized by moving another carrier takes the loan over;
        // a transfer does not widen the loan's extent.
        if (declared != declaringStatement.end())
          if (const HirStatement *statement =
                  indexedStatement(body, declared->second);
              statement != nullptr && statement->value)
            if (const HirValue *initializer =
                    indexedValue(body, *statement->value);
                initializer != nullptr &&
                initializer->kind == HirValueKind::Move)
              continue;
        if (declared == declaringStatement.end() ||
            !(tree.regionOf(declared->second) == tree.regionOf(*creation)))
          unconfined.insert(loan.id);
      }
  }
  // Cleanup is a use of every dependency a cleanup-bound carrier retains:
  // its ancestors cannot end at an ordinary last read beneath it.
  for (const LoanFacts &loan : loans) {
    if (!loan.lifecycleBound)
      continue;
    std::vector<MirLoanId> pending{loan.parent, loan.storedDependency};
    for (const auto dependencyParent : loan.dependencyParents)
      pending.push_back(dependencyParent);
    std::unordered_set<MirLoanId> visited;
    while (!pending.empty()) {
      const MirLoanId ancestor = pending.back();
      pending.pop_back();
      if (ancestor == 0 || !visited.insert(ancestor).second)
        continue;
      unconfined.insert(ancestor);
      if (const LoanFacts *record = findFacts(ancestor)) {
        pending.push_back(record->parent);
        pending.push_back(record->storedDependency);
        for (const auto dependencyParent : record->dependencyParents)
          pending.push_back(dependencyParent);
      }
    }
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (const LoanFacts &loan : loans) {
      std::vector<MirLoanId> parents{loan.parent, loan.storedDependency};
      for (const auto dependencyParent : loan.dependencyParents)
        parents.push_back(dependencyParent);
      for (const MirLoanId parent : parents) {
        if (parent == 0 || parent == loan.id || !uses.contains(parent))
          continue;
        auto &target = uses[parent];
        for (const HirStatementId use : uses[loan.id])
          if (std::find(target.begin(), target.end(), use) == target.end()) {
            target.push_back(use);
            changed = true;
          }
      }
    }
  }

  const auto within = [&](HirStatementId statement, HirStatementId root) {
    for (HirStatementId cursor = statement; cursor != 0;
         cursor = tree.parentOf(cursor))
      if (cursor == root)
        return true;
    return false;
  };
  std::function<bool(HirStatementId)> cannotFallThrough =
      [&](HirStatementId id) -> bool {
    const HirStatement *statement = indexedStatement(body, id);
    if (statement == nullptr)
      return false;
    switch (statement->kind) {
    case HirStatementKind::Return:
    case HirStatementKind::Break:
    case HirStatementKind::Continue:
      return true;
    case HirStatementKind::Block:
      return !statement->statements.empty() &&
             cannotFallThrough(statement->statements.back());
    case HirStatementKind::If:
      return statement->body && statement->elseBranch &&
             cannotFallThrough(*statement->body) &&
             cannotFallThrough(*statement->elseBranch);
    default:
      return false;
    }
  };
  struct Point {
    int kind; // 0 after, 1 then entry, 2 else entry
    HirStatementId statement;
  };
  // Every statement in a subtree, the root included.
  std::function<void(HirStatementId, std::vector<HirStatementId> &)> subtree =
      [&](HirStatementId id, std::vector<HirStatementId> &out) {
        const HirStatement *statement = indexedStatement(body, id);
        if (statement == nullptr)
          return;
        out.push_back(id);
        for (const auto nested :
             {statement->initializer, statement->body, statement->elseBranch})
          if (nested)
            subtree(*nested, out);
        for (const HirStatementId nested : statement->statements)
          subtree(nested, out);
        for (const HirSwitchArm &arm : statement->switchArms)
          for (const HirStatementId nested : arm.statements)
            subtree(nested, out);
      };
  const auto conflictsIn = [&](HirStatementId root, const LoanFacts &loan) {
    std::vector<HirStatementId> statements;
    subtree(root, statements);
    std::vector<HirStatementId> conflicts;
    for (const HirStatementId statement : statements)
      if (conflictsWithLoan(body, tree, statement, loan.placeRoot, loan.access))
        conflicts.push_back(statement);
    return conflicts;
  };
  // The if statements whose nearest enclosing if-arm is `arm`, in order.
  const auto nestedConditionals = [&](HirStatementId arm) {
    std::vector<HirStatementId> statements;
    subtree(arm, statements);
    std::vector<HirStatementId> nested;
    for (const HirStatementId statement : statements) {
      const HirStatement *candidate = indexedStatement(body, statement);
      // An `else if` arm is itself the nested conditional.
      if (statement == arm) {
        if (candidate != nullptr && candidate->kind == HirStatementKind::If)
          nested.push_back(statement);
        continue;
      }
      if (candidate == nullptr || candidate->kind != HirStatementKind::If)
        continue;
      bool direct = true;
      for (HirStatementId cursor = tree.parentOf(statement);
           cursor != 0 && cursor != arm; cursor = tree.parentOf(cursor)) {
        const HirStatement *ancestor = indexedStatement(body, cursor);
        if (ancestor != nullptr && ancestor->kind == HirStatementKind::If)
          direct = false;
      }
      if (direct)
        nested.push_back(statement);
    }
    std::sort(nested.begin(), nested.end(),
              [&](HirStatementId left, HirStatementId right) {
                return tree.orderOf(left) < tree.orderOf(right);
              });
    return nested;
  };
  // Loops and switches inside an arm need a general CFG proof.
  const auto unsupportedFlow = [&](HirStatementId arm) {
    std::vector<HirStatementId> statements;
    subtree(arm, statements);
    for (const HirStatementId statement : statements) {
      const HirStatement *candidate = indexedStatement(body, statement);
      if (candidate == nullptr)
        continue;
      switch (candidate->kind) {
      case HirStatementKind::While:
      case HirStatementKind::For:
      case HirStatementKind::DoWhile:
      case HirStatementKind::RangeFor:
      case HirStatementKind::Switch:
        return true;
      default:
        break;
      }
    }
    return false;
  };
  std::function<bool(const HirStatement &, bool, const LoanFacts &,
                     const std::vector<HirStatementId> &, std::vector<Point> &)>
      armEndpoints;
  std::function<bool(const HirStatement &, bool, const LoanFacts &,
                     const std::vector<HirStatementId> &, std::vector<Point> &)>
      terminatingEndpoints = [&](const HirStatement &conditional,
                                 bool thenBranch, const LoanFacts &loan,
                                 const std::vector<HirStatementId> &own,
                                 std::vector<Point> &points) -> bool {
    const std::optional<HirStatementId> arm =
        thenBranch ? conditional.body : conditional.elseBranch;
    if (!arm)
      return true;
    if (unsupportedFlow(*arm))
      return false;
    if (cannotFallThrough(*arm)) {
      if (conflictsIn(*arm, loan).empty())
        return true;
      return armEndpoints(conditional, thenBranch, loan, own, points);
    }
    for (const HirStatementId nested : nestedConditionals(*arm)) {
      const HirStatement *inner = indexedStatement(body, nested);
      if (inner == nullptr ||
          !terminatingEndpoints(*inner, true, loan, own, points) ||
          !terminatingEndpoints(*inner, false, loan, own, points))
        return false;
    }
    return true;
  };
  armEndpoints = [&](const HirStatement &conditional, bool thenBranch,
                     const LoanFacts &loan,
                     const std::vector<HirStatementId> &own,
                     std::vector<Point> &points) -> bool {
    const std::optional<HirStatementId> arm =
        thenBranch ? conditional.body : conditional.elseBranch;
    if (arm && unsupportedFlow(*arm))
      return false;
    if (arm && cannotFallThrough(*arm) && conflictsIn(*arm, loan).empty())
      return true;
    std::optional<HirStatementId> lastUse;
    if (arm)
      for (const HirStatementId use : own)
        if (within(use, *arm) &&
            (!lastUse || tree.orderOf(use) > tree.orderOf(*lastUse)))
          lastUse = use;
    if (!lastUse) {
      points.push_back({thenBranch ? 1 : 2, conditional.id});
      return true;
    }
    const auto nested = nestedConditionals(*arm);
    std::optional<HirStatementId> lastUseConditional;
    for (const HirStatementId candidate : nested)
      if (within(*lastUse, candidate)) {
        lastUseConditional = candidate;
        break;
      }
    for (const HirStatementId candidate : nested) {
      if (lastUseConditional
              ? candidate == *lastUseConditional
              : tree.orderOf(candidate) >= tree.orderOf(*lastUse))
        break;
      const HirStatement *inner = indexedStatement(body, candidate);
      if (inner == nullptr ||
          !terminatingEndpoints(*inner, true, loan, own, points) ||
          !terminatingEndpoints(*inner, false, loan, own, points))
        return false;
    }
    if (!lastUseConditional) {
      const HirStatement *last = indexedStatement(body, *lastUse);
      if (last == nullptr || (last->kind != HirStatementKind::Variable &&
                              last->kind != HirStatementKind::Expression))
        return false;
      points.push_back({0, *lastUse});
      return true;
    }
    if (conflictsIn(*lastUseConditional, loan).empty()) {
      points.push_back({0, *lastUseConditional});
      return true;
    }
    const HirStatement *inner = indexedStatement(body, *lastUseConditional);
    return inner != nullptr && armEndpoints(*inner, true, loan, own, points) &&
           armEndpoints(*inner, false, loan, own, points);
  };
  // The innermost if-arm statement containing `statement`, if any, below
  // `top` (inclusive).
  const auto innermostArm = [&](HirStatementId statement) -> HirStatementId {
    for (HirStatementId cursor = statement; cursor != 0;) {
      const HirStatementId parent = tree.parentOf(cursor);
      const HirStatement *owner = indexedStatement(body, parent);
      if (owner != nullptr && owner->kind == HirStatementKind::If &&
          ((owner->body && *owner->body == cursor) ||
           (owner->elseBranch && *owner->elseBranch == cursor)))
        return cursor;
      cursor = parent;
    }
    return 0;
  };
  // The plan applies when one of its end points precedes a conflict on the
  // same path, or when the if cannot fall through.
  conditionalPlan = [&](const HirStatement &conditional, const LoanFacts &loan,
                        const std::vector<HirStatementId> &own,
                        MirDerivedLoanEnds &ends) -> bool {
    std::vector<Point> points;
    if (!armEndpoints(conditional, true, loan, own, points) ||
        !armEndpoints(conditional, false, loan, own, points))
      return false;
    bool resolved = false;
    for (const HirStatementId conflict : conflictsIn(conditional.id, loan)) {
      for (const Point &point : points) {
        if (point.kind != 0) {
          const auto arm =
              point.kind == 1 ? conditional.body : conditional.elseBranch;
          const HirStatement *owner = indexedStatement(body, point.statement);
          const auto entryArm =
              owner == nullptr
                  ? std::nullopt
                  : (point.kind == 1 ? owner->body : owner->elseBranch);
          (void)arm;
          if (entryArm && within(conflict, *entryArm))
            resolved = true;
          continue;
        }
        const HirStatementId scope = innermostArm(point.statement);
        if (point.statement != conflict &&
            tree.orderOf(point.statement) < tree.orderOf(conflict) &&
            (scope == 0 || within(conflict, scope)))
          resolved = true;
      }
    }
    if (!resolved && !cannotFallThrough(conditional.id))
      return false;
    for (const Point &point : points) {
      auto &target = point.kind == 0   ? ends.after[point.statement]
                     : point.kind == 1 ? ends.thenEntry[point.statement]
                                       : ends.elseEntry[point.statement];
      target.push_back(loan.id);
    }
    return true;
  };
  // A loan whose only use in a switch is its subject ends at every arm's
  // entry, and after the switch when no default arm exists.
  switchPlan = [&](const HirStatement &selection, const LoanFacts &loan,
                   const std::vector<HirStatementId> &own,
                   MirDerivedLoanEnds &ends) -> bool {
    for (const HirStatementId use : own)
      if (use != selection.id && within(use, selection.id))
        return false;
    // Binding a payload uses the subject's authority at that arm's entry.
    for (const HirSwitchArm &arm : selection.switchArms)
      if (!arm.payloadBindings.empty())
        return false;
    bool hasDefault = false;
    for (const HirSwitchArm &arm : selection.switchArms)
      for (const auto &label : arm.labels)
        if (label.isDefault)
          hasDefault = true;
    for (std::size_t arm = 0; arm < selection.switchArms.size(); ++arm)
      ends.switchArmEntry[{selection.id, arm}].push_back(loan.id);
    if (!hasDefault)
      ends.after[selection.id].push_back(loan.id);
    return true;
  };

  struct ArmStep {
    HirStatementId statement; // the if or switch
    std::size_t arm;          // 0/1 for then/else, or the switch arm
    bool operator==(const ArmStep &other) const {
      return statement == other.statement && arm == other.arm;
    }
  };
  // The structured arms between `top` (exclusive) and `statement`, outermost
  // first.
  const auto armPath = [&](HirStatementId statement, HirStatementId top) {
    std::vector<ArmStep> path;
    for (HirStatementId cursor = statement; cursor != 0 && cursor != top;) {
      const HirStatementId parent = tree.parentOf(cursor);
      if (const HirStatement *owner = indexedStatement(body, parent)) {
        if (owner->kind == HirStatementKind::If) {
          if (owner->body && *owner->body == cursor)
            path.push_back({parent, 0});
          else if (owner->elseBranch && *owner->elseBranch == cursor)
            path.push_back({parent, 1});
        } else if (owner->kind == HirStatementKind::Switch) {
          for (std::size_t arm = 0; arm < owner->switchArms.size(); ++arm) {
            const auto &list = owner->switchArms[arm].statements;
            if (std::find(list.begin(), list.end(), cursor) != list.end())
              path.push_back({parent, arm});
          }
        }
      }
      cursor = parent;
    }
    std::reverse(path.begin(), path.end());
    return path;
  };
  const auto isPrefix = [](const std::vector<ArmStep> &prefix,
                           const std::vector<ArmStep> &path) {
    return prefix.size() <= path.size() &&
           std::equal(prefix.begin(), prefix.end(), path.begin());
  };
  // The statement list that directly holds `statement`.
  const auto siblingsOf = [&](HirStatementId statement) {
    const HirStatementId parent = tree.parentOf(statement);
    if (parent == 0)
      return body.roots;
    const HirStatement *owner = indexedStatement(body, parent);
    if (owner == nullptr)
      return std::vector<HirStatementId>{};
    for (const HirSwitchArm &arm : owner->switchArms)
      if (std::find(arm.statements.begin(), arm.statements.end(), statement) !=
          arm.statements.end())
        return arm.statements;
    return owner->statements;
  };
  // The loop or switch a break leaves.
  const auto breakTarget = [&](HirStatementId statement) -> HirStatementId {
    for (HirStatementId cursor = tree.parentOf(statement); cursor != 0;
         cursor = tree.parentOf(cursor))
      if (const HirStatement *owner = indexedStatement(body, cursor))
        switch (owner->kind) {
        case HirStatementKind::While:
        case HirStatementKind::For:
        case HirStatementKind::DoWhile:
        case HirStatementKind::RangeFor:
        case HirStatementKind::Switch:
          return cursor;
        default:
          break;
        }
    return 0;
  };
  breakPlans = [&](const HirStatement &target, const LoanFacts &loan,
                   const std::vector<HirStatementId> &own,
                   MirDerivedLoanEnds &ends) {
    std::vector<HirStatementId> statements;
    subtree(target.id, statements);
    for (const HirStatementId conflict : statements) {
      if (conflict == target.id ||
          !conflictsWithLoan(body, tree, conflict, loan.placeRoot, loan.access))
        continue;
      const auto list = siblingsOf(conflict);
      const auto at = std::find(list.begin(), list.end(), conflict);
      if (at == list.end() || std::next(at) == list.end())
        continue;
      const HirStatement *next = indexedStatement(body, *std::next(at));
      if (next == nullptr || next->kind != HirStatementKind::Break ||
          breakTarget(next->id) != target.id)
        continue;
      const auto conflictPath = armPath(conflict, target.id);
      std::optional<HirStatementId> lastUse;
      bool valid = true;
      for (const HirStatementId use : own) {
        if (!within(use, target.id) && use != target.id)
          continue;
        if (tree.orderOf(use) >= tree.orderOf(next->id))
          continue;
        const auto usePath = armPath(use, target.id);
        if (!isPrefix(usePath, conflictPath) &&
            !isPrefix(conflictPath, usePath))
          continue; // sibling paths never meet
        if (usePath != conflictPath ||
            tree.orderOf(use) >= tree.orderOf(conflict)) {
          valid = false;
          break;
        }
        if (!lastUse || tree.orderOf(use) > tree.orderOf(*lastUse))
          lastUse = use;
      }
      if (!valid)
        continue;
      if (lastUse) {
        const HirStatement *last = indexedStatement(body, *lastUse);
        if (last == nullptr ||
            std::find(list.begin(), list.end(), *lastUse) == list.end() ||
            (last->kind != HirStatementKind::Variable &&
             last->kind != HirStatementKind::Expression))
          continue;
        ends.after[*lastUse].push_back(loan.id);
        continue;
      }
      if (conflictPath.empty())
        continue;
      // The innermost conditional arm, else the innermost switch arm.
      std::optional<ArmStep> entry;
      for (auto step = conflictPath.rbegin(); step != conflictPath.rend();
           ++step)
        if (const HirStatement *owner = indexedStatement(body, step->statement);
            owner != nullptr && owner->kind == HirStatementKind::If) {
          entry = *step;
          break;
        }
      if (!entry)
        entry = conflictPath.back();
      const HirStatement *owner = indexedStatement(body, entry->statement);
      if (owner == nullptr)
        continue;
      if (owner->kind == HirStatementKind::If)
        (entry->arm == 0 ? ends.thenEntry : ends.elseEntry)[owner->id]
            .push_back(loan.id);
      else
        ends.switchArmEntry[{owner->id, entry->arm}].push_back(loan.id);
    }
  };

  for (const LoanFacts &loan : loans) {
    // An entry loan is the caller's: it outlives the whole body.
    if (loan.entry)
      continue;
    // A cleanup-bound carrier keeps its loan until the carrier's cleanup;
    // several exclusive aliases share state transitions a single last-use
    // end point cannot express. Both end at their region's exit.
    if (loan.lifecycleBound || unconfined.contains(loan.id) ||
        (loan.carriers.size() > 1 && loan.access != AccessMode::ReadOnly))
      continue;
    const auto &own = uses[loan.id];
    if (own.empty())
      continue;
    // A returning arm cannot reach uses after its enclosing conditional.
    // The ordinary last-use plan below only visits the final use's enclosing
    // statement, so it otherwise misses an earlier `if (...) { ...; return; }`.
    // Reuse the existing arm proof, but only for function-exiting arms: a
    // break/continue can still reach uses outside that arm and needs its own
    // target-aware plan.
    if (created[loan.id]) {
      const auto creationRegion = tree.regionOf(*created[loan.id]);
      for (const auto &conditional : body.statements) {
        if (conditional.kind != HirStatementKind::If ||
            !(tree.regionOf(conditional.id) == creationRegion) ||
            tree.orderOf(conditional.id) <= tree.orderOf(*created[loan.id]))
          continue;
        for (const bool thenBranch : {true, false}) {
          const auto arm =
              thenBranch ? conditional.body : conditional.elseBranch;
          if (!arm || !cannotFallThrough(*arm) || unsupportedFlow(*arm))
            continue;
          std::vector<HirStatementId> statements;
          subtree(*arm, statements);
          const bool otherExit = std::any_of(
              statements.begin(), statements.end(), [&](HirStatementId id) {
                const auto *statement = indexedStatement(body, id);
                return statement &&
                       (statement->kind == HirStatementKind::Break ||
                        statement->kind == HirStatementKind::Continue);
              });
          if (otherExit)
            continue;
          std::vector<Point> points;
          if (!terminatingEndpoints(conditional, thenBranch, loan, own, points))
            continue;
          for (const auto &point : points) {
            auto &target = point.kind == 0 ? result.after[point.statement]
                           : point.kind == 1
                               ? result.thenEntry[point.statement]
                               : result.elseEntry[point.statement];
            target.push_back(loan.id);
          }
        }
      }
    }
    const HirStatementId last = *std::max_element(
        own.begin(), own.end(), [&](HirStatementId left, HirStatementId right) {
          return tree.orderOf(left) < tree.orderOf(right);
        });
    const RegionKey region =
        created[loan.id] ? tree.regionOf(*created[loan.id]) : RegionKey{0, 0};
    HirStatementId end = last;
    while (end != 0 && !(tree.regionOf(end) == region))
      end = tree.parentOf(end);
    if (end == 0)
      continue;
    const HirStatement *ending = indexedStatement(body, end);
    if (ending != nullptr && ending->kind == HirStatementKind::If &&
        conditionalPlan(*ending, loan, own, result))
      continue;
    if (ending != nullptr && ending->kind == HirStatementKind::Switch &&
        switchPlan(*ending, loan, own, result))
      continue;
    // A loan still live when the body completes is retired by the body's
    // exit (or escapes through a return), not after a statement. Nested
    // blocks retire it after their last statement like any other.
    if (ending == nullptr ||
        (tree.lastInList(end) && ending->kind != HirStatementKind::Switch) ||
        ending->kind == HirStatementKind::Return ||
        ending->kind == HirStatementKind::Break ||
        ending->kind == HirStatementKind::Continue) {
      result.regionExit[loan.id] = end;
      continue;
    }
    result.after[end].push_back(loan.id);

    // A path that reaches `conflict; break;` out of this loop or switch never
    // returns to a later use, so the loan also ends on that path: after its
    // last use before the conflict, or at the entry of the arm that holds it.
    if (ending->kind != HirStatementKind::While &&
        ending->kind != HirStatementKind::For &&
        ending->kind != HirStatementKind::DoWhile &&
        ending->kind != HirStatementKind::Switch)
      continue;
    breakPlans(*ending, loan, own, result);
  }
  // A consumed switch subject hands its borrowed dependency to the arm's
  // transferred payload references: that dependency ends at the arm's entry.
  for (const HirStatement &statement : body.statements) {
    if (statement.kind != HirStatementKind::Switch || !statement.value)
      continue;
    const HirValue *subject = indexedValue(body, *statement.value);
    if (subject == nullptr || subject->kind != HirValueKind::Move)
      continue;
    const SymbolId root = placeRoot(body, *statement.value);
    const HirBinding *owner = nullptr;
    for (const HirBinding &binding : body.bindings)
      if (root != 0 && binding.info.symbol == root)
        owner = &binding;
    if (owner == nullptr || retainedLoanOf(owner->id) == 0)
      continue;
    std::size_t dependency = retainedLoanOf(owner->id);
    if (const LoanFacts *loan = findFacts(dependency);
        loan != nullptr && loan->reference)
      dependency = loan->storedDependency;
    if (dependency == 0)
      continue;
    for (std::size_t arm = 0; arm < statement.switchArms.size(); ++arm) {
      bool transfers = false;
      bool valid = true;
      for (const auto &payload : statement.switchArms[arm].payloadBindings) {
        const HirValue *extraction = indexedValue(body, payload.value);
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
  // A terminating-arm plan and the ordinary final-use plan can establish
  // the same endpoint. Emit it once, retaining dependency ordering.
  const auto uniqueEnds = [](auto &points) {
    for (auto &[point, ids] : points) {
      (void)point;
      std::unordered_set<MirLoanId> seen;
      ids.erase(
          std::remove_if(ids.begin(), ids.end(),
                         [&](MirLoanId id) { return !seen.insert(id).second; }),
          ids.end());
    }
  };
  uniqueEnds(result.after);
  uniqueEnds(result.thenEntry);
  uniqueEnds(result.elseEntry);
  uniqueEnds(result.switchArmEntry);
  return result;
}

} // namespace lang
