#pragma once

#include "gti/hir.h"

#include <cstddef>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lang {

// Loan end points derived by MIR from a body's own statement tree and value
// uses. MIR is the single authority for body-local borrow validity (see
// docs/architecture/mir.md, "Body-local lifetime validity"); this derivation
// replaces the semantic end points that HIR used to transport.
struct MirDerivedLoanEnds {
  // Loans retired after a statement completes.
  std::unordered_map<HirStatementId, std::vector<SemanticLoanId>> after;
  // Loans retired on entry to an if statement's then/else branch.
  std::unordered_map<HirStatementId, std::vector<SemanticLoanId>> thenEntry;
  std::unordered_map<HirStatementId, std::vector<SemanticLoanId>> elseEntry;
  // Loans retired on entry to one switch arm.
  std::map<std::pair<HirStatementId, std::size_t>, std::vector<SemanticLoanId>>
      switchArmEntry;
  // Development: loans left to their region's exit, with the statement whose
  // completion would otherwise have ended them.
  std::unordered_map<SemanticLoanId, HirStatementId> regionExit;
};

// The loan facts liveness reads, independent of which layer recorded them.
struct LoanFacts {
  std::size_t id = 0;
  // A reference loan (as opposed to a stored carrier group).
  bool reference = false;
  std::vector<HirBindingId> carriers;
  std::size_t parent = 0;
  std::size_t storedDependency = 0;
  std::vector<std::size_t> dependencyParents;
  bool entry = false;
  bool lifecycleBound = false;
  AccessMode access = AccessMode::ReadOnly;
  // Root symbol of the borrowed place, for conflicting-write detection.
  SymbolId placeRoot = 0;
};

[[nodiscard]] MirDerivedLoanEnds
deriveLoanEnds(const HirBody &body, const std::vector<LoanFacts> &loans,
               const std::function<std::size_t(HirBindingId)> &retainedLoanOf);

} // namespace lang
