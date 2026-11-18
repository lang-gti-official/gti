#pragma once

#include "gti/mir.h"

#include <compare>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace lang {

// Type-level capacity to carry dependencies, not a claim that this value
// currently has any. Expression-local ownerless facts may predate a store.
[[nodiscard]] bool mirTypeMayHoldDependencies(const SemanticType &type,
                                              const SemanticTypeTraits &traits);

// A known empty set is ownerless; unknown must never be treated as empty.
// These are MIR loan groups, not runtime state or lifetime parameter names.
using MirCarriedLoans = std::optional<std::vector<MirLoanId>>;

struct MirCarrierRoot {
  MirPlaceRootKind kind = MirPlaceRootKind::Binding;
  std::size_t identity = 0;
  // Concrete formal packs share a binding, but each element has independent
  // contents and a distinct caller argument. Ordinary roots leave this empty.
  std::optional<std::size_t> packElement = std::nullopt;
  auto operator<=>(const MirCarrierRoot &) const = default;
};

// Resolve a formal root against the expanded signature and source bindings.
// A projected pack root is never silently treated as the whole binding.
[[nodiscard]] std::optional<std::size_t> mirRetainingParameterIndex(
    const MirBody &body, const std::vector<SemanticType> &parameterTypes,
    const std::vector<HirBindingId> &parameterBindings, MirCarrierRoot root);

struct MirCarrierFlow {
  // Optional converged dependencies at each reachable block entry. These are
  // value provenance facts, not permission to use an inactive or expired group.
  std::map<MirBlockId, std::map<MirCarrierRoot, MirCarriedLoans>> entryRoots;
  // Dependencies read by each instruction, before any replacement commits.
  std::map<MirInstructionId, MirCarriedLoans> reads;
  // Carried values used before their producer or after EndBorrow on a path.
  // Absence of an expired group is not an independent permission proof.
  std::map<MirInstructionId, MirCarriedLoans> unavailableReads;
  // Receiver-only reads preserve the distinction from other call arguments
  // for direct operator calls without a separate CallInput instruction.
  std::map<MirInstructionId, MirCarriedLoans> receiverReads;
  std::map<MirInstructionId, MirCarriedLoans> unavailableReceiverReads;
  // Contents of a call-result loan's physical source after the call's
  // modeled writes. Earlier results never acquire later writes retroactively.
  // These facts do not certify the result's source contract or permission;
  // consumers must independently check that contract and effects completeness.
  std::map<MirInstructionId, MirCarriedLoans> resultSourceContents;
  std::map<MirInstructionId, MirCarriedLoans> unavailableResultSourceContents;
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
  // Actual source provenance at each modeled retaining call, keyed by the
  // callee's formal root. Preserve unknown and known-empty distinctly, even
  // when destination resolution prevents applying the write.
  std::map<MirInstructionId, std::map<MirCarrierRoot, MirCarriedLoans>>
      retainingInputs;
  // Dependencies of returned values, distinct from loans merely read at exit.
  std::map<MirBlockId, MirCarriedLoans> returned;
  // Independently reconstructed contents of whole-value copies through a
  // shared reference parameter, excluding the transient physical access loan.
  std::map<MirInstructionId, MirCarriedLoans> copiedCallResults;
  std::map<MirInstructionId, MirCarriedLoans> unavailableCopiedCallResults;
  std::map<MirBlockId, MirCarriedLoans> exits;
  std::map<MirBlockId, MirCarriedLoans> unavailableExits;
  // Unsupported provenance or retaining call effects prohibit using this
  // result to shorten a loan. Existing conservative liveness remains valid.
  bool complete = true;
  // False when an operation might write storage without a modeled transfer.
  // Unknown reads still prevent liveness precision, but a direct write can
  // describe its added dependencies without knowing the old receiver state.
  bool effectsComplete = true;
  // Operations whose retaining effects could not be reconstructed. Structural
  // control-flow failures can also make effects incomplete without an entry.
  std::vector<MirInstructionId> incompleteEffectInstructions;
};

struct MirRetainingStoreEffect {
  // Receiver or formal binding, never a local variable or a nominal type.
  MirCarrierRoot destination;
  MirCarrierRoot source;
  auto operator<=>(const MirRetainingStoreEffect &) const = default;
};

