#pragma once

#include "gti/frontend_work.h"
#include "gti/mir_dominance.h"
#include "gti/mir_verification_cache.h"

#include <optional>
#include <vector>

namespace lang::mir_detail {

// Explicitly scoped to one frontend invocation. The cache owns facts across
// revisions, while the counters belong to this invocation.
class VerificationSession final {
public:
  VerificationSession(MirVerificationCache *cache, FrontendWorkMetrics *work);
  void recordRoundBody(bool reused);
  bool enabled() const { return cache != nullptr; }
  VerificationSession(const VerificationSession &) = delete;
  VerificationSession &operator=(const VerificationSession &) = delete;

  std::optional<MirDominanceInfo>
  dominance(const std::vector<std::size_t> &key);
  void rememberDominance(std::vector<std::size_t> key,
                         const MirDominanceInfo &facts);
  bool knowsExclusiveBoundaries(const std::vector<std::size_t> &key);
  void rememberExclusiveBoundaries(std::vector<std::size_t> key);

private:
  MirVerificationCache *cache;
  FrontendWorkMetrics *work;
};
// Called after the body verifier validates block, expression and instruction
// identities and derives marker counts from the current executable body.
std::optional<std::pair<MirBlockId, MirInstructionId>>
conflictingExpressionBoundaries(const MirBody &body,
                                const std::vector<std::size_t> &markerCounts,
                                const MirDominanceInfo &dominance,
                                VerificationSession *reuse);
} // namespace lang::mir_detail
