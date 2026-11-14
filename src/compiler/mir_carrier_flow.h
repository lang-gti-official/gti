#pragma once

#include "gti/mir.h"

#include <map>
#include <optional>
#include <vector>

namespace lang {

// A known empty set is ownerless; unknown must never be treated as empty.
// These are MIR loan groups, not runtime state or lifetime parameter names.
using MirCarriedLoans = std::optional<std::vector<MirLoanId>>;

struct MirCarrierFlow {
  // Dependencies read by each instruction, before any replacement commits.
  std::map<MirInstructionId, MirCarriedLoans> reads;
  // Dependencies retained by the destination after an Initialize/Assign.
  std::map<MirInstructionId, MirCarriedLoans> stores;
  std::map<MirBlockId, MirCarriedLoans> exits;
  // Unsupported provenance or retaining call effects prohibit using this
  // result to shorten a loan. Existing conservative liveness remains valid.
  bool complete = true;
};

// Forward fixed point over actual MIR control flow. Copies retain independent
// sets; exact direct local replacements kill the previous set; projected and
// indirect writes widen. This query does not admit a source store or certify
// raw storage: those require their separate type/storage contracts.
[[nodiscard]] MirCarrierFlow deriveMirCarrierFlow(const MirBody &body);

} // namespace lang
