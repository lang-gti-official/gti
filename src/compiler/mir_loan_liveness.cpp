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
  std::function<bool(const HirStatement &, const HirLoan &,
                     const std::vector<HirStatementId> &, MirDerivedLoanEnds &)>
      conditionalPlan;
  std::function<bool(const HirStatement &, const HirLoan &,
                     const std::vector<HirStatementId> &, MirDerivedLoanEnds &)>
      switchPlan;
  std::function<void(const HirStatement &, const HirLoan &,
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
        // A carrier initialized by moving another carrier takes the loan over;
        // a transfer does not widen the loan's extent.
        if (declared != declaringStatement.end())
          if (const HirStatement *statement =
                  body.findStatement(declared->second);
              statement != nullptr && statement->value)
            if (const HirValue *initializer = body.findValue(*statement->value);
                initializer != nullptr &&
                initializer->kind == HirValueKind::Move)
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

  const auto within = [&](HirStatementId statement, HirStatementId root) {
    for (HirStatementId cursor = statement; cursor != 0;
         cursor = tree.parentOf(cursor))
      if (cursor == root)
        return true;
    return false;
  };
  std::function<bool(HirStatementId)> cannotFallThrough =
      [&](HirStatementId id) -> bool {
    const HirStatement *statement = body.findStatement(id);
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
        const HirStatement *statement = body.findStatement(id);
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
  const auto conflictsIn = [&](HirStatementId root, const HirLoan &loan) {
    std::vector<HirStatementId> statements;
    subtree(root, statements);
    std::vector<HirStatementId> conflicts;
    for (const HirStatementId statement : statements)
      if (conflictsWithLoan(body, tree, statement, loan.place.root,
                            loan.access))
        conflicts.push_back(statement);
    return conflicts;
  };
  // The if statements whose nearest enclosing if-arm is `arm`, in order.
  const auto nestedConditionals = [&](HirStatementId arm) {
    std::vector<HirStatementId> statements;
    subtree(arm, statements);
    std::vector<HirStatementId> nested;
    for (const HirStatementId statement : statements) {
      const HirStatement *candidate = body.findStatement(statement);
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
        const HirStatement *ancestor = body.findStatement(cursor);
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
      const HirStatement *candidate = body.findStatement(statement);
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
  std::function<bool(const HirStatement &, bool, const HirLoan &,
                     const std::vector<HirStatementId> &, std::vector<Point> &)>
      armEndpoints;
  std::function<bool(const HirStatement &, bool, const HirLoan &,
                     const std::vector<HirStatementId> &, std::vector<Point> &)>
      terminatingEndpoints = [&](const HirStatement &conditional,
                                 bool thenBranch, const HirLoan &loan,
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
      const HirStatement *inner = body.findStatement(nested);
      if (inner == nullptr ||
          !terminatingEndpoints(*inner, true, loan, own, points) ||
          !terminatingEndpoints(*inner, false, loan, own, points))
        return false;
    }
    return true;
  };
  armEndpoints = [&](const HirStatement &conditional, bool thenBranch,
                     const HirLoan &loan,
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
      const HirStatement *inner = body.findStatement(candidate);
      if (inner == nullptr ||
          !terminatingEndpoints(*inner, true, loan, own, points) ||
          !terminatingEndpoints(*inner, false, loan, own, points))
        return false;
    }
    if (!lastUseConditional) {
      const HirStatement *last = body.findStatement(*lastUse);
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
    const HirStatement *inner = body.findStatement(*lastUseConditional);
    return inner != nullptr && armEndpoints(*inner, true, loan, own, points) &&
           armEndpoints(*inner, false, loan, own, points);
  };
  // The innermost if-arm statement containing `statement`, if any, below
  // `top` (inclusive).
  const auto innermostArm = [&](HirStatementId statement) -> HirStatementId {
    for (HirStatementId cursor = statement; cursor != 0;) {
      const HirStatementId parent = tree.parentOf(cursor);
      const HirStatement *owner = body.findStatement(parent);
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
  conditionalPlan = [&](const HirStatement &conditional, const HirLoan &loan,
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
          const HirStatement *owner = body.findStatement(point.statement);
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
      target.push_back(loan.semanticLoan);
    }
    return true;
  };
  // A loan whose only use in a switch is its subject ends at every arm's
  // entry, and after the switch when no default arm exists.
  switchPlan = [&](const HirStatement &selection, const HirLoan &loan,
                   const std::vector<HirStatementId> &own,
                   MirDerivedLoanEnds &ends) -> bool {
    for (const HirStatementId use : own)
      if (use != selection.id && within(use, selection.id))
        return false;
    bool hasDefault = false;
    for (const HirSwitchArm &arm : selection.switchArms)
      for (const auto &label : arm.labels)
        if (label.isDefault)
          hasDefault = true;
    for (std::size_t arm = 0; arm < selection.switchArms.size(); ++arm)
      ends.switchArmEntry[{selection.id, arm}].push_back(loan.semanticLoan);
    if (!hasDefault)
      ends.after[selection.id].push_back(loan.semanticLoan);
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
      if (const HirStatement *owner = body.findStatement(parent)) {
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
    const HirStatement *owner = body.findStatement(parent);
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
      if (const HirStatement *owner = body.findStatement(cursor))
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
  breakPlans = [&](const HirStatement &target, const HirLoan &loan,
                   const std::vector<HirStatementId> &own,
                   MirDerivedLoanEnds &ends) {
    std::vector<HirStatementId> statements;
    subtree(target.id, statements);
    for (const HirStatementId conflict : statements) {
      if (conflict == target.id ||
          !conflictsWithLoan(body, tree, conflict, loan.place.root,
                             loan.access))
        continue;
      const auto list = siblingsOf(conflict);
      const auto at = std::find(list.begin(), list.end(), conflict);
      if (at == list.end() || std::next(at) == list.end())
        continue;
      const HirStatement *next = body.findStatement(*std::next(at));
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
        const HirStatement *last = body.findStatement(*lastUse);
        if (last == nullptr ||
            std::find(list.begin(), list.end(), *lastUse) == list.end() ||
            (last->kind != HirStatementKind::Variable &&
             last->kind != HirStatementKind::Expression))
          continue;
        ends.after[*lastUse].push_back(loan.semanticLoan);
        continue;
      }
      if (conflictPath.empty())
        continue;
      // The innermost conditional arm, else the innermost switch arm.
      std::optional<ArmStep> entry;
      for (auto step = conflictPath.rbegin(); step != conflictPath.rend();
           ++step)
        if (const HirStatement *owner = body.findStatement(step->statement);
            owner != nullptr && owner->kind == HirStatementKind::If) {
          entry = *step;
          break;
        }
      if (!entry)
        entry = conflictPath.back();
      const HirStatement *owner = body.findStatement(entry->statement);
      if (owner == nullptr)
        continue;
      if (owner->kind == HirStatementKind::If)
        (entry->arm == 0 ? ends.thenEntry : ends.elseEntry)[owner->id]
            .push_back(loan.semanticLoan);
      else
        ends.switchArmEntry[{owner->id, entry->arm}].push_back(
            loan.semanticLoan);
    }
  };

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
    const HirStatement *ending = body.findStatement(end);
    if (ending != nullptr && ending->kind == HirStatementKind::If &&
        conditionalPlan(*ending, loan, own, result))
      continue;
    if (ending != nullptr && ending->kind == HirStatementKind::Switch &&
        switchPlan(*ending, loan, own, result))
      continue;
    // A loan still live when the body completes is retired by the body's
    // exit (or escapes through a return), not after a statement. Nested
    // blocks retire it after their last statement like any other.
    if (ending == nullptr || tree.lastInList(end) ||
        ending->kind == HirStatementKind::Return ||
        ending->kind == HirStatementKind::Break ||
        ending->kind == HirStatementKind::Continue) {
      result.regionExit[loan.semanticLoan] = end;
      continue;
    }
    result.after[end].push_back(loan.semanticLoan);

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
  if (report && std::getenv("GTI_TMP_LOAN_TREE"))
    for (const HirStatement &st : body.statements) {
      std::cerr << "stmt " << st.id << " kind=" << static_cast<int>(st.kind)
                << " body=" << st.body.value_or(0)
                << " else=" << st.elseBranch.value_or(0) << " list=";
      for (auto c : st.statements)
        std::cerr << c << ',';
      std::cerr << "\n";
    }
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
