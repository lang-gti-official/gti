#pragma once

#include "gti/hir.h"
#include "gti/mir.h"
#include "gti/mir_dominance.h"
#include "gti/optimization/alias.h"
#include "gti/optimization/effects.h"
#include "gti/optimization/loops.h"
#include "gti/optimization/report.h"
#include "gti/optimization/safety.h"
#include "gti/optimization/sccp.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lang {

enum class MirAnalysisKind : std::uint8_t {
  CfgReachability,
  Dominance,
  ValueUses,
  Effects,
  ScalarPromotionInputs,
  SafetyChecks,
  ClosedCallSafety,
  PlaceAliasEscape,
  Loops,
  SparseConstants,
  Count,
};

[[nodiscard]] std::string_view name(MirAnalysisKind kind);

struct MirCfgAnalysis {
  std::vector<std::vector<MirBlockId>> successors;
  std::vector<std::vector<MirBlockId>> predecessors;
  std::vector<bool> reachable;
  bool acyclic = false;

  friend bool operator==(const MirCfgAnalysis &,
                         const MirCfgAnalysis &) = default;
};

struct MirInstructionEffectFact {
  MirInstructionId instruction = 0;
  MirEffectTraits effects;

  friend bool operator==(const MirInstructionEffectFact &,
                         const MirInstructionEffectFact &) = default;
};

enum class MirScalarPromotionMissReason : std::uint8_t {
  NotMutableBinding,
  UnsupportedType,
  AliasedOrProjected,
  Loaned,
  OwningOrDropped,
  InitiallyAvailable,
  UnsupportedAccess,
  FailureSensitive,
  CyclicControlFlow,
  NoUsefulLoad,
};

[[nodiscard]] std::string_view name(MirScalarPromotionMissReason reason);

struct MirScalarPromotionCandidate {
  MirPlaceId place = 0;
  std::vector<MirInstructionId> writes;
  std::vector<MirInstructionId> loads;

  friend bool operator==(const MirScalarPromotionCandidate &,
                         const MirScalarPromotionCandidate &) = default;
};

struct MirScalarPromotionMiss {
  MirPlaceId place = 0;
  MirScalarPromotionMissReason reason =
      MirScalarPromotionMissReason::UnsupportedAccess;

  friend bool operator==(const MirScalarPromotionMiss &,
                         const MirScalarPromotionMiss &) = default;
};

struct MirScalarPromotionInputs {
  std::vector<MirScalarPromotionCandidate> candidates;
  std::vector<MirScalarPromotionMiss> misses;

  friend bool operator==(const MirScalarPromotionInputs &,
                         const MirScalarPromotionInputs &) = default;
};

class MirPreservedAnalyses {
public:
  [[nodiscard]] static MirPreservedAnalyses all();
  [[nodiscard]] static MirPreservedAnalyses none();
  [[nodiscard]] static MirPreservedAnalyses
  only(std::initializer_list<MirAnalysisKind> analyses);

  [[nodiscard]] bool preserves(MirAnalysisKind analysis) const;

private:
  std::uint32_t mask = 0;
};

// A snapshot-scoped analysis cache keyed by stable MirBodyAddress and a
// monotonically increasing revision. The first query binds the manager to one
// MirProgram identity; cross-snapshot queries fail closed. Program-wide
// analyses and every body-local result derived from them are invalidated by a
// semantic mutation to any body. A pass may preserve them only for a change,
// such as attaching verified metadata, that cannot affect their inputs. No
// interior MIR pointers survive invalidation.
class MirBodyAnalysisManager {
public:
  [[nodiscard]] std::size_t revision(MirBodyAddress address) const;

  [[nodiscard]] const MirCfgAnalysis *cfg(const MirProgram &program,
                                          MirBodyAddress address);
  [[nodiscard]] const MirDominanceInfo *dominance(const MirProgram &program,
                                                  MirBodyAddress address);
  [[nodiscard]] const std::vector<std::vector<MirValueUse>> *
  valueUses(const MirProgram &program, MirBodyAddress address);
  [[nodiscard]] const std::vector<MirInstructionEffectFact> *
  effects(const MirProgram &program, MirBodyAddress address);
  [[nodiscard]] const MirScalarPromotionInputs *
  scalarPromotionInputs(const MirProgram &program, MirBodyAddress address);
  [[nodiscard]] const MirSafetyAnalysis *safetyChecks(const MirProgram &program,
                                                      MirBodyAddress address);
  [[nodiscard]] const MirClosedCallSafetyAnalysis *
  closedCallSafety(const MirProgram &program);
  [[nodiscard]] const MirPlaceAliasEscapeAnalysis *
  placeAliasEscape(const MirProgram &program, MirBodyAddress address);
  [[nodiscard]] const MirLoopAnalysis *loops(const MirProgram &program,
                                             MirBodyAddress address);
  [[nodiscard]] const MirSccpAnalysis *
  sparseConstants(const MirProgram &program, MirBodyAddress address);

  // Increments the body's revision and drops every cached result not named by
  // the pass. An omitted/unknown preservation claim therefore invalidates all
  // analyses conservatively.
  [[nodiscard]] std::vector<std::string>
  invalidate(MirBodyAddress address, const MirPreservedAnalyses &preserved);

private:
  struct Entry {
    MirBodyAddress address;
    std::size_t revision = 0;
    std::optional<MirCfgAnalysis> cfg;
    std::optional<MirDominanceInfo> dominance;
    std::optional<std::vector<std::vector<MirValueUse>>> valueUses;
    std::optional<std::vector<MirInstructionEffectFact>> effects;
    std::optional<MirScalarPromotionInputs> scalarPromotionInputs;
    std::optional<MirSafetyAnalysis> safetyChecks;
    std::optional<MirPlaceAliasEscapeAnalysis> placeAliasEscape;
    std::optional<MirLoopAnalysis> loops;
    std::optional<MirSccpAnalysis> sparseConstants;
  };

  [[nodiscard]] Entry &entry(MirBodyAddress address);
  [[nodiscard]] const Entry *findEntry(MirBodyAddress address) const;
  [[nodiscard]] bool accepts(const MirProgram &program);

  const MirProgram *programIdentity = nullptr;
  std::size_t programRevision = 0;
  std::size_t closedCallSafetyRevision = 0;
  std::optional<MirClosedCallSafetyAnalysis> closedCallSafetyCache;
  std::deque<Entry> entries;
};

struct MirPassContext {
  const HirProgram &hir;
  MirProgram &program;
  TargetInfo target;
};

struct MirPassOutcome {
  OptimizationPassReport report;
  MirPreservedAnalyses preserved = MirPreservedAnalyses::none();
  // Iterative passes may invalidate between bounded rounds so subsequent
  // analysis queries observe the new body revision. The pipeline then avoids
  // applying the same invalidation a second time.
  bool analysesInvalidated = false;
  std::vector<MirTransformRecord> transformations;
  MirVerificationResult verification;
};

class MirPass {
public:
  MirPass() = default;
  MirPass(const MirPass &) = delete;
  MirPass &operator=(const MirPass &) = delete;
  virtual ~MirPass() = default;

  [[nodiscard]] virtual std::string_view name() const = 0;
  [[nodiscard]] virtual MirPassOutcome
  run(MirPassContext &context, MirBodyAnalysisManager &analyses) = 0;
};

} // namespace lang
