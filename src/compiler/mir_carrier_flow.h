#pragma once

#include "gti/mir.h"

#include <compare>
#include <map>
#include <optional>
#include <vector>

namespace lang {

// A known empty set is ownerless; unknown must never be treated as empty.
// These are MIR loan groups, not runtime state or lifetime parameter names.
using MirCarriedLoans = std::optional<std::vector<MirLoanId>>;

struct MirCarrierRoot {
  MirPlaceRootKind kind = MirPlaceRootKind::Binding;
  std::size_t identity = 0;
  auto operator<=>(const MirCarrierRoot &) const = default;
};

struct MirCarrierFlow {
  // Dependencies read by each instruction, before any replacement commits.
  std::map<MirInstructionId, MirCarriedLoans> reads;
  // Dependencies retained by the destination after an Initialize/Assign.
  std::map<MirInstructionId, MirCarriedLoans> stores;
  // Actual input and every possible destination of a retaining write. Unlike
  // stores, inputs do not include the destination's previous contents.
  std::map<MirInstructionId, MirCarriedLoans> storeInputs;
  std::map<MirInstructionId, std::vector<MirCarrierRoot>> storeTargets;
  // Preserve which input belongs to which actual destination. A call may
  // perform several independent stores; flattening them creates false
  // cross-destination lifetime dependencies.
  std::map<MirInstructionId, std::map<MirCarrierRoot, MirCarriedLoans>>
      storeInputsByTarget;
  std::map<MirBlockId, MirCarriedLoans> exits;
  // Unsupported provenance or retaining call effects prohibit using this
  // result to shorten a loan. Existing conservative liveness remains valid.
  bool complete = true;
  // False when an operation might write storage without a modeled transfer.
  // Unknown reads still prevent liveness precision, but a direct write can
  // describe its added dependencies without knowing the old receiver state.
  bool effectsComplete = true;
};

struct MirRetainingStoreEffect {
  // Receiver or formal binding, never a local variable or a nominal type.
  MirCarrierRoot destination;
  MirCarrierRoot source;
  auto operator<=>(const MirRetainingStoreEffect &) const = default;
};

struct MirRetainingStoreEffects {
  std::vector<MirRetainingStoreEffect> effects;
  bool complete = false;
};

struct MirConservativeRetention {
  std::vector<HirBindingId> carriers;
  bool unmappedStorage = false;
};

// Proven writes remain obligations even when another operation makes exact
// body flow incomplete. This only widens fallback liveness; it never grants
// source storage permission or treats unknown inputs as ownerless.
[[nodiscard]] std::map<MirLoanId, MirConservativeRetention>
conservativeMirRetentions(const MirCarrierFlow &flow);

// Reconstruct source-body effects from MIR writes and entry provenance.
// With a program, static helper calls compose through the same analysis;
// recursive cycles remain incomplete until a checked fixed point is available.
// This does not authorize a store: typed lifetime/storage contracts remain
// separate obligations. Unsupported calls and local escaping origins yield
// incomplete rather than an empty (non-retaining) summary.
[[nodiscard]] MirRetainingStoreEffects
deriveMirRetainingStoreEffects(const MirFunctionInstance &function,
                               const MirProgram *program = nullptr);

// Forward fixed point over actual MIR control flow. Copies retain independent
// sets; exact direct local replacements kill the previous set; projected and
// indirect writes widen. This query does not admit a source store or certify
// raw storage: those require their separate type/storage contracts.
[[nodiscard]] MirCarrierFlow
deriveMirCarrierFlow(const MirBody &body, const MirProgram *program = nullptr);

} // namespace lang