struct MirRetainingStoreEffects {
  // Known retention obligations survive incomplete effects. Without complete,
  // these can only widen lifetimes, never authorize stores or narrow results.
  std::vector<MirRetainingStoreEffect> effects;
  // Known formal/receiver contents forwarded by normal returns. Nullopt is
  // unknown, while an empty vector proves an independent returned value.
  std::optional<std::vector<MirCarrierRoot>> returnedOrigins;
  // Body-local origins for each retaining instruction, before flattening the
  // callable's summary. Only usable when complete is true. This distinguishes
  // a modeled store from another use of the same input (which may still need
  // an ownerless or escape contract); it is not source storage permission.
  std::map<MirInstructionId, std::vector<MirRetainingStoreEffect>> writeEffects;
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

// Reuse converged helper summaries while inspecting one immutable program.
// The program and its bodies must remain unchanged for this object's lifetime.
// A detached caller body may change between flow queries: its flow is always
// reconstructed, and only summaries of functions in the immutable program are
// cached. A changed program (including a mutated-MIR test) needs a new session;
// provisional recursive facts never escape a query.
// Forward fixed point over actual MIR control flow. Copies retain independent
// sets; exact direct local replacements kill the previous set; projected and
// indirect writes widen. This query does not admit a source store or certify
// raw storage: those require their separate type/storage contracts.
[[nodiscard]] MirCarrierFlow
deriveMirCarrierFlow(const MirBody &body, const MirProgram *program = nullptr,
                     bool includeBlockEntries = false);

class MirCarrierAnalysis {
public:
  explicit MirCarrierAnalysis(const MirProgram &program);
  // Starts from `donor`'s converged call summaries. Only for a program whose
  // function bodies equal the donor program's (a copy differing in entry
  // metadata, for example).
  MirCarrierAnalysis(const MirProgram &program,
                     const MirCarrierAnalysis &donor);
  // Starts from `donor`'s converged summaries of every function whose whole
  // summary input is unchanged in `program`: its body, and recursively every
  // function, constructor and cleanup body it reaches. Everything else is
  // recomputed. Function and class tables must otherwise agree (successive
  // lowerings of one HIR program).
  [[nodiscard]] static std::unique_ptr<MirCarrierAnalysis>
  reusingUnchanged(const MirProgram &program, const MirCarrierAnalysis &donor);
  ~MirCarrierAnalysis();
  [[nodiscard]] const MirProgram &program() const;
  [[nodiscard]] MirCarrierFlow flow(const MirBody &body,
                                    bool includeBlockEntries = false);
  [[nodiscard]] MirRetainingStoreEffects
  retainingStores(HirFunctionInstanceId function);

private:
  friend class MirCarrierAnalysisScope;
  friend std::uint64_t activeMirCarrierSession(const MirProgram &program);
  friend MirCarrierFlow deriveMirCarrierFlow(const MirBody &body,
                                             const MirProgram *program,
                                             bool includeBlockEntries);
  struct Impl;
  std::unique_ptr<Impl> implementation;
};

// While alive on this thread, deriveMirCarrierFlow over the analysis's
// program reuses its call summaries, and each flow of one of the program's
// own bodies is computed once. Only for a program that does not change while
// the scope is alive (whole-program verification); a body outside the
// program (an edited copy, for example) is always analysed afresh.
// The analysis a live MirCarrierAnalysisScope holds over `program`, if any.
[[nodiscard]] MirCarrierAnalysis *
activeMirCarrierAnalysis(const MirProgram &program);
// A process-unique identity of the live scope over `program` (0 if none).
// Queries over that immutable program may cache results under it.
[[nodiscard]] std::uint64_t activeMirCarrierSession(const MirProgram &program);

class MirCarrierAnalysisScope {
public:
  explicit MirCarrierAnalysisScope(MirCarrierAnalysis &analysis);
  ~MirCarrierAnalysisScope();
  MirCarrierAnalysisScope(const MirCarrierAnalysisScope &) = delete;
  MirCarrierAnalysisScope &operator=(const MirCarrierAnalysisScope &) = delete;

private:
  MirCarrierAnalysis *previous = nullptr;
};

// Reconstruct source-body effects from MIR writes and entry provenance.
// With a program, static helper calls compose through the same analysis;
// recursive helpers converge over finite formal-root effect relations.
// This does not authorize a store: typed lifetime/storage contracts remain
// separate obligations. Unsupported calls and local escaping origins yield
// incomplete rather than an empty (non-retaining) summary.
[[nodiscard]] MirRetainingStoreEffects
deriveMirRetainingStoreEffects(const MirFunctionInstance &function,
                               const MirProgram *program = nullptr);

} // namespace lang
