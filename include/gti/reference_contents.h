#pragma once

#include "gti/borrow_conditions.h"

namespace lang {

// A phase supplies this direct, complete value-storage inventory from its
// authoritative instantiated type/layout. It stops at checked references.
struct ReferenceContentsField {
  BorrowPath fields;
  SemanticType reference = SemanticType::Unknown;
  std::vector<BorrowDependencyCondition> conditions;

  friend bool operator==(const ReferenceContentsField &,
                         const ReferenceContentsField &) = default;
};

struct ReferenceContentsInventory {
  std::vector<ReferenceContentsField> fields;
};

// Crossings are structural evidence, never permission to reborrow or detach
// an exclusive source. Formation and the requested access remain phase-owned.
struct ReferenceContentsCrossing {
  BorrowPath fields;
  SemanticType reference = SemanticType::Unknown;
};

struct ReferenceSupplierPath : ReferenceContentsField {
  std::vector<ReferenceContentsCrossing> crossings;
};

enum class ReferenceSupplierQueryStatus { Complete, Unknown, Cyclic, Exhausted };

struct ReferenceSupplierQuery {
  ReferenceSupplierQueryStatus status = ReferenceSupplierQueryStatus::Unknown;
  // Populated only for Complete. A failed traversal never publishes a prefix
  // as a complete inventory, including when its direct paths were known.
  std::vector<ReferenceSupplierPath> paths;
};

// First-stage acyclic composition. Recursive source types retain their direct
// storage inventory; publishing their transitive supplier closure requires a
// compact graph selector, not an unbounded list of structural paths. A repeated
// exact type is Cyclic; nonregular generic recursion exhausts the shared budget.
// Neither result means that no external references exist.
// The callback owns exact field types and guard identities. Complete describes
// the structural supplier query only: it supplies neither an active alternative
// set nor a checked contents group or permission to cross a reference.
template <typename Inventory>
[[nodiscard]] ReferenceSupplierQuery collectReferenceSupplierPaths(
    const SemanticType &root, Inventory &&inventory) {
  ReferenceSupplierQuery result{.status = ReferenceSupplierQueryStatus::Complete};
  BorrowInventoryBudget budget;
  std::vector<const SemanticType *> active;
  const auto fail = [&](ReferenceSupplierQueryStatus status) {
    result.status = status;
    result.paths.clear();
    return false;
  };
  // Charge nested type data before equality checks or copying path witnesses.
  // A nonregular generic recipe may grow its arguments without repeating a
  // nominal identity; a limit on reference crossings alone would not bound it.
  const auto chargeType = [&](const auto &self, const SemanticType &type,
                              std::size_t depth) -> bool {
    if (depth >= borrowInventoryDepthLimit ||
        !budget.consume(1 + type.valueArguments.size() + type.lifetimeArguments.size() +
                        type.quantifiedLifetimes.size() + type.lambdaEnclosingClassValues.size() +
                        type.lambdaEnclosingFunctionValues.size()))
      return false;
    for (const auto *children : {&type.arguments, &type.lambdaEnclosingClassTypes,
                                 &type.lambdaEnclosingFunctionTypes})
      for (const auto &child : *children)
        if (!self(self, child, depth + 1))
          return false;
    return true;
  };
  const auto visit = [&](const auto &self, const SemanticType &type,
                         BorrowPath prefix,
                         std::vector<BorrowDependencyCondition> conditions,
                         std::vector<ReferenceContentsCrossing> crossings) -> bool {
    if (active.size() >= borrowInventoryDepthLimit ||
        prefix.size() >= borrowInventoryDepthLimit || !chargeType(chargeType, type, 0))
      return fail(ReferenceSupplierQueryStatus::Exhausted);
    if (type.kind == SemanticType::Unknown)
      return fail(ReferenceSupplierQueryStatus::Unknown);
    if (std::any_of(active.begin(), active.end(), [&](const auto *ancestor) {
          return *ancestor == type;
        }))
      return fail(ReferenceSupplierQueryStatus::Cyclic);
    const auto direct = inventory(type);
    if (!direct)
      return fail(ReferenceSupplierQueryStatus::Unknown);
    active.push_back(&type);
    for (const auto &field : direct->fields) {
      if (!budget.consume(1 + field.fields.size() + field.conditions.size()) ||
          field.fields.size() + prefix.size() >= borrowInventoryDepthLimit ||
          !chargeType(chargeType, field.reference, 0))
        return fail(ReferenceSupplierQueryStatus::Exhausted);
      if (field.fields.empty() || !isWellFormedBorrowPath(field.fields) ||
          field.reference.kind != SemanticType::Reference ||
          field.reference.arguments.size() != 1 ||
          !field.reference.referenceLifetime.specified())
        return fail(ReferenceSupplierQueryStatus::Unknown);
      ReferenceSupplierPath path;
      path.fields = prefix;
      path.fields.insert(path.fields.end(), field.fields.begin(), field.fields.end());
      path.reference = field.reference;
      path.conditions = conditions;
      for (auto condition : field.conditions) {
        if (!budget.consume(condition.subobject.size()) ||
            condition.subobject.size() + prefix.size() >= borrowInventoryDepthLimit ||
            !chargeType(chargeType, condition.enumeration, 0))
          return fail(ReferenceSupplierQueryStatus::Exhausted);
        if (!isWellFormedBorrowPath(condition.subobject) ||
            condition.enumeration.kind != SemanticType::Enum)
          return fail(ReferenceSupplierQueryStatus::Unknown);
        condition.subobject.insert(condition.subobject.begin(), prefix.begin(), prefix.end());
        path.conditions.push_back(std::move(condition));
      }
      path.crossings = crossings;
      result.paths.push_back(path);
      path.crossings.push_back({.fields = path.fields, .reference = path.reference});
      if (!self(self, field.reference.arguments.front(), std::move(path.fields),
                std::move(path.conditions), std::move(path.crossings)))
        return false;
    }
    active.pop_back();
    return true;
  };
  (void)visit(visit, root, {}, {}, {});
  return result;
}

} // namespace lang
