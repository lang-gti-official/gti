#pragma once

#include "gti/generic_constraint.h"
#include "gti/semantic_core.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

// A fact implied by a true predicate, not an alternative predicate evaluator.
// Opaque source predicates can filter concrete instances without this proof.
struct PredicateCapability {
  GenericConstraintKind atomic = GenericConstraintKind::None;
  StructuralConstraintKind structural = StructuralConstraintKind::None;
  std::vector<SemanticType> arguments;
};

// The trusted reflection query and generic-body proof transport share this
// mapping. Public wrapper names never establish a lifecycle or safety fact.
[[nodiscard]] inline std::optional<GenericConstraintKind>
metaTypeTraitConstraint(std::uint64_t selector) {
  switch (selector) {
  case 2:
    return GenericConstraintKind::Reflectable;
  case 3:
    return GenericConstraintKind::SignedIntegral;
  case 4:
    return GenericConstraintKind::CopyConstructible;
  case 5:
    return GenericConstraintKind::MoveConstructible;
  case 6:
    return GenericConstraintKind::Copyable;
  case 7:
    return GenericConstraintKind::Movable;
  case 8:
    return GenericConstraintKind::Relocatable;
  case 9:
    return GenericConstraintKind::BitwiseRelocatable;
  case 10:
    return GenericConstraintKind::Transferable;
  case 11:
    return GenericConstraintKind::Shareable;
  case 12:
    return GenericConstraintKind::DefaultInitializable;
  case 13:
    return GenericConstraintKind::IntegralEnum;
  case 14:
    return GenericConstraintKind::UnsignedIntegral;
  case 15:
    return GenericConstraintKind::Numeric;
  case 16:
    return GenericConstraintKind::SignedNumeric;
  case 17:
    return GenericConstraintKind::Integral;
  case 18:
    return GenericConstraintKind::FloatingPoint;
  case 19:
    return GenericConstraintKind::Boolean;
  case 20:
    return GenericConstraintKind::ObjectLayout;
  case 21:
    return GenericConstraintKind::BorrowFree;
  default:
    return std::nullopt;
  }
}

} // namespace lang
