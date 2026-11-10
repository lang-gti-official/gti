#pragma once

#include "gti/hir.h"

#include <cstddef>
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

[[nodiscard]] MirDerivedLoanEnds deriveLoanEnds(const HirBody &body);

// Development comparison against the transported semantic end points. Returns
// the number of loans whose derived end differs and, when `report` is set,
// writes one line per disagreement to stderr.
struct MirLoanEndAgreement {
  std::size_t loans = 0;
  std::size_t agree = 0;
  std::size_t semanticEarlyPlan = 0;
  std::size_t differ = 0;
};
[[nodiscard]] MirLoanEndAgreement
compareLoanEnds(const HirBody &body, const MirDerivedLoanEnds &derived,
                bool report);

} // namespace lang
