#include "mir_loan_liveness.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
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
};

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
      if (binding != nullptr && binding->payloadPattern != nullptr)
        unconfined.insert(loan.semanticLoan);
      const auto declared = declaringStatement.find(carrier);
      if (loan.access == AccessMode::Mutable &&
          declared != declaringStatement.end())
        if (const HirStatement *statement =
                body.findStatement(declared->second);
            statement != nullptr && statement->value)
          if (const HirValue *initializer =
                  body.findValue(*statement->value);
              initializer != nullptr &&
              initializer->borrowOrigin == BorrowOriginKind::Global)
            unconfined.insert(loan.semanticLoan);
    }
    // A carrier attached from another region (or with no declaration of its
    // own) makes the loan's extent region-wide: it ends at the region exit.
    if (!loan.entry && creation && loan.carriers.size() > 1)
      for (const HirBindingId carrier : loan.carriers) {
        const auto declared = declaringStatement.find(carrier);
        if (declared == declaringStatement.end() ||
            !(tree.regionOf(declared->second) == tree.regionOf(*creation)))
          unconfined.insert(loan.semanticLoan);
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
  for (const HirLoan &loan : body.loans) {
    ++agreement.loans;
    const SemanticLoanId id = loan.semanticLoan;
    std::vector<HirStatementId> semanticAfter;
    bool entryEnd = false;
    for (const HirStatement &statement : body.statements) {
      if (contains(statement.endedLoans, id))
        semanticAfter.push_back(statement.id);
      if (contains(statement.thenEntryEndedLoans, id) ||
          contains(statement.elseEntryEndedLoans, id))
        entryEnd = true;
      for (const HirSwitchArm &arm : statement.switchArms)
        if (contains(arm.entryEndedLoans, id))
          entryEnd = true;
    }
    std::vector<HirStatementId> derivedAfter;
    for (const auto &[statement, loans] : derived.after)
      if (contains(loans, id))
        derivedAfter.push_back(statement);
    std::sort(semanticAfter.begin(), semanticAfter.end());
    std::sort(derivedAfter.begin(), derivedAfter.end());
    if (entryEnd || semanticAfter.size() > 1) {
      ++agreement.semanticEarlyPlan;
      continue;
    }
    if (semanticAfter == derivedAfter) {
      ++agreement.agree;
      continue;
    }
    ++agreement.differ;
    if (report) {
      if (loan.carriers.size() >= 5) {
        std::cerr << "TMPC";
        for (auto c : loan.carriers)
          for (const HirBinding &b : body.bindings)
            if (b.id == c)
              std::cerr << ' ' << (b.variable ? b.variable->name().lexeme : std::string(b.parameter ? "param" : "?"));
        std::cerr << '\n';
      }
      std::cerr << "loan-end-differ semantic=" << id
                << " kind=" << static_cast<int>(loan.kind)
                << " entry=" << loan.entry
                << " carriers=" << loan.carriers.size()
                << " parent=" << loan.parent << " semantic-after=[";
      const auto describe = [&](HirStatementId statement) {
        const HirStatement *found = body.findStatement(statement);
        std::cerr << statement << ':'
                  << (found ? static_cast<int>(found->kind) : -1) << ' ';
      };
      for (const auto statement : semanticAfter)
        describe(statement);
      std::cerr << "] derived-after=[";
      for (const auto statement : derivedAfter)
        describe(statement);
      if (const auto exit = derived.regionExit.find(id);
          exit != derived.regionExit.end()) {
        std::cerr << " region-exit-at=";
        describe(exit->second);
        const HirStatement *parentStatement = nullptr;
        for (const HirStatement &candidate : body.statements)
          for (const HirStatementId child : candidate.statements)
            if (child == exit->second)
              parentStatement = &candidate;
        std::cerr << " list-owner-kind="
                  << (parentStatement ? static_cast<int>(parentStatement->kind)
                                      : -1);
      }
      std::cerr << "]\n";
    }
  }
  return agreement;
}

} // namespace lang
